#include <loopz/sample_rates.hpp>
#include <loopz/macos/scene_renderer.hpp>
#include <loopz/timeline.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/causal_resampler.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace loopz::macos {
namespace {
bool online(const DeviceAudioView& view, const RenderClock& clock, bool capture) noexcept {
    auto& header = view.header();
    const auto now = clock.nowNs.load();
    const auto heartbeat =
        capture ? header.helperHeartbeatNs.load() : header.driverHeartbeatNs.load();
    return !header.retired.load() &&
           (capture ? header.helperOnline.load() : header.driverOnline.load()) && heartbeat &&
           (now >= heartbeat ? now - heartbeat < kDeviceAudioHeartbeatTimeoutNs
                             : heartbeat - now <= kDeviceAudioConcurrentHeartbeatSkewNs);
}
bool safeFrame(uint64_t first, uint32_t frames, uint32_t rate) noexcept {
    return rate >= 8000 && rate <= 192000 && first <= (UINT64_MAX / 1000000000ULL) * rate - frames;
}
uint64_t holdDeadline(uint64_t now, uint64_t duration, bool pending) noexcept {
    const auto deadline = now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
    return (deadline & ~uint64_t{1}) | uint64_t(pending);
}
} // namespace
struct TransportAudioSink::Impl {
    std::shared_ptr<DeviceAudioRegion> region;
    std::shared_ptr<const RenderClock> clock;
    const DeviceAudioView view;
    CausalResampler converter;
    // Low bit distinguishes a pending native transaction from its bounded
    // completion grace. One atomic keeps render/control observations coherent.
    std::atomic<uint64_t> configurationHold{0};
    Impl(std::shared_ptr<DeviceAudioRegion> mapping, std::shared_ptr<const RenderClock> time,
         uint32_t renderRate)
        : region(std::move(mapping)), clock(std::move(time)), view(region->view()),
          converter(view.channels(), renderRate, view.sampleRate()) {
        if (!clock)
            throw std::invalid_argument("A transport sink needs a render clock");
    }
};
TransportAudioSink::TransportAudioSink(std::shared_ptr<DeviceAudioRegion> region,
                                       std::shared_ptr<const RenderClock> clock,
                                       uint32_t renderRate) {
    if (!region)
        throw std::invalid_argument("A transport sink needs a mapping");
    impl_ = std::make_unique<Impl>(std::move(region), std::move(clock), renderRate);
}
uint32_t TransportAudioSink::delayFrames(uint32_t renderRate, uint32_t endpointRate) noexcept {
    return CausalResampler::delayFrames(renderRate, endpointRate);
}
TransportAudioSink::~TransportAudioSink() = default;
uint32_t TransportAudioSink::channels() const noexcept {
    return impl_->view.channels();
}
bool TransportAudioSink::requested() const noexcept {
    auto& self = *impl_;
    if (self.view.header().retired.load())
        return false;
    const auto hold = self.configurationHold.load();
    if (online(self.view, *self.clock, false))
        return true;
    // A first resume does not settle the layout: HAL can pause IO again
    // during the same approval. Keep the bounded grace through those cycles.
    return hold && self.clock->nowNs.load() < (hold & ~uint64_t{1});
}
bool TransportAudioSink::beginConfigurationHold() noexcept {
    auto& self = *impl_;
    if (!online(self.view, *self.clock, false))
        return false;
    self.configurationHold.store(holdDeadline(self.clock->nowNs.load(), 10000000000ULL, true));
    return true;
}
void TransportAudioSink::endConfigurationHold() noexcept {
    auto& self = *impl_;
    auto hold = self.configurationHold.load();
    if (hold & 1) {
        const auto now = self.clock->nowNs.load();
        self.configurationHold.compare_exchange_strong(
            hold, now < (hold & ~uint64_t{1}) ? holdDeadline(now, 500000000ULL, false) : 0);
    }
}
void TransportAudioSink::idle() noexcept {
    impl_->view.header().helperOnline.store(0);
    impl_->converter.reset();
}
bool TransportAudioSink::publish(uint64_t firstFrame, uint32_t renderRate, RoutingInput input,
                                 uint32_t frames) noexcept {
    auto& self = *impl_;
    if (self.view.header().retired.load() || !frames || frames > kBlock ||
        input.capacityFrames < frames || input.channels.size() != channels() ||
        !safeFrame(firstFrame, frames, renderRate) ||
        std::any_of(
            input.channels.begin(), input.channels.end(),
            [](auto* plane) {
                return !plane;
            })) {
        idle();
        return false;
    }
    if (renderRate != self.converter.sourceRate()) {
        idle();
        return false;
    }
    const auto block = self.converter.convert(firstFrame, input, frames);
    if (!block.valid || (block.frames && !self.view.writeCapture(
                                             block.firstFrame, block.interleaved, block.frames))) {
        idle();
        return false;
    }
    self.view.header().helperHeartbeatNs.store(self.clock->nowNs.load());
    self.view.header().helperOnline.store(1);
    return true;
}
struct TransportAudioSource::Impl {
    std::shared_ptr<DeviceAudioRegion> region;
    std::shared_ptr<const RenderClock> clock;
    const DeviceAudioView view;
    const bool capture;
    const AudioResampler resampler;
    struct ReadContext {
        const Impl& owner;
        uint64_t nowNs;
        static ResampleFrame frame(const void* context, uint64_t position,
                                   std::span<float> destination) noexcept {
            const auto& self = *static_cast<const ReadContext*>(context);
            const auto& owner = self.owner;
            std::array<float, kMaximumRoutingChannels> samples{};
            const auto buffer = std::span<float>(samples.data(), owner.view.channels());
            const auto read = owner.capture
                                  ? owner.view.readCapture(position, buffer, 1, self.nowNs,
                                                           kDeviceAudioConcurrentHeartbeatSkewNs)
                                  : owner.view.readPlayback(position, buffer, 1, self.nowNs,
                                                            kDeviceAudioConcurrentHeartbeatSkewNs);
            std::copy_n(samples.data(), destination.size(), destination.data());
            return {read.validBlock && !read.missingFrames, owner.view.channels()};
        }
    };
    Impl(std::shared_ptr<DeviceAudioRegion> mapping, std::shared_ptr<const RenderClock> time,
         TransportDirection direction)
        : region(std::move(mapping)), clock(std::move(time)), view(region->view()),
          capture(direction == TransportDirection::Capture) {
        if (!clock)
            throw std::invalid_argument("A transport source needs a render clock");
    }
};
TransportAudioSource::TransportAudioSource(std::shared_ptr<DeviceAudioRegion> region,
                                           std::shared_ptr<const RenderClock> clock,
                                           TransportDirection direction) {
    if (!region)
        throw std::invalid_argument("A transport source needs a mapping");
    impl_ = std::make_unique<Impl>(std::move(region), std::move(clock), direction);
}
TransportAudioSource::~TransportAudioSource() = default;
uint32_t TransportAudioSource::channels() const noexcept {
    return impl_->view.channels();
}
AudioReadResult TransportAudioSource::read(uint64_t firstHostNs, double rate, RoutingOutput output,
                                           uint32_t frames,
                                           AudioReadCursor& cursor) const noexcept {
    const auto& self = *impl_;
    const auto sourceChannels = self.view.channels();
    const bool shape = frames && frames <= kBlock && frames <= output.capacityFrames &&
                       !output.channels.empty() && output.channels.size() <= sourceChannels &&
                       std::all_of(output.channels.begin(), output.channels.end(), [](auto* plane) {
                           return plane;
                       });
    const auto clear = [&] {
        for (auto* plane : output.channels)
            if (plane)
                std::fill_n(plane, std::min(frames, output.capacityFrames), 0);
    };
    if (!shape || !std::isfinite(rate) || rate < 8000 || rate > 192000 ||
        firstHostNs > UINT64_MAX - 1000000000ULL || !online(self.view, *self.clock, self.capture)) {
        clear();
        return {frames, false};
    }
    const auto sourceRate = self.view.sampleRate();
    const auto whole = (firstHostNs / 1000000000ULL) * sourceRate;
    const long double offset =
        static_cast<long double>(firstHostNs % 1000000000ULL) * sourceRate / 1e9L;
    const Impl::ReadContext context{self, self.clock->nowNs.load()};
    const auto integral = std::floor(offset);
    const auto fraction = static_cast<double>(offset - integral);
    uint32_t missing = 0;
    if (rate == sourceRate && (fraction < .0001999 || fraction > .9998001)) {
        // Stay inside AudioResampler's aligned cutoff: adding frames can change
        // rounding near a floating-point exponent boundary. The 1e-7 margin
        // exceeds that error over the supported <=192 kHz, <=256-frame range.
        // Near-cutoff positions retain the reference reader below. Batch
        // transport validation/counters instead of repeating them per frame;
        // each frame still retains its atomic publication-tag validation.
        const auto first = whole + static_cast<uint64_t>(integral) + (fraction > .9998);
        constexpr uint32_t tileFrames = 32;
        std::array<float, tileFrames * kDeviceAudioMaximumChannels> samples;
        for (uint32_t begin = 0; begin < frames; begin += tileFrames) {
            const auto count = std::min(tileFrames, frames - begin);
            const auto interleaved = std::span<float>(samples.data(), count * sourceChannels);
            const auto read =
                self.capture
                    ? self.view.readCapture(first + begin, interleaved, count, context.nowNs,
                                            kDeviceAudioConcurrentHeartbeatSkewNs)
                    : self.view.readPlayback(first + begin, interleaved, count, context.nowNs,
                                             kDeviceAudioConcurrentHeartbeatSkewNs);
            missing += read.missingFrames;
            for (size_t channel = 0; channel < output.channels.size(); ++channel)
                for (uint32_t frame = 0; frame < count; ++frame) {
                    const auto value = samples[frame * sourceChannels + channel];
                    output.channels[channel][begin + frame] = std::isfinite(value) ? value : 0;
                }
        }
    } else {
        missing = self.resampler.read({whole, offset, double(sourceRate), rate},
                                      {output.channels, output.capacityFrames}, frames,
                                      Impl::ReadContext::frame, &context);
    }
    if (!online(self.view, *self.clock, self.capture)) {
        clear();
        return {frames, false};
    }
    cursor.hostNs = firstHostNs;
    cursor.rate = rate;
    cursor.frames = frames;
    cursor.generation = self.view.generation();
    return {missing, true};
}
struct SceneRenderer::Impl {
    struct SinkEntry {
        std::array<uint64_t, 2> identity;
        std::weak_ptr<TransportAudioSink> sink;
    };
    struct Prepared final : PreparedGraphActivation {
        struct HeldSink {
            std::shared_ptr<TransportAudioSink> sink;
            bool held{false};
        };
        Impl& owner;
        std::unique_ptr<RealtimePublication<AudioScene>::Update> update;
        AudioScene* next;
        std::vector<HeldSink> retained;
        bool committed{false};
        Prepared(Impl& state, std::unique_ptr<RealtimePublication<AudioScene>::Update> next,
                 AudioScene* scene, std::vector<HeldSink> sinks)
            : owner(state), update(std::move(next)), next(scene), retained(std::move(sinks)) {
            owner.staged = scene;
            // All preparation/allocation succeeded before changing demand on
            // any committed sink. Failed or rejected preparation keeps its DSP.
            for (auto& entry : retained)
                entry.held = entry.sink->beginConfigurationHold();
        }
        ~Prepared() override {
            if (!committed) {
                if (owner.staged == next)
                    owner.staged = nullptr;
                releaseHolds();
            }
        }
        void releaseHolds() noexcept {
            for (auto& entry : retained)
                if (entry.held) {
                    entry.sink->endConfigurationHold();
                    entry.held = false;
                }
        }
        void commit() noexcept override {
            if (committed)
                return;
            next->activate();
            update->commit();
            owner.staged = nullptr;
            releaseHolds();
            committed = true;
        }
    };
    struct Planned final : GraphActivationPlan {
        SceneRenderer& owner;
        const RouteDocument document;
        const std::vector<DevicePassThroughDelay> floors;
        const std::vector<DeviceInputDelay> inputs;
        const std::vector<uint32_t> latencies;
        bool attempted{false};
        Planned(SceneRenderer& renderer, const RouteDocument& desired,
                std::vector<DevicePassThroughDelay> passThrough,
                std::vector<DeviceInputDelay> sources, std::vector<uint32_t> endpoints)
            : owner(renderer), document(desired), floors(std::move(passThrough)),
              inputs(std::move(sources)), latencies(std::move(endpoints)) {}
        std::span<const uint32_t> inputLatencyFrames() const noexcept override {
            return latencies;
        }
        std::unique_ptr<PreparedGraphActivation>
        prepare(std::span<const DeviceTransport> devices) override {
            if (std::exchange(attempted, true))
                throw std::logic_error("A graph timing plan can only prepare once");
            return owner.prepareWithTiming(document, devices, floors, inputs);
        }
    };
    const SceneTiming timing;
    TimingFloors timingFloors;
    InputTiming inputTiming;
    const AudioScene::SourceResolver external;
    const SinkDecorator decorator;
    SourceRegistry* sources{nullptr};
    AudioScene* staged{nullptr};
    std::atomic<bool> editorPreview{false};
    std::shared_ptr<RenderClock> clock = std::make_shared<RenderClock>();
    std::shared_ptr<SceneHistoryStore> histories = std::make_shared<SceneHistoryStore>();
    RealtimePublication<AudioScene> scenes;
    std::vector<SinkEntry> sinks;
    Impl(SceneTiming time, AudioScene::SourceResolver source, SinkDecorator sink, size_t limit)
        : timing(time), external(std::move(source)), decorator(std::move(sink)), scenes(limit) {}
};
SceneRenderer::SceneRenderer(SceneTiming timing, AudioScene::SourceResolver externalSources,
                             SinkDecorator sinks, size_t retirementLimit)
    : impl_(std::make_unique<Impl>(timing, std::move(externalSources), std::move(sinks),
                                   retirementLimit)) {}
