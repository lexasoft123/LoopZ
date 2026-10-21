#include <loopz/macos/scene_renderer.hpp>
#include <loopz/timeline.hpp>
#include <loopz/audio_resampler.hpp>

#include <cassert>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <numbers>
#include <chrono>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace loopz;
using namespace loopz::macos;
using Phase = DriverConfigurationPhase;
void close(float actual, float expected) {
    assert(std::isfinite(actual) && std::fabs(actual - expected) < 0.00002f);
}
uint64_t ceiling(uint64_t frame, uint32_t to, uint32_t from) {
    return (frame / from) * to + ((frame % from) * to + from - 1) / from;
}
void conversion(uint32_t channels, uint32_t renderRate, uint32_t endpointRate) {
    auto region = DeviceAudioRegion::create(channels, endpointRate, 1);
    auto clock = std::make_shared<RenderClock>();
    const uint64_t now = 150000000000ULL;
    clock->nowNs = now;
    auto view = region->view();
    view.header().driverHeartbeatNs = now;
    view.header().driverOnline = 1;
    TransportAudioSink sink(region, clock, renderRate);
    assert(sink.requested());
    std::array<std::array<float, kBlock>, 64> samples{};
    std::array<const float*, 64> planes{};
    for (unsigned channel = 0; channel < channels; ++channel)
        planes[channel] = samples[channel].data();
    const auto maximumOutput = (uint64_t(64) * endpointRate + renderRate - 1) / renderRate;
    std::vector<float> captured((maximumOutput + 2) * channels);
    const auto origin = static_cast<uint64_t>(renderRate) * 100;
    const auto delay = TransportAudioSink::delayFrames(renderRate, endpointRate);
    uint64_t total = 0;
    for (unsigned block = 0; block < 100; ++block) {
        const auto first = origin + block * 64;
        for (unsigned channel = 0; channel < channels; ++channel)
            for (unsigned index = 0; index < 64; ++index)
                samples[channel][index] =
                    static_cast<float>(first + index - origin) * 0.0001f + channel * 0.01f;
        assert(sink.publish(first, renderRate, {{planes.data(), channels}, kBlock}, 64));
        const auto begin = ceiling(first, endpointRate, renderRate);
        const auto end = ceiling(first + 64, endpointRate, renderRate);
        const auto count = static_cast<uint32_t>(end - begin);
        const auto result =
            view.readCapture(begin, {captured.data(), count * channels}, count, now);
        assert(result.validBlock && !result.missingFrames);
        total += count;
        for (auto frame = begin; frame < end; ++frame) {
            const auto relative =
                static_cast<long double>(frame) * renderRate / endpointRate - origin - delay;
            if (relative < 2 * delay)
                continue; // The separate impulse test checks the startup transient.
            for (unsigned channel = 0; channel < channels; ++channel)
                close(captured[(frame - begin) * channels + channel],
                      static_cast<float>(relative) * 0.0001f + channel * 0.01f);
        }
    }
    assert(total == ceiling(origin + 6400, endpointRate, renderRate) -
                        ceiling(origin, endpointRate, renderRate));
    // Partial blocks can contain no endpoint sample while downsampling. They
    // still advance interpolation history and must not be reported as failures.
    for (unsigned index = 0; index < 16; ++index)
        assert(sink.publish(origin + 6400 + index, renderRate, {{planes.data(), channels}, kBlock},
                            1));
    clock->nowNs = now + kDeviceAudioHeartbeatTimeoutNs + 1;
    assert(!sink.requested());
    view.header().driverHeartbeatNs = clock->nowNs.load();
    assert(sink.requested());
    region->retire();
    assert(!sink.requested() &&
           !sink.publish(origin, renderRate, {{planes.data(), channels}, kBlock}, 64));
}
void independentPlaybackReaders() {
    auto region = DeviceAudioRegion::create(8, 44100, 7);
    auto clock = std::make_shared<RenderClock>();
    clock->nowNs = 100000000000ULL;
    auto view = region->view();
    view.header().driverOnline = 1;
    view.header().driverHeartbeatNs = clock->nowNs.load();
    std::array<float, 1024 * 8> data{};
    for (unsigned i = 0; i < data.size(); ++i)
        data[i] = (i % 8 + 1) * 0.01f;
    // Converted readers need filter history around the requested timestamp.
    assert(view.writePlayback(4410000 - 256, data, 1024));
    TransportAudioSource source(region, clock, TransportDirection::Playback);
    AudioReadCursor cursor1, cursor2;
    std::array<std::array<float, kBlock>, 8> samples{};
    std::array<float*, 8> planes{};
    for (unsigned channel = 0; channel < 8; ++channel)
        planes[channel] = samples[channel].data();
    const RoutingOutput output{planes, kBlock};
    for (auto* cursor : {&cursor1, &cursor2}) {
        const auto result = source.read(100000000000ULL, 48000, output, 256, *cursor);
        assert(result.online && !result.missingFrames && cursor->generation == 7);
        for (unsigned channel = 0; channel < 8; ++channel)
            for (unsigned index = 0; index < 256; ++index)
                close(samples[channel][index], (channel + 1) * 0.01f);
    }
    // Callback stoppage is unavailable even if the online bit remains set.
    clock->nowNs.fetch_add(kDeviceAudioHeartbeatTimeoutNs + 1);
    auto result = source.read(100000000000ULL, 48000, output, 64, cursor1);
    assert(!result.online && result.missingFrames == 64);
    close(samples[7][0], 0);
    view.header().driverHeartbeatNs = clock->nowNs.load();
    result = source.read(100000000000ULL, 48000, output, 64, cursor1);
    assert(result.online && !result.missingFrames);
    close(samples[7][0], 0.08f);
    region->retire();
    assert(!source.read(100000000000ULL, 48000, output, 64, cursor1).online);
    view.header().driverOnline = 1;
    view.header().driverHeartbeatNs = clock->nowNs.load();
    assert(!source.read(100000000000ULL, 48000, output, 64, cursor2).online);
}
void boundedConfigurationDemand() {
    auto region = DeviceAudioRegion::create(2, 48000, 1);
    auto clock = std::make_shared<RenderClock>();
    clock->nowNs = 100000000000ULL;
    auto& header = region->header();
    TransportAudioSink sink(region, clock);
    assert(!sink.beginConfigurationHold() && !sink.requested());
    header.driverOnline = 1;
    header.driverHeartbeatNs = clock->nowNs.load();
    assert(sink.beginConfigurationHold());
    assert(sink.requested()); // Live IO during preparation cannot clear pending demand.
    header.driverOnline = 0;
    clock->nowNs.fetch_add(300000000);
    assert(sink.requested());
    sink.endConfigurationHold();
    clock->nowNs.fetch_add(499999998);
    assert(sink.requested());
    clock->nowNs.fetch_add(4);
    assert(!sink.requested());
    header.driverOnline = 1;
    header.driverHeartbeatNs = clock->nowNs.load();
    assert(sink.beginConfigurationHold());
    header.driverOnline = 0;
    clock->nowNs.fetch_add(10000000001ULL);
    assert(!sink.requested());
    sink.endConfigurationHold();
    assert(!sink.requested()); // Expired pending work cannot reactivate the sink.
    header.driverOnline = 1;
    header.driverHeartbeatNs = clock->nowNs.load();
    assert(sink.beginConfigurationHold());
    sink.endConfigurationHold();
    assert(sink.requested());
    header.driverOnline = 0;
    assert(sink.requested()); // A second HAL pause remains inside the settling window.
    clock->nowNs.fetch_add(500000001);
    assert(!sink.requested());
    header.driverOnline = 1;
    header.driverHeartbeatNs = clock->nowNs.load();
    assert(sink.beginConfigurationHold());
    region->retire();
    assert(!sink.requested() && !sink.beginConfigurationHold());
}
void bandlimitedPublicInput() {
    struct Case {
        uint32_t sourceRate, renderRate, channels;
        double frequency;
        bool stopband;
    };
    for (const auto example :
         {Case{48000, 44100, 2, 20000, false}, Case{48000, 44100, 2, 22100, true},
          Case{44100, 48000, 8, 19000, false}, Case{48000, 8000, 2, 4400, true},
          Case{48000, 44100, 64, 1000, false}})
        for (auto direction : {TransportDirection::Capture, TransportDirection::Playback}) {
            auto region = DeviceAudioRegion::create(example.channels, example.sourceRate, 23);
            auto clock = std::make_shared<RenderClock>();
            constexpr uint64_t originNs = 100000000000ULL;
            clock->nowNs = originNs + 1000000000;
            auto view = region->view();
            view.header().helperOnline = view.header().driverOnline = 1;
            view.header().helperHeartbeatNs = clock->nowNs.load();
            view.header().driverHeartbeatNs = clock->nowNs.load();
            constexpr uint32_t sourceFrames = 8192;
            std::vector<float> input(static_cast<size_t>(sourceFrames) * example.channels);
            for (uint32_t frame = 0; frame < sourceFrames; ++frame)
                for (uint32_t channel = 0; channel < example.channels; ++channel)
                    input[frame * example.channels + channel] =
                        static_cast<float>(0.25 * std::sin(2 * std::numbers::pi_v<double> *
                                                           (example.frequency + channel * 37) *
                                                           frame / example.sourceRate));
            const auto firstFrame = uint64_t(example.sourceRate) * 100;
            assert(direction == TransportDirection::Capture
                       ? view.writeCapture(firstFrame, input, sourceFrames)
                       : view.writePlayback(firstFrame, input, sourceFrames));
            TransportAudioSource source(region, clock, direction);
            std::array<std::array<float, kBlock>, 64> samples{};
            std::array<float*, 64> planes{};
            for (uint32_t channel = 0; channel < example.channels; ++channel)
                planes[channel] = samples[channel].data();
            const RoutingOutput output{{planes.data(), example.channels}, kBlock};
            const auto first =
                originNs + static_cast<uint64_t>(2048.37L * 1e9L / example.sourceRate);
            AudioReadCursor cursor;
            const auto result = source.read(first, example.renderRate, output, kBlock, cursor);
            assert(result.online && !result.missingFrames && cursor.generation == 23);
            double minimumSnr = 300, maximumGain = -300;
            for (uint32_t channel = 0; channel < example.channels; ++channel) {
                double error = 0, energy = 0, reference = 0;
                for (uint32_t frame = 0; frame < kBlock; ++frame) {
                    const auto expected =
                        0.25 *
                        std::sin(2 * std::numbers::pi_v<double> *
                                 (example.frequency + channel * 37) *
                                 ((first - originNs) / 1e9 + double(frame) / example.renderRate));
                    const auto actual = samples[channel][frame];
                    error += (actual - expected) * (actual - expected);
                    energy += actual * actual;
                    reference += expected * expected;
                }
                minimumSnr =
                    std::min(minimumSnr, 10 * std::log10(reference / std::max(error, 1e-30)));
                maximumGain =
                    std::max(maximumGain, 10 * std::log10(std::max(energy, 1e-30) / reference));
            }
            assert(example.stopband ? maximumGain < -90 : minimumSnr > 90);
            // Fresh regions have no samples before the publication origin.
            // Missing filter wings must produce silence instead of switching
            // to unfiltered reads at the start of a recovered stream.
            const auto missing = source.read(originNs, example.renderRate, output, 64, cursor);
            assert(missing.online && missing.missingFrames == 64);
            for (uint32_t channel = 0; channel < example.channels; ++channel)
                for (uint32_t frame = 0; frame < 64; ++frame)
                    assert(samples[channel][frame] == 0);
        }
}

