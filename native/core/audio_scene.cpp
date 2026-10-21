#include <loopz/audio_scene.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace loopz {
namespace {
struct PlanarBlock {
    std::vector<float> samples;
    std::array<float*, kMaximumRoutingChannels> writable{};
    std::array<const float*, kMaximumRoutingChannels> readable{};
    uint32_t channels, capacity;
    PlanarBlock(uint32_t count, uint32_t frames)
        : samples(static_cast<size_t>(count) * frames), channels(count), capacity(frames) {
        for (uint32_t channel = 0; channel < count; ++channel) {
            writable[channel] = samples.data() + channel * frames;
            readable[channel] = writable[channel];
        }
    }
    RoutingInput input() const noexcept {
        return {std::span<const float* const>(readable.data(), channels), capacity};
    }
    RoutingOutput output() noexcept {
        return {std::span<float* const>(writable.data(), channels), capacity};
    }
    void clear(uint32_t frames) noexcept {
        for (uint32_t channel = 0; channel < channels; ++channel)
            std::fill_n(writable[channel], frames, 0.0f);
    }
    void sanitize(uint32_t frames) noexcept {
        for (uint32_t channel = 0; channel < channels; ++channel)
            for (uint32_t frame = 0; frame < frames; ++frame)
                if (!std::isfinite(writable[channel][frame]))
                    writable[channel][frame] = 0;
    }
};
constexpr size_t kNoDevice = std::numeric_limits<size_t>::max();

// The serial render owner alone accesses samples and interval bounds. Fixed
// capacity covers every supported compensation delay and render quantum.
class RenderHistory {
  public:
    explicit RenderHistory(uint32_t channels)
        : channels_(channels), samples_(static_cast<size_t>(channels) * capacity_) {}
    void reset() noexcept {
        begin_ = end_ = 0;
    }
    void write(uint64_t first, RoutingInput input, uint32_t frames) noexcept {
        if (first != end_)
            begin_ = first;
        end_ = first + frames;
        if (end_ - begin_ > capacity_)
            begin_ = end_ - capacity_;
        const auto offset = static_cast<uint32_t>(first % capacity_);
        const auto tail = std::min(frames, capacity_ - offset);
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            auto* plane = samples_.data() + channel * capacity_;
            std::copy_n(input.channels[channel], tail, plane + offset);
            std::copy_n(input.channels[channel] + tail, frames - tail, plane);
        }
    }
    uint32_t read(uint64_t first, RoutingOutput output, uint32_t frames) const noexcept {
        const auto start = std::max(first, begin_);
        const auto end = std::min(first + frames, end_);
        if (start >= end)
            return frames;
        const auto count = static_cast<uint32_t>(end - start);
        const auto prefix = static_cast<uint32_t>(start - first);
        const auto offset = static_cast<uint32_t>(start % capacity_);
        const auto tail = std::min(count, capacity_ - offset);
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            const auto* plane = samples_.data() + channel * capacity_;
            std::copy_n(plane + offset, tail, output.channels[channel] + prefix);
            std::copy_n(plane, count - tail, output.channels[channel] + prefix + tail);
        }
        return frames - count;
    }

  private:
    static constexpr uint32_t capacity_ = kCapacity / 2 + kBlock;
    const uint32_t channels_;
    std::vector<float> samples_;
    uint64_t begin_{0}, end_{0};
};
RoutingOutput suffix(PlanarBlock& block, uint32_t prefix,
                     std::array<float*, kMaximumRoutingChannels>& planes) noexcept {
    if (!prefix)
        return block.output();
    for (uint32_t channel = 0; channel < block.channels; ++channel)
        planes[channel] = block.writable[channel] + prefix;
    return {{planes.data(), block.channels}, block.capacity - prefix};
}
} // namespace

struct SceneHistoryStore::Impl {
    struct Entry {
        std::string device;
        uint32_t channels, rate, latency;
        std::weak_ptr<RenderHistory> history;
    };
    std::vector<Entry> entries;
    std::shared_ptr<RenderHistory> obtain(const std::string& device, uint32_t channels,
                                          uint32_t rate, uint32_t latency) {
        std::erase_if(entries, [](const auto& entry) {
            return entry.history.expired();
        });
        for (const auto& entry : entries)
            if (entry.device == device && entry.channels == channels && entry.rate == rate &&
                entry.latency == latency)
                if (auto history = entry.history.lock())
                    return history;
        auto history = std::make_shared<RenderHistory>(channels);
        entries.push_back({device, channels, rate, latency, history});
        return history;
    }
};
SceneHistoryStore::SceneHistoryStore() : impl_(std::make_unique<Impl>()) {}
SceneHistoryStore::~SceneHistoryStore() = default;