SceneRenderer::~SceneRenderer() = default;
SceneRenderer::SceneRenderer(SceneTiming timing, SourceRegistry& sources, SinkDecorator sinks,
                             size_t retirementLimit)
    : SceneRenderer(
          timing,
          [&sources](const auto& device, const auto& binding, const auto& route) {
              return sources.resolve(device, binding, route);
          },
          std::move(sinks), retirementLimit) {
    impl_->sources = &sources;
}
namespace {
std::vector<uint32_t> endpointLatencies(const RouteDocument& document, SceneTiming timing,
                                        std::span<const DevicePassThroughDelay> floors,
                                        std::span<const DeviceInputDelay> inputs) {
    auto plan = planSceneTiming(document, timing, floors, inputs);
    for (size_t index = 0; index < document.devices.size(); ++index) {
        auto& latency = plan.latencyFrames[index];
        const auto& definition = document.devices[index];
        if (!definition.routing.enabled)
            continue;
        if (!supportedVirtualSampleRate(definition.sampleRate))
            throw std::invalid_argument("Unsupported scene endpoint sample rate");
        const uint64_t numerator =
            uint64_t(latency +
                     TransportAudioSink::delayFrames(
                         timing.renderRate, static_cast<uint32_t>(definition.sampleRate))) *
            static_cast<uint32_t>(definition.sampleRate);
        latency = static_cast<uint32_t>((numerator + timing.renderRate - 1) / timing.renderRate);
        if (latency > kMaximumInputLatencyFrames)
            throw std::invalid_argument("Scene input latency exceeds transport limit");
    }
    return std::move(plan.latencyFrames);
}

} // namespace
std::vector<uint32_t> SceneRenderer::inputLatencyFrames(const RouteDocument& document) const {
    const auto floors =
        impl_->timingFloors ? impl_->timingFloors(document) : std::vector<DevicePassThroughDelay>{};
    const auto inputs =
        impl_->inputTiming ? impl_->inputTiming(document) : std::vector<DeviceInputDelay>{};
    return endpointLatencies(document, impl_->timing, floors, inputs);
}
std::unique_ptr<GraphActivationPlan> SceneRenderer::plan(const RouteDocument& document) {
    auto floors =
        impl_->timingFloors ? impl_->timingFloors(document) : std::vector<DevicePassThroughDelay>{};
    auto inputs =
        impl_->inputTiming ? impl_->inputTiming(document) : std::vector<DeviceInputDelay>{};
    auto latencies = endpointLatencies(document, impl_->timing, floors, inputs);
    return std::make_unique<Impl::Planned>(*this, document, std::move(floors), std::move(inputs),
                                           std::move(latencies));
}