void publicInputBenchmark() {
    for (uint32_t channels : {2U, 8U, 64U})
        for (const auto rates :
             {std::pair{48000U, 48000U}, std::pair{48000U, 44100U}, std::pair{44100U, 48000U}}) {
            auto region = DeviceAudioRegion::create(channels, rates.first, 31);
            auto clock = std::make_shared<RenderClock>();
            constexpr uint64_t origin = 100000000000ULL;
            clock->nowNs = origin + 1000000000;
            auto view = region->view();
            view.header().driverOnline = 1;
            view.header().driverHeartbeatNs = clock->nowNs.load();
            std::vector<float> input(8192 * channels, 0.25f);
            assert(view.writePlayback(uint64_t(rates.first) * 100, input, 8192));
            TransportAudioSource source(region, clock, TransportDirection::Playback);
            std::array<std::array<float, 64>, 64> samples{};
            std::array<float*, 64> planes{};
            for (uint32_t channel = 0; channel < channels; ++channel)
                planes[channel] = samples[channel].data();
            const RoutingOutput output{{planes.data(), channels}, 64};
            AudioReadCursor cursor;
            std::array<double, 20> times{};
            for (uint32_t block = 0; block < 24; ++block) {
                const auto start = std::chrono::steady_clock::now();
                const auto result = source.read(origin + frameNs(2048, rates.first), rates.second,
                                                output, 64, cursor);
                assert(result.online && !result.missingFrames);
                if (block >= 4)
                    times[block - 4] = std::chrono::duration<double, std::micro>(
                                           std::chrono::steady_clock::now() - start)
                                           .count();
            }
            std::sort(times.begin(), times.end());
            std::printf("{\"channels\":%u,\"sourceRate\":%u,\"outputRate\":%u,\"frames\":64,"
                        "\"medianMicroseconds\":%.3f,\"maximumMicroseconds\":%.3f,"
                        "\"periodMicroseconds\":%.3f}\n",
                        channels, rates.first, rates.second, times[10], times.back(),
                        64e6 / rates.second);
        }
}