SceneTimingPlan planSceneTiming(const RouteDocument& document, SceneTiming timing,
                                std::span<const DevicePassThroughDelay> passThroughFloors,
                                std::span<const DeviceInputDelay> inputDelays) {
    validateRouteDocument(document);
    if (timing.renderRate < 8000 || timing.renderRate > 192000 ||
        (timing.blockFrames != 32 && timing.blockFrames != 64 && timing.blockFrames != 128 &&
         timing.blockFrames != kBlock) ||
        timing.sourceDelayFrames > kCapacity / 2 || timing.passThroughDelayFrames > kCapacity / 2)
        throw std::invalid_argument("Invalid scene render timing");
    SceneTimingPlan result;
    std::unordered_set<std::string> floorDevices;
    for (const auto& floor : passThroughFloors) {
        if (floor.frames > kCapacity / 2 || !floorDevices.insert(floor.device).second ||
            std::none_of(document.devices.begin(), document.devices.end(), [&](const auto& device) {
                return device.id == floor.device;
            }))
            throw std::invalid_argument("Invalid or duplicate device pass-through timing floor");
    }
    result.latencyFrames.resize(document.devices.size());
    std::set<std::pair<std::string, std::string>> inputKeys;
    for (const auto& delay : inputDelays) {
        const auto device =
            std::find_if(document.devices.begin(), document.devices.end(), [&](const auto& item) {
                return item.id == delay.device;
            });
        if (delay.frames > kCapacity / 2 ||
            (delay.filter && *delay.filter != ResamplingFilter::LinearPhase &&
             *delay.filter != ResamplingFilter::MinimumPhase) ||
            device == document.devices.end() ||
            !inputKeys.emplace(delay.device, delay.source).second)
            throw std::invalid_argument("Invalid or duplicate input timing budget");
        const auto source =
            std::find_if(device->bindings.begin(), device->bindings.end(), [&](const auto& item) {
                return item.id == delay.source;
            });
        if (source == device->bindings.end() ||
            (source->kind != SourceKind::InputDevice && source->kind != SourceKind::Application) ||
            (delay.filter && source->kind != SourceKind::InputDevice))
            throw std::invalid_argument("Input timing budget requires a capture binding; filter "
                                        "selection is physical-only");
    }
    std::unordered_map<std::string, size_t> indices;
    for (size_t index = 0; index < document.devices.size(); ++index)
        indices.emplace(document.devices[index].id, index);
    std::vector<bool> visited(document.devices.size(), false);
    std::function<void(size_t)> visit = [&](size_t index) {
        if (visited[index])
            return;
        visited[index] = true;
        for (const auto& binding : document.devices[index].bindings)
            if (binding.kind == SourceKind::VirtualDevice)
                visit(indices.at(binding.selector));
        result.renderOrder.push_back(index);
    };
    for (size_t index = 0; index < document.devices.size(); ++index)
        visit(index);
    for (const auto index : result.renderOrder) {
        const auto& device = document.devices[index];
        if (!device.routing.enabled)
            continue;
        auto passDelay = timing.passThroughDelayFrames;
        for (const auto& floor : passThroughFloors)
            if (floor.device == device.id)
                passDelay = std::max(passDelay, floor.frames);
        for (const auto& route : device.routing.sources) {
            if (!route.enabled)
                continue;
            const auto binding = std::find_if(device.bindings.begin(), device.bindings.end(),
                                              [&](const auto& value) {
                                                  return value.id == route.id;
                                              });
            auto sourceDelay = timing.sourceDelayFrames;
            if (binding->kind == SourceKind::InputDevice ||
                binding->kind == SourceKind::Application)
                for (const auto& delay : inputDelays)
                    if (delay.device == device.id && delay.source == binding->id)
                        sourceDelay = delay.frames;
            const auto latency = binding->kind == SourceKind::VirtualDevice
                                     ? result.latencyFrames[indices.at(binding->selector)]
                                 : binding->kind == SourceKind::PassThrough ? passDelay
                                                                            : sourceDelay;
            result.latencyFrames[index] = std::max(result.latencyFrames[index], latency);
        }
    }
    return result;
}