std::unique_ptr<PreparedGraphActivation>
SceneRenderer::prepare(const RouteDocument& document, std::span<const DeviceTransport> devices) {
    auto& self = *impl_;
    const auto floors =
        self.timingFloors ? self.timingFloors(document) : std::vector<DevicePassThroughDelay>{};
    const auto inputs =
        self.inputTiming ? self.inputTiming(document) : std::vector<DeviceInputDelay>{};
    return prepareWithTiming(document, devices, floors, inputs);
}
std::unique_ptr<PreparedGraphActivation> SceneRenderer::prepareWithTiming(
    const RouteDocument& document, std::span<const DeviceTransport> devices,
    std::span<const DevicePassThroughDelay> floors, std::span<const DeviceInputDelay> inputs) {
    auto& self = *impl_;
    const auto latencies = endpointLatencies(document, self.timing, floors, inputs);
    for (size_t index = 0; index < document.devices.size(); ++index) {
        const auto& definition = document.devices[index];
        if (!definition.routing.enabled)
            continue;
        const auto transport = std::find_if(devices.begin(), devices.end(), [&](const auto& item) {
            return item.id == definition.id;
        });
        if (transport == devices.end() || transport->inputLatencyFrames != latencies[index])
            throw std::invalid_argument("Scene transport latency does not match its timing plan");
    }
    self.scenes.checkStageCapacity();
    auto active = self.scenes.borrow();
    std::vector<Impl::Prepared::HeldSink> retained;
    if (self.sources)
        self.sources->prepare(document);
    std::erase_if(self.sinks, [](const auto& entry) {
        return entry.sink.expired();
    });
    const auto mapping = [&](const std::string& id) -> const DeviceTransport& {
        const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& value) {
            return value.id == id;
        });
        if (found == devices.end())
            throw std::invalid_argument("Enabled scene device has no transport");
        return *found;
    };
    auto scene = std::make_unique<AudioScene>(
        document, self.timing,
        [&](const auto& device, const auto& binding, const auto& route) -> ResolvedAudioSource {
            if (binding.kind == SourceKind::PassThrough)
                return {std::make_shared<TransportAudioSource>(
                            mapping(device.id).region, self.clock, TransportDirection::Playback),
                        {}};
            return self.external ? self.external(device, binding, route) : ResolvedAudioSource{};
        },
        [&](const auto& device) -> std::shared_ptr<AudioSink> {
            const auto& transport = mapping(device.id);
            const auto identity = transport.region->view().mappingIdentity();
            std::shared_ptr<TransportAudioSink> sink;
            for (const auto& entry : self.sinks)
                if (entry.identity == identity) {
                    sink = entry.sink.lock();
                    break;
                }
            if (!sink) {
                sink = std::make_shared<TransportAudioSink>(transport.region, self.clock,
                                                            self.timing.renderRate);
                self.sinks.push_back({identity, sink});
            } else if (active) {
                const auto& previous = active->document().devices;
                const auto old =
                    std::find_if(previous.begin(), previous.end(), [&](const auto& item) {
                        return item.id == device.id;
                    });
                if (old != previous.end() && old->routing.enabled &&
                    old->routing.outputChannels == device.routing.outputChannels &&
                    old->sampleRate == device.sampleRate &&
                    hasPlaybackEndpoint(*old) != transport.hasOutput)
                    retained.push_back({sink});
            }
            return self.decorator ? self.decorator(device, sink) : sink;
        },
        self.histories, floors, inputs);
    auto* next = scene.get();
    return std::make_unique<Impl::Prepared>(self, self.scenes.stage(std::move(scene)), next,
                                            std::move(retained));
}
void SceneRenderer::setTimingFloors(TimingFloors floors) {
    impl_->timingFloors = std::move(floors);
}
void SceneRenderer::setInputTiming(InputTiming timing) {
    impl_->inputTiming = std::move(timing);
}
SceneRenderResult SceneRenderer::render(uint64_t firstFrame, uint32_t frames,
                                        uint64_t nowNs) noexcept {
    impl_->clock->nowNs.store(nowNs);
    auto scene = impl_->scenes.borrow();
    return scene ? scene->render(firstFrame, frames, impl_->editorPreview.load() ? UINT32_MAX : 0)
                 : SceneRenderResult{false, 0, 0, 0};
}
SceneRenderer::Observation SceneRenderer::observe() noexcept {
    return impl_->scenes.borrow();
}
void SceneRenderer::collect() noexcept {
    impl_->scenes.collect();
}
void SceneRenderer::pollSources(uint64_t nowMilliseconds) {
    if (impl_->sources) {
        auto scene = observe();
        const auto preview = impl_->editorPreview.load() ? UINT32_MAX : 0;
        uint32_t demand = scene ? scene->requestedDevices() : 0;
        if (scene && impl_->staged) {
            const auto stagedDemand = impl_->staged->requestedDevices();
            const auto& staged = impl_->staged->document().devices;
            const auto& current = scene->document().devices;
            for (size_t index = 0; index < staged.size(); ++index) {
                if (!(stagedDemand & (uint32_t{1} << index)))
                    continue;
                for (size_t active = 0; active < current.size(); ++active)
                    if (current[active].id == staged[index].id)
                        demand |= uint32_t{1} << active;
            }
        }
        impl_->sources->poll(scene.get(), nowMilliseconds, demand, preview);
    }
    collect();
}
void SceneRenderer::setEditorPreview(bool enabled) noexcept {
    impl_->editorPreview.store(enabled);
}
} // namespace loopz::macos