void alignedPublicBlocksMatchFrameReader() {
    struct Context {
        DeviceAudioView view;
        TransportDirection direction;
        uint64_t now;
        static ResampleFrame frame(const void* opaque, uint64_t position,
                                   std::span<float> output) noexcept {
            const auto& self = *static_cast<const Context*>(opaque);
            std::array<float, kDeviceAudioMaximumChannels> samples;
            const auto buffer = std::span<float>(samples.data(), self.view.channels());
            const auto result = self.direction == TransportDirection::Capture
                                    ? self.view.readCapture(position, buffer, 1, self.now,
                                                            kDeviceAudioConcurrentHeartbeatSkewNs)
                                    : self.view.readPlayback(position, buffer, 1, self.now,
                                                             kDeviceAudioConcurrentHeartbeatSkewNs);
            std::copy_n(samples.begin(), output.size(), output.begin());
            return {result.validBlock && !result.missingFrames, self.view.channels()};
        }
    };
    const AudioResampler reference;
    const auto boundary = [&](uint32_t rate, uint64_t fractionalNs) {
        auto region = DeviceAudioRegion::create(2, rate, 47);
        const auto view = region->view();
        auto clock = std::make_shared<RenderClock>();
        clock->nowNs = 101000000000ULL;
        view.header().driverOnline = 1;
        view.header().driverHeartbeatNs = clock->nowNs.load();
        TransportAudioSource source(region, clock, TransportDirection::Playback);
        const uint64_t host = 100000000000ULL + fractionalNs;
        const auto anchor = host / 1000000000ULL * rate;
        const auto offset = static_cast<long double>(host % 1000000000ULL) * rate / 1e9L;
        const auto first = anchor + static_cast<uint64_t>(std::floor(offset)) - 256;
        std::vector<float> samples(1024 * 2);
        for (uint32_t frame = 0; frame < 1024; ++frame) {
            samples[2 * frame] = .2f * std::sin(.7 * frame);
            samples[2 * frame + 1] = frame % 3 ? -.1f : .3f;
        }
        assert(view.writePlayback(first, samples, 1024));
        const Context context{view, TransportDirection::Playback, clock->nowNs.load()};
        std::array<std::array<float, kBlock>, 2> actual{}, expected{};
        const std::array<float*, 2> output{actual[0].data(), actual[1].data()};
        const std::array<float*, 2> comparison{expected[0].data(), expected[1].data()};
        const auto missing = reference.read({anchor, offset, double(rate), double(rate)},
                                            {comparison, kBlock}, kBlock, Context::frame, &context);
        AudioReadCursor cursor;
        const auto result = source.read(host, rate, {output, kBlock}, kBlock, cursor);
        assert(result.online && result.missingFrames == missing && actual == expected);
    };
    // Adding frames can cross a floating-point exponent boundary and move a
    // fraction just across AudioResampler's aligned-read cutoff mid-block.
    boundary(44100, 88322000);
    boundary(88200, 44161000);
    boundary(176400, 742080500);
    for (const auto count : {2U, 8U, 64U})
        for (const auto rate : {44100U, 48000U, 96000U, 192000U})
            for (const auto direction :
                 {TransportDirection::Playback, TransportDirection::Capture}) {
                auto region = DeviceAudioRegion::create(count, rate, 47);
                const auto view = region->view();
                auto clock = std::make_shared<RenderClock>();
                clock->nowNs = 100000000000ULL;
                view.header().helperOnline = view.header().driverOnline = 1;
                view.header().helperHeartbeatNs = view.header().driverHeartbeatNs =
                    clock->nowNs.load();
                TransportAudioSource source(region, clock, direction);
                const Context context{view, direction, clock->nowNs.load()};
                // Span a ring wrap, with holes at both tile and block boundaries.
                const uint64_t first = kDeviceAudioCapacity * 200ULL - 19;
                std::array<float, kDeviceAudioMaximumChannels> frame{};
                for (uint32_t index = 0; index < 512; ++index) {
                    if (index == 16 || index == 31 || index == 63 || index == 257)
                        continue;
                    for (uint32_t channel = 0; channel < count; ++channel)
                        frame[channel] = .001f * (channel + 1) + .00001f * index;
                    const auto samples = std::span<const float>(frame.data(), count);
                    assert(direction == TransportDirection::Capture
                               ? view.writeCapture(first + index, samples, 1)
                               : view.writePlayback(first + index, samples, 1));
                }
                for (const auto requested : {1U, count})
                    for (const auto frames : {1U, 31U, 64U, kBlock})
                        for (const auto fractionNs : {0ULL, 1ULL, 1000000000ULL / rate / 3}) {
                            std::array<std::array<float, kBlock + 1>, kDeviceAudioMaximumChannels>
                                actual{}, expected{};
                            std::array<float*, kDeviceAudioMaximumChannels> output{}, comparison{};
                            for (uint32_t channel = 0; channel < requested; ++channel) {
                                actual[channel].fill(-99);
                                output[channel] = actual[channel].data();
                                comparison[channel] = expected[channel].data();
                            }
                            const auto host = frameNs(first, rate) + fractionNs;
                            const auto anchor = host / 1000000000ULL * rate;
                            const auto offset =
                                static_cast<long double>(host % 1000000000ULL) * rate / 1e9L;
                            const auto missing =
                                reference.read({anchor, offset, double(rate), double(rate)},
                                               {{comparison.data(), requested}, kBlock}, frames,
                                               Context::frame, &context);
                            AudioReadCursor cursor;
                            const auto read = source.read(
                                host, rate, {{output.data(), requested}, kBlock}, frames, cursor);
                            assert(read.online && read.missingFrames == missing &&
                                   cursor.generation == 47);
                            for (uint32_t channel = 0; channel < requested; ++channel) {
                                for (uint32_t index = 0; index < frames; ++index)
                                    assert(actual[channel][index] == expected[channel][index]);
                                assert(actual[channel][frames] == -99);
                            }
                        }
                region->retire();
                std::array<float, 64> retired;
                retired.fill(1);
                std::array<float*, 1> plane{retired.data()};
                AudioReadCursor cursor;
                const auto result =
                    source.read(frameNs(first, rate), rate, {plane, 64}, 64, cursor);
                assert(!result.online && result.missingFrames == 64);
                assert(std::all_of(retired.begin(), retired.end(), [](auto value) {
                    return value == 0;
                }));
            }
}