struct AudioScene::Impl {
    struct Input {
        const SourceRouting& configuration;
        SourceKind kind;
        ResolvedAudioSource binding;
        std::optional<ResamplingFilter> filter;
        std::optional<uint32_t> budget;
        PlanarBlock scratch;
        AudioMeter meter;
        size_t upstream{kNoDevice};
        std::atomic<bool> online{false};
        std::atomic<uint64_t> missing{0};
        Input(const SourceRouting& route, SourceKind type, uint32_t frames)
            : configuration(route), kind(type), scratch(route.channels, frames),
              meter(route.channels) {}
    };
    struct Device {
        const VirtualDeviceDefinition& definition;
        std::shared_ptr<AudioSink> sink;
        std::unique_ptr<RoutingGraph> graph;
        std::vector<std::unique_ptr<Input>> inputs;
        std::vector<RoutingInput> views;
        PlanarBlock output;
        AudioMeter meter;
        std::shared_ptr<RenderHistory> history;
        uint32_t latency{0};
        bool needed{false}, graphOK{true};
        std::atomic<bool> active{false};
        std::atomic<uint64_t> rendered{0}, graphFailures{0}, sinkFailures{0};
        Device(const VirtualDeviceDefinition& route, uint32_t frames)
            : definition(route), output(route.routing.outputChannels, frames),
              meter(route.routing.outputChannels) {}
    };
    const RouteDocument document;
    const SceneTiming timing;
    std::vector<std::unique_ptr<Device>> devices;
    std::vector<size_t> order;
    std::atomic<uint32_t> demanded{0};
    std::atomic<bool> rendered{false};
    std::atomic<bool> timingReady{false};

    Impl(RouteDocument desired, SceneTiming clock, const SourceResolver& source,
         const SinkResolver& sink, const std::shared_ptr<SceneHistoryStore>& histories,
         std::span<const DevicePassThroughDelay> passThroughFloors,
         std::span<const DeviceInputDelay> inputDelays)
        : document(std::move(desired)), timing(clock) {
        auto plan = planSceneTiming(document, timing, passThroughFloors, inputDelays);
        order = std::move(plan.renderOrder);
        std::unordered_map<std::string, size_t> indices;
        for (const auto& definition : document.devices) {
            indices.emplace(definition.id, devices.size());
            auto node = std::make_unique<Device>(definition, timing.blockFrames);
            node->latency = plan.latencyFrames[devices.size()];
            if (definition.routing.enabled) {
                node->sink = sink ? sink(definition) : nullptr;
                if (node->sink && node->sink->channels() != definition.routing.outputChannels)
                    throw std::invalid_argument("Scene sink channels do not match its device");
                node->graph = std::make_unique<RoutingGraph>(timing.renderRate, definition.routing);
            }
            devices.push_back(std::move(node));
        }
        for (auto& device : devices) {
            for (const auto& route : device->definition.routing.sources) {
                const auto binding =
                    std::find_if(device->definition.bindings.begin(),
                                 device->definition.bindings.end(), [&](const auto& value) {
                                     return value.id == route.id;
                                 });
                auto input = std::make_unique<Input>(route, binding->kind, timing.blockFrames);
                if (binding->kind == SourceKind::VirtualDevice)
                    input->upstream = indices.at(binding->selector);
                else if (device->definition.routing.enabled && route.enabled && source) {
                    input->binding = source(device->definition, *binding, route);
                    if (input->binding.source && input->binding.source->channels() < route.channels)
                        throw std::invalid_argument("Scene source channel capacity is too small");
                    if (!input->binding.cursor)
                        input->binding.cursor = std::make_shared<AudioReadCursor>();
                    for (const auto& delay : inputDelays)
                        if (delay.device == device->definition.id && delay.source == binding->id) {
                            input->filter = delay.filter;
                            input->budget = delay.frames;
                        }
                }
                device->views.push_back(input->scratch.input());
                device->inputs.push_back(std::move(input));
            }
        }
        auto store = histories ? histories : std::make_shared<SceneHistoryStore>();
        for (const auto& device : devices) {
            if (!device->definition.routing.enabled)
                continue;
            for (const auto& input : device->inputs) {
                if (!input->configuration.enabled || input->upstream == kNoDevice)
                    continue;
                auto& upstream = *devices[input->upstream];
                if (upstream.definition.routing.enabled && device->latency > upstream.latency &&
                    !upstream.history)
                    upstream.history =
                        store->impl_->obtain(upstream.definition.id, upstream.output.channels,
                                             timing.renderRate, upstream.latency);
            }
        }
    }
    void idle() noexcept {
        timingReady.store(false);
        demanded.store(0);
        for (auto& device : devices) {
            device->active.store(false);
            if (device->history)
                device->history->reset();
            if (device->sink)
                device->sink->idle();
            device->meter.clear();
            for (auto& input : device->inputs) {
                input->online.store(false);
                input->meter.clear();
            }
        }
    }
};