class Source final : public AudioSource {
  public:
    std::atomic<bool> available{true};
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0.2f);
        return {0, available.load()};
    }
};
class Link final : public DriverConfigurationLink {
  public:
    uint64_t incarnation{1}, latest{0}, submissions{0};
    bool reject{false};
    std::map<uint64_t, DriverConfigurationResult> results;
    std::vector<DeviceTransport> staged;
    uint64_t session() const noexcept override {
        return incarnation;
    }
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override {
        ++submissions;
        if (reject)
            return -50;
        staged.assign(devices.begin(), devices.end());
        latest = revision;
        results[revision] = {revision, Phase::Pending, 0};
        return 0;
    }
    DriverConfigurationResult status(uint64_t revision) override {
        if (!revision)
            return latest ? results.at(latest) : DriverConfigurationResult{0, Phase::Committed, 0};
        const auto found = results.find(revision);
        return found == results.end() ? DriverConfigurationResult{revision, Phase::Unknown, 0}
                                      : found->second;
    }
    int32_t cancel(uint64_t revision) override {
        results.at(revision).phase = Phase::RollingBack;
        return 0;
    }
    void invalidate() noexcept override {
        incarnation = 0;
    }
    void finish(Phase phase = Phase::Committed) {
        results.at(latest).phase = phase;
    }
};
RouteDocument document() {
    RouteDocument result;
    for (unsigned i = 0; i < 3; ++i) {
        VirtualDeviceDefinition device;
        device.id = "device-" + std::to_string(i);
        device.name = device.id;
        device.routing.sources = {{"input", 2, 1, true, false, defaultChannelConnections(2, 2)}};
        device.bindings = {{"input",
                            i == 0   ? SourceKind::InputDevice
                            : i == 1 ? SourceKind::VirtualDevice
                                     : SourceKind::PassThrough,
                            i == 0   ? "physical"
                            : i == 1 ? "device-0"
                                     : ""}};
        result.devices.push_back(std::move(device));
    }
    return result;
}
void visibilityKeepsProducerWarm() {
    auto desired = document();
    desired.devices.resize(2);
    auto& target = desired.devices[1];
    target.routing.sources.push_back({"pass", 2, 0, true, false, {}});
    target.bindings.push_back({"pass", SourceKind::PassThrough, ""});
    auto source = std::make_shared<Source>();
    SceneRenderer renderer({}, [&](const auto&, const auto&, const auto&) {
        return ResolvedAudioSource{source, {}};
    });
    DeviceTransportPool pool;
    auto mappings = pool.prepare(desired, renderer.inputLatencyFrames(desired));
    auto initial = renderer.prepare(desired, mappings.devices());
    pool.commit(std::move(mappings));
    initial->commit();
    uint64_t first = 4800000;
    const auto block = [&](bool targetOnline, bool continuous) {
        const auto now = frameNs(first, 48000);
        for (size_t index = 0; index < pool.devices().size(); ++index) {
            auto& header = pool.devices()[index].region->header();
            header.driverOnline = index == 0 || targetOnline;
            if (header.driverOnline)
                header.driverHeartbeatNs = now;
        }
        const auto result = renderer.render(first, 64, now);
        if (continuous) {
            assert(result.activeDevices == 2 && !result.failedGraphs && !result.failedSinks);
            std::array<float, 128> samples;
            const auto read = pool.devices()[1].region->view().readCapture(first, samples, 64, now);
            assert(read.validBlock && !read.missingFrames);
            for (const auto value : samples)
                close(value, .2f);
        }
        first += 64;
        return result.activeDevices;
    };
    block(true, false); // Ordinary one-sample converter startup padding.
    block(true, true);
    for (const bool commit : {true, false, true, false}) {
        auto changed = desired;
        changed.devices[1].routing.sources[1].enabled =
            !changed.devices[1].routing.sources[1].enabled;
        auto staged = pool.prepare(changed, renderer.inputLatencyFrames(changed));
        assert(staged.devices()[1].region == pool.devices()[1].region);
        auto program = renderer.prepare(changed, staged.devices());
        for (unsigned pause = 0; pause < 256; ++pause)
            block(false, true); // macOS has stopped only the edited device's IO.
        if (commit) {
            pool.commit(std::move(staged));
            program->commit();
            desired = std::move(changed);
        } else
            program.reset();
        block(false, true); // Commit/rollback can precede HAL restart.
        block(true, true);
        assert(block(false, true) == 2); // HAL may pause again after the first resume.
        for (unsigned settle = 0; settle < 380; ++settle)
            block(false, false);
        assert(block(false, false) == 1); // Settling cannot keep an unused route active forever.
        block(true, false);
        block(true, true);
    }
    // Preparation never starts an unused input merely because it has a mapping.
    assert(block(false, false) == 1);
    auto changed = desired;
    changed.devices[1].routing.sources[1].enabled = !changed.devices[1].routing.sources[1].enabled;
    auto staged = pool.prepare(changed, renderer.inputLatencyFrames(changed));
    auto program = renderer.prepare(changed, staged.devices());
    assert(block(false, false) == 1);
    program.reset();
    assert(block(false, false) == 1);
    block(true, false);
    block(true, true);
    // Input format/storage changes retain the existing isolation contract.
    changed.devices[1].sampleRate = 44100;
    staged = pool.prepare(changed, renderer.inputLatencyFrames(changed));
    assert(staged.devices()[1].region != pool.devices()[1].region);
    program = renderer.prepare(changed, staged.devices());
    assert(block(false, false) == 1);
    program.reset();
}
void endpointLatencyPlanning() {
    auto desired = document();
    desired.devices[1].sampleRate = 44100;
    for (auto rate : {8000U, 44100U, 48000U, 192000U})
        for (auto delay : {0U, 128U, 1024U, 16384U}) {
            SceneRenderer renderer({rate, 64, delay, 17}, AudioScene::SourceResolver{});
            const auto result = renderer.inputLatencyFrames(desired);
            const auto expected = [&](uint32_t lookback, uint32_t endpointRate) {
                return ((uint64_t(lookback) + TransportAudioSink::delayFrames(rate, endpointRate)) *
                            endpointRate +
                        rate - 1) /
                       rate;
            };
            assert(result.size() == 3 && result[0] == expected(delay, 48000) &&
                   result[1] == expected(delay, 44100) && result[2] == expected(17, 48000));
            assert(result[0] <= kMaximumInputLatencyFrames);
        }
    desired.devices[0].routing.enabled = false;
    SceneRenderer renderer({}, AudioScene::SourceResolver{});
    const auto latencies = renderer.inputLatencyFrames(desired);
    assert(latencies == std::vector<uint32_t>({0, 104, 129}));
    DeviceTransportPool pool;
    pool.apply(desired); // A raw mapping without timing must not reach the renderer.
    bool rejected = false;
    try {
        (void)renderer.prepare(desired, pool.devices());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    pool.apply(desired, latencies);
    auto prepared = renderer.prepare(desired, pool.devices());
    prepared->commit();
    assert(renderer.observe()->deviceLatencyFrames(1) == 0);
}

void latencyMetadataFollowsSceneCommit() {
    Link link;
    SceneRenderer renderer({}, AudioScene::SourceResolver{});
    ConfigurationController controller(link, {}, &renderer);
    auto desired = document();
    uint64_t now = 1000;
    controller.setDesired(desired);
    controller.poll(now);
    assert(link.staged[0].inputLatencyFrames == 1025 && link.staged[1].inputLatencyFrames == 1025 &&
           link.staged[2].inputLatencyFrames == 129);
    link.finish();
    controller.poll(now += 100);
    const auto original = controller.devices();
    desired.devices[0].routing.sources[0].enabled = false;
    controller.setDesired(desired);
    controller.poll(now += 100);
    assert(link.staged[0].inputLatencyFrames == 1 && link.staged[1].inputLatencyFrames == 1);
    assert(link.staged[0].region != original[0].region &&
           link.staged[1].region != original[1].region &&
           link.staged[2].region == original[2].region);
    assert(controller.devices()[0].region == original[0].region &&
           renderer.observe()->deviceLatencyFrames(0) == 1024);
    link.finish(Phase::Aborted);
    controller.poll(now += 100);
    assert(!original[0].region->header().retired.load());
    assert(renderer.observe()->deviceLatencyFrames(1) == 1024);
    for (unsigned attempt = 0;
         attempt < 100 && controller.status().phase != RestorationPhase::Applying; ++attempt)
        controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::Applying);
    link.finish();
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::Restored);
    assert(controller.devices()[0].inputLatencyFrames == 1 &&
           renderer.observe()->deviceLatencyFrames(0) == 0 &&
           renderer.observe()->deviceLatencyFrames(1) == 0);
    assert(original[0].region->header().retired.load() &&
           original[1].region->header().retired.load() &&
           !original[2].region->header().retired.load());
}