AudioScene::AudioScene(RouteDocument document, SceneTiming timing, SourceResolver source,
                       SinkResolver sink, std::shared_ptr<SceneHistoryStore> histories,
                       std::span<const DevicePassThroughDelay> passThroughFloors,
                       std::span<const DeviceInputDelay> inputDelays)
    : impl_(std::make_unique<Impl>(std::move(document), timing, source, sink, histories,
                                   passThroughFloors, inputDelays)) {}
AudioScene::~AudioScene() = default;
SceneRenderResult AudioScene::render(uint64_t firstFrame, uint32_t frames,
                                     uint32_t previewDevices) noexcept {
    // Conservatively exclude the last second of uint64 nanosecond time; never
    // pass an unrepresentable long-double conversion to frameNs().
    const auto maximum = (UINT64_MAX / 1000000000ULL) * impl_->timing.renderRate;
    if (!frames || frames > impl_->timing.blockFrames || firstFrame > maximum - frames) {
        impl_->idle();
        return {false, 0, 0, 0};
    }
    SceneRenderResult result{true, 0, 0, 0};
    bool timingReady = true;
    const auto demand = requestedDevices(previewDevices);
    impl_->demanded.store(demand);
    for (size_t index = 0; index < impl_->devices.size(); ++index)
        impl_->devices[index]->needed = demand & (uint32_t{1} << index);
    for (const auto index : impl_->order) {
        auto& node = *impl_->devices[index];
        node.active.store(node.needed);
        node.output.clear(frames);
        if (!node.needed) {
            if (node.history)
                node.history->reset();
            node.meter.clear();
            for (auto& input : node.inputs) {
                input->online.store(false);
                input->meter.clear();
            }
            if (node.sink)
                node.sink->idle();
            continue;
        }
        ++result.activeDevices;
        for (size_t source = 0; source < node.inputs.size(); ++source) {
            auto& input = *node.inputs[source];
            input.scratch.clear(frames);
            auto data = input.scratch.input();
            AudioReadResult read{0, false};
            if (input.configuration.enabled) {
                if (input.upstream != kNoDevice) {
                    const auto& upstream = *impl_->devices[input.upstream];
                    if (upstream.needed && upstream.graphOK) {
                        const auto delay = node.latency - upstream.latency;
                        if (!delay)
                            data = upstream.output.input();
                        else {
                            const auto prefix = static_cast<uint32_t>(std::min<uint64_t>(
                                frames, firstFrame < delay ? delay - firstFrame : 0));
                            read.missingFrames = prefix;
                            if (prefix < frames) {
                                std::array<float*, kMaximumRoutingChannels> planes;
                                read.missingFrames += upstream.history->read(
                                    firstFrame + prefix - delay,
                                    suffix(input.scratch, prefix, planes), frames - prefix);
                            }
                        }
                        read.online = true;
                        if (read.missingFrames)
                            timingReady = false;
                    } else
                        read.missingFrames = frames;
                } else if (input.binding.source) {
                    const auto prefix = static_cast<uint32_t>(std::min<uint64_t>(
                        frames, firstFrame < node.latency ? node.latency - firstFrame : 0));
                    if (prefix)
                        timingReady = false;
                    if (prefix < frames) {
                        std::array<float*, kMaximumRoutingChannels> planes;
                        const auto time =
                            frameNs(firstFrame + prefix - node.latency, impl_->timing.renderRate);
                        const auto output = suffix(input.scratch, prefix, planes);
                        read = input.filter
                                   ? input.binding.source->readWithFilter(
                                         time, impl_->timing.renderRate, output, frames - prefix,
                                         *input.binding.cursor, *input.filter)
                                   : input.binding.source->read(time, impl_->timing.renderRate,
                                                                output, frames - prefix,
                                                                *input.binding.cursor);
                    }
                    if (!read.online || read.missingFrames > frames - prefix) {
                        input.scratch.clear(frames);
                        read = {frames, false};
                    } else
                        read.missingFrames += prefix;
                    input.scratch.sanitize(frames);
                } else
                    read.missingFrames = frames;
            }
            input.online.store(read.online);
            input.missing.fetch_add(read.missingFrames);
            // Offline and disabled sources have fully cleared scratch planes.
            // Online reads may have silent channels but valid audio elsewhere,
            // even when missingFrames equals the whole block.
            if (read.online)
                input.meter.publish(data, frames);
            else
                input.meter.publishSilence(frames);
            node.views[source] = data;
        }
        node.graphOK = node.graph->render(node.views, node.output.output(), frames, firstFrame);
        if (!node.graphOK) {
            node.output.clear(frames);
            node.graphFailures.fetch_add(1);
            ++result.failedGraphs;
        }
        node.output.sanitize(frames);
        if (node.history)
            node.history->write(firstFrame, node.output.input(), frames);
        node.meter.publish(node.output.input(), frames);
        node.rendered.fetch_add(frames);
        if (node.sink && !node.sink->publish(firstFrame, impl_->timing.renderRate,
                                             node.output.input(), frames)) {
            node.sinkFailures.fetch_add(1);
            ++result.failedSinks;
        }
    }
    impl_->timingReady.store(timingReady);
    impl_->rendered.store(true);
    return result;
}
const RouteDocument& AudioScene::document() const noexcept {
    return impl_->document;
}
void AudioScene::activate() noexcept {
    for (auto& device : impl_->devices)
        if (device->sink)
            device->sink->activate();
}
const AudioSink* AudioScene::sink(size_t device) const noexcept {
    return device < impl_->devices.size() ? impl_->devices[device]->sink.get() : nullptr;
}
SceneTiming AudioScene::timing() const noexcept {
    return impl_->timing;
}
uint32_t AudioScene::requestedDevices(uint32_t previewDevices) const noexcept {
    uint32_t mask = 0;
    for (size_t index = 0; index < impl_->devices.size(); ++index) {
        const auto& node = *impl_->devices[index];
        if (node.definition.routing.enabled &&
            ((previewDevices & (uint32_t{1} << index)) || (node.sink && node.sink->requested())))
            mask |= uint32_t{1} << index;
    }
    for (auto position = impl_->order.rbegin(); position != impl_->order.rend(); ++position) {
        if (!(mask & (uint32_t{1} << *position)))
            continue;
        for (const auto& input : impl_->devices[*position]->inputs)
            if (input->configuration.enabled && input->upstream != kNoDevice &&
                impl_->devices[input->upstream]->definition.routing.enabled)
                mask |= uint32_t{1} << input->upstream;
    }
    return mask;
}
bool AudioScene::hasRendered() const noexcept {
    return impl_->rendered.load();
}
bool AudioScene::timingReady() const noexcept {
    return impl_->timingReady.load();
}
uint32_t AudioScene::demandedDevices() const noexcept {
    return impl_->demanded.load();
}
SceneDeviceStatus AudioScene::deviceStatus(size_t device) const noexcept {
    if (device >= impl_->devices.size())
        return {};
    const auto& node = *impl_->devices[device];
    return {node.active.load(), node.rendered.load(), node.graphFailures.load(),
            node.sinkFailures.load()};
}
SceneSourceStatus AudioScene::sourceStatus(size_t device, size_t source) const noexcept {
    if (device >= impl_->devices.size() || source >= impl_->devices[device]->inputs.size())
        return {};
    const auto& input = *impl_->devices[device]->inputs[source];
    return {input.online.load(), input.missing.load(), input.filter, input.budget};
}
bool AudioScene::deviceMeters(size_t device, AudioMeterSnapshot& output) const noexcept {
    return device < impl_->devices.size() && impl_->devices[device]->meter.read(output);
}
bool AudioScene::sourceMeters(size_t device, size_t source,
                              AudioMeterSnapshot& output) const noexcept {
    return device < impl_->devices.size() && source < impl_->devices[device]->inputs.size() &&
           impl_->devices[device]->inputs[source]->meter.read(output);
}
uint32_t AudioScene::deviceLatencyFrames(size_t device) const noexcept {
    return device < impl_->devices.size() ? impl_->devices[device]->latency : 0;
}
const ResolvedAudioSource* AudioScene::input(size_t device, size_t source) const noexcept {
    if (device >= impl_->devices.size() || source >= impl_->devices[device]->inputs.size())
        return nullptr;
    return &impl_->devices[device]->inputs[source]->binding;
}
} // namespace loopz