void onlineDevices(const ConfigurationController& controller, uint64_t now, uint64_t first) {
    std::array<float, 2048> playback{};
    playback.fill(0.1f);
    for (const auto& device : controller.devices()) {
        auto view = device.region->view();
        view.header().driverHeartbeatNs = now;
        view.header().driverOnline = 1;
        if (device.hasOutput)
            assert(view.writePlayback(first - 512, playback, 1024));
    }
}
float sample(const ConfigurationController& controller, unsigned device, uint64_t frame,
             uint64_t now) {
    auto view = controller.devices()[device].region->view();
    std::array<float, 2> data{};
    const auto result = view.readCapture(frame, data, 1, now);
    assert(result.validBlock && !result.missingFrames);
    return data[0];
}
void compensatedHistorySurvivesNativeStaging() {
    Link link;
    auto source = std::make_shared<Source>();
    SceneRenderer renderer({}, [&](const auto&, const auto&, const auto&) {
        return ResolvedAudioSource{source, {}};
    });
    ConfigurationController controller(link, {}, &renderer);
    const auto original = document();
    RouteDocument desired{kRouteDocumentVersion, {original.devices[0], original.devices[2]}};
    desired.devices[0].routing.sources.push_back(
        {"virtual", 2, 1, true, false, defaultChannelConnections(2, 2)});
    desired.devices[0].bindings.push_back(
        {"virtual", SourceKind::VirtualDevice, desired.devices[1].id});
    controller.setDesired(desired);
    uint64_t now = 1000, first = 4800000;
    controller.poll(now);
    link.finish();
    controller.poll(now += 100);
    auto block = [&](float expected) {
        onlineDevices(controller, now * 1000000, first);
        const auto result = renderer.render(first, 64, now * 1000000);
        if (!result.validBlock || result.failedGraphs || result.failedSinks)
            std::fprintf(
                stderr,
                "Compensation render failed: frame=%llu graphs=%u sinks=%u phase=%u detail=%s\n",
                static_cast<unsigned long long>(first), result.failedGraphs, result.failedSinks,
                unsigned(controller.status().phase), controller.status().detail.c_str());
        assert(result.validBlock && !result.failedGraphs && !result.failedSinks);
        close(sample(controller, 0, first + 16, now * 1000000), expected);
        first += 64;
    };
    for (unsigned i = 0; i < 20; ++i)
        block(i < 14 ? 0.2f : 0.3f);
    const auto stable = controller.devices()[1].region;
    desired.devices[1].routing.gain = 0.5f;
    controller.setDesired(desired);
    controller.poll(now += 100);
    block(0.3f); // Candidate preparation does not touch the active history.
    link.finish(Phase::Aborted);
    controller.poll(now += 100);
    block(0.3f);
    controller.retryNow(now);
    controller.poll(++now);
    link.finish();
    controller.poll(now += 100);
    renderer.collect();
    assert(controller.devices()[1].region == stable);
    block(0.3f); // Retained pre-commit history is still complete.
    for (unsigned i = 1; i < 18; ++i)
        block(i < 14 ? 0.3f : 0.25f);
}
void stagedGraphRestoration() {
    Link link;
    auto source = std::make_shared<Source>();
    bool failPreparation = false;
    SceneRenderer renderer(
        {},
        [&](const auto&, const auto&, const auto&) {
            if (failPreparation)
                throw std::runtime_error("Resource preparation unavailable");
            return ResolvedAudioSource{source, {}};
        },
        {}, 1);
    ConfigurationController controller(link, {}, &renderer);
    auto desired = document();
    uint64_t now = 1000, first = 4800000;
    const auto render = [&] {
        onlineDevices(controller, now * 1000000, first);
        auto result = renderer.render(first, 64, now * 1000000);
        if (!result.validBlock || result.activeDevices != 3 || result.failedGraphs ||
            result.failedSinks)
            std::fprintf(stderr,
                         "render failure: time=%llu frame=%llu valid=%d active=%u graphs=%u "
                         "sinks=%u phase=%d\n",
                         static_cast<unsigned long long>(now),
                         static_cast<unsigned long long>(first), result.validBlock,
                         result.activeDevices, result.failedGraphs, result.failedSinks,
                         static_cast<int>(controller.status().phase));
        assert(result.validBlock && result.activeDevices == 3 && !result.failedGraphs &&
               !result.failedSinks);
    };
    const auto accept = [&] {
        assert(controller.status().transactionRevision);
        link.finish();
        controller.poll(now += 100);
        renderer.collect();
    };
    controller.setDesired(desired);
    failPreparation = true;
    controller.poll(now);
    assert(!link.submissions && controller.status().phase == RestorationPhase::Retry);
    assert(!renderer.observe());
    failPreparation = false;
    controller.poll(now += 100);
    assert(link.submissions == 1 && !renderer.observe());
    accept();
    render();
    close(sample(controller, 0, first + 16, now * 1000000), 0.2f);
    close(sample(controller, 1, first + 16, now * 1000000), 0.2f);
    close(sample(controller, 2, first + 16, now * 1000000), 0.1f);
    auto stable = controller.devices();

    // An unavailable input retains its entire binding and all dependent DSP.
    source->available = false;
    first += 64;
    render();
    close(sample(controller, 1, first + 16, now * 1000000), 0);
    close(sample(controller, 2, first + 16, now * 1000000), 0.1f);
    assert(renderer.observe()->document().devices[0].bindings[0].selector == "physical");
    source->available = true;
    first += 64;
    render();
    close(sample(controller, 1, first + 16, now * 1000000), 0.2f);
    assert(link.submissions == 1);

    desired.devices[0].routing.gain = 0.5f;
    controller.setDesired(desired);
    controller.poll(now += 100);
    first += 64;
    render(); // Driver hasn't committed; old DSP remains live.
    close(sample(controller, 1, first + 16, now * 1000000), 0.2f);
    link.finish(Phase::Aborted);
    controller.poll(now += 100);
    assert(renderer.observe()->document().devices[0].routing.gain == 1);
    controller.retryNow(now);
    controller.poll(now);
    accept();
    first += 64;
    render();
    close(sample(controller, 1, first + 16, now * 1000000), 0.1f);
    close(sample(controller, 1, first, now * 1000000),
          0.2f); // Unchanged sink retains its boundary sample.
    for (unsigned i = 0; i < 3; ++i)
        assert(controller.devices()[i].region == stable[i].region);

    // A driver connection disappears with an accepted transaction in flight.
    // Its stale acknowledgement must never publish that prepared DSP graph.
    desired.devices[0].routing.gain = 0.25f;
    controller.setDesired(desired);
    controller.poll(now += 100);
    const auto stale = link.latest;
    link.incarnation = 0;
    controller.poll(now += 100);
    link.results.at(stale).phase = Phase::Committed;
    assert(renderer.observe()->document().devices[0].routing.gain == 0.5f);
    link.incarnation = 2;
    controller.poll(now += 100);
    assert(link.latest > stale && controller.status().phase == RestorationPhase::Applying);
    accept();
    first += 64;
    render();
    close(sample(controller, 1, first + 16, now * 1000000), 0.05f);

    // Retiring one mapping recreates only that endpoint; saved DSP and the
    // upstream/downstream relationships are restored without a new user edit.
    stable = controller.devices();
    stable[1].region->retire();
    for (unsigned poll = 0; poll < 100 && !controller.status().transactionRevision; ++poll)
        controller.poll(now += 100);
    accept();
    first += 64;
    render();
    close(sample(controller, 1, first + 16, now * 1000000), 0.05f);
    assert(controller.devices()[0].region == stable[0].region);
    assert(controller.devices()[1].region != stable[1].region);
    assert(controller.devices()[2].region == stable[2].region);
    assert(controller.desired()->devices.size() == 3);
    assert(controller.status().phase == RestorationPhase::Restored);

    // A stalled observation keeps the old graph alive. Publication exhaustion
    // must fail before native submission and automatically retry after release.
    std::optional<SceneRenderer::Observation> held(renderer.observe());
    desired.devices[0].routing.gain = 0.125f;
    controller.setDesired(desired);
    controller.poll(now += 100);
    accept();
    assert(held->get()->document().devices[0].routing.gain == 0.25f);
    desired.devices[0].routing.gain = 0.0625f;
    controller.setDesired(desired);
    const auto submitted = link.submissions;
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::Retry && link.submissions == submitted);
    assert(renderer.observe()->document().devices[0].routing.gain == 0.125f);
    assert(controller.desired()->devices[0].routing.gain == 0.0625f);
    held.reset();
    renderer.collect();
    for (unsigned poll = 0; poll < 100 && !controller.status().transactionRevision; ++poll)
        controller.poll(now += 100);
    accept();
    first += 64;
    render();
    close(sample(controller, 1, first + 16, now * 1000000), 0.0125f);
}

void convertedOutputSurvivesStaging() {
    for (const auto rates : {std::pair{48000U, 44100U}, std::pair{44100U, 48000U}}) {
        Link link;
        auto source = std::make_shared<Source>();
        SceneRenderer renderer({rates.first, 64, 1024, 128},
                               [&](const auto&, const auto&, const auto&) {
                                   return ResolvedAudioSource{source, {}};
                               });
        ConfigurationController controller(link, {}, &renderer);
        auto desired = document();
        desired.devices.resize(1);
        desired.devices[0].sampleRate = rates.second;
        uint64_t now = 1000, first = uint64_t(rates.first) * 100;
        controller.setDesired(desired);
        controller.poll(now);
        link.finish();
        controller.poll(now += 100);
        assert(controller.status().phase == RestorationPhase::Restored &&
               controller.devices().size() == 1);
        const auto region = controller.devices()[0].region;
        const auto render = [&] {
            onlineDevices(controller, now * 1000000, first);
            const auto result = renderer.render(first, 64, now * 1000000);
            assert(result.validBlock && !result.failedGraphs && !result.failedSinks);
            const auto output = ceiling(first, rates.second, rates.first);
            first += 64;
            return output;
        };
        // Prime both wings of the causal filter before checking steady audio.
        for (unsigned block = 0; block < 8; ++block)
            render();
        close(sample(controller, 0, render(), now * 1000000), 0.2f);
        desired.devices[0].routing.gain = 0.5f;
        controller.setDesired(desired);
        controller.poll(now += 100);
        close(sample(controller, 0, render(), now * 1000000), 0.2f);
        link.finish(Phase::Aborted);
        controller.poll(now += 100);
        close(sample(controller, 0, render(), now * 1000000), 0.2f);
        controller.retryNow(now);
        controller.poll(now += 100);
        link.finish();
        controller.poll(now += 100);
        assert(controller.devices()[0].region == region);
        // The unchanged sink retains prior samples across the accepted edit.
        close(sample(controller, 0, render(), now * 1000000), 0.2f);
        for (unsigned block = 0; block < 8; ++block)
            render();
        close(sample(controller, 0, render(), now * 1000000), 0.1f);
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark-public-input") {
        publicInputBenchmark();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--aligned-input") {
        alignedPublicBlocksMatchFrameReader();
        return 0;
    }
    const bool trace = argc == 2 && std::string_view(argv[1]) == "--trace-progress";
    assert(argc == 1 || trace);
    for (auto channels : {2U, 8U, 64U})
        for (auto renderRate : {8000U, 44100U, 48000U, 192000U})
            for (auto endpointRate : kVirtualSampleRates) {
                if (trace) {
                    std::printf("Conversion: channels=%u render=%u endpoint=%u\n", channels,
                                renderRate, endpointRate);
                    std::fflush(stdout);
                }
                conversion(channels, renderRate, endpointRate);
            }
    if (trace) {
        std::puts("Conversion matrix complete; starting native restoration contracts");
        std::fflush(stdout);
    }
    boundedConfigurationDemand();
    visibilityKeepsProducerWarm();
    independentPlaybackReaders();
    alignedPublicBlocksMatchFrameReader();
    bandlimitedPublicInput();
    endpointLatencyPlanning();
    latencyMetadataFollowsSceneCommit();
    compensatedHistorySurvivesNativeStaging();
    stagedGraphRestoration();
    convertedOutputSurvivesStaging();
    std::puts("Scene rendering: endpoint clocks, stale leases, independent readers and "
              "transactional full graph restoration passed");
}
