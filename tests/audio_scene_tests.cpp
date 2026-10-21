#include <loopz/audio_scene.hpp>
#include <loopz/realtime_publication.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <thread>

namespace {
thread_local bool trackHeap = false;
thread_local unsigned heapEvents = 0;
} // namespace
void* operator new(std::size_t size) {
    if (trackHeap)
        ++heapEvents;
    if (auto* value = std::malloc(size ? size : 1))
        return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* value) noexcept {
    if (trackHeap && value)
        ++heapEvents;
    std::free(value);
}
void operator delete[](void* value) noexcept {
    ::operator delete(value);
}
void operator delete(void* value, std::size_t) noexcept {
    ::operator delete(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    ::operator delete(value);
}

namespace {
using namespace loopz;
void close(float actual, float expected) {
    assert(std::isfinite(actual) && std::fabs(actual - expected) < 0.00001f);
}
template <typename Operation> void rejects(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
class Source final : public AudioSource {
  public:
    uint32_t count;
    std::atomic<bool> online{true};
    float value{0.2f};
    bool malformed{false};
    mutable uint64_t lastTime{0}, reads{0};
    explicit Source(uint32_t channels = 2) : count(channels) {}
    uint32_t channels() const noexcept override {
        return count;
    }
    AudioReadResult read(uint64_t time, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override {
        lastTime = time;
        ++reads;
        ++cursor.generation;
        cursor.rate = rate;
        for (uint32_t channel = 0; channel < output.channels.size(); ++channel)
            std::fill_n(output.channels[channel], frames, value * (channel + 1));
        return {malformed ? frames + 1 : 0, online.load()};
    }
};
class Sink final : public AudioSink {
  public:
    uint32_t count;
    std::atomic<bool> demand{true};
    bool healthy{true};
    uint64_t blocks{0}, idleCalls{0}, lastFrame{0};
    std::array<std::array<float, kBlock>, kMaximumRoutingChannels> samples{};
    explicit Sink(uint32_t channels = 2) : count(channels) {}
    uint32_t channels() const noexcept override {
        return count;
    }
    bool requested() const noexcept override {
        return demand.load();
    }
    bool publish(uint64_t first, uint32_t, RoutingInput input, uint32_t frames) noexcept override {
        ++blocks;
        lastFrame = first;
        for (uint32_t channel = 0; channel < count; ++channel)
            std::copy_n(input.channels[channel], frames, samples[channel].begin());
        return healthy;
    }
    void idle() noexcept override {
        ++idleCalls;
    }
};
VirtualDeviceDefinition device(std::string id, SourceKind kind = SourceKind::InputDevice,
                               std::string selector = "physical", uint32_t channels = 2) {
    VirtualDeviceDefinition result;
    result.id = std::move(id);
    result.name = result.id;
    result.routing.outputChannels = channels;
    result.routing.sources = {
        {"source", channels, 1, true, false, defaultChannelConnections(channels, channels)}};
    result.bindings = {{"source", kind, std::move(selector)}};
    return result;
}
SceneRenderResult render(AudioScene& scene, uint64_t first = 100000, uint32_t frames = 64,
                         uint32_t previewDevices = 0) {
    heapEvents = 0;
    trackHeap = true;
    const auto result = scene.render(first, frames, previewDevices);
    trackHeap = false;
    assert(!heapEvents);
    return result;
}
float clockSignal(uint64_t frame, uint32_t channel) {
    return (0.01f + static_cast<float>(frame % 97) * 0.001f) * (channel + 1) / 64;
}
class ClockSource final : public AudioSource {
  public:
    explicit ClockSource(uint32_t channels) : count(channels) {}
    uint32_t channels() const noexcept override {
        return count;
    }
    AudioReadResult read(uint64_t time, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        const auto first =
            static_cast<uint64_t>(std::llround(static_cast<long double>(time) * rate / 1e9L));
        for (uint32_t channel = 0; channel < count; ++channel)
            for (uint32_t sample = 0; sample < frames; ++sample)
                output.channels[channel][sample] = clockSignal(first + sample, channel);
        return {0, online.load()};
    }
    const uint32_t count;
    std::atomic<bool> online{true};
};
RouteDocument compensatedDocument(uint32_t channels) {
    auto fast = device("fast", SourceKind::PassThrough, "", channels);
    auto relay = device("relay", SourceKind::VirtualDevice, "fast", channels);
    auto mixed = device("mixed", SourceKind::VirtualDevice, "relay", channels);
    mixed.routing.sources.push_back(
        {"capture", channels, 1, true, false, defaultChannelConnections(channels, channels)});
    mixed.bindings.push_back({"capture", SourceKind::InputDevice, "physical"});
    mixed.routing.sources.push_back(
        {"pass", channels, 1, true, false, defaultChannelConnections(channels, channels)});
    mixed.bindings.push_back({"pass", SourceKind::PassThrough, ""});
    return {kRouteDocumentVersion, {mixed, relay, fast}};
}
void inputBudgetsPreserveAlignment() {
    auto source = std::make_shared<ClockSource>(2);
    auto sink = std::make_shared<Sink>(2);
    const std::vector<DeviceInputDelay> inputs{{"mixed", "capture", 352}};
    AudioScene scene(
        compensatedDocument(2), {48000, 32, 1024, 128},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto& definition) -> std::shared_ptr<AudioSink> {
            return definition.id == "mixed" ? sink : nullptr;
        },
        {}, {}, inputs);
    assert(scene.deviceLatencyFrames(0) == 352 && scene.deviceLatencyFrames(1) == 128);
    uint64_t first = 100000;
    for (unsigned block = 0; block < 24; ++block, first += 32) {
        assert(render(scene, first, 32).activeDevices == 3);
        if (block < 8)
            continue;
        assert(scene.timingReady());
        for (uint32_t channel = 0; channel < 2; ++channel)
            for (uint32_t frame = 0; frame < 32; ++frame)
                close(sink->samples[channel][frame], 3 * clockSignal(first + frame - 352, channel));
    }
}
void alignedPathsAndHistoryWrap() {
    for (uint32_t rate : {44100U, 48000U})
        for (uint32_t quantum : {32U, 64U, 256U})
            for (uint32_t channels : {2U, 8U, 64U}) {
                auto source = std::make_shared<ClockSource>(channels);
                auto sink = std::make_shared<Sink>(channels);
                const SceneTiming timing{rate, quantum, 166, 17};
                const auto plan = planSceneTiming(compensatedDocument(channels), timing);
                assert(plan.renderOrder == std::vector<size_t>({2, 1, 0}));
                assert(plan.latencyFrames == std::vector<uint32_t>({166, 17, 17}));
                AudioScene scene(
                    compensatedDocument(channels), timing,
                    [&](const auto&, const auto&, const auto&) {
                        return ResolvedAudioSource{source, {}};
                    },
                    [&](const auto& d) -> std::shared_ptr<AudioSink> {
                        return d.id == "mixed" ? sink : nullptr;
                    });
                assert(scene.deviceLatencyFrames(0) == 166 && scene.deviceLatencyFrames(1) == 17 &&
                       scene.deviceLatencyFrames(2) == 17);
                const uint64_t start = 100000;
                assert(!scene.timingReady());
                uint64_t first = start;
                const std::array<uint32_t, 4> sizes{quantum, quantum - 1, 1, 17};
                // Each matrix case checks cold/warm alignment and partial
                // blocks. A wide case crosses the ring; the separate maximum
                // delay case checks repeated wraps at the retention boundary.
                const bool wrapCase = rate == 48000 && quantum == 256 && channels == 64;
                const unsigned blocks = wrapCase ? 160 : 32;
                for (unsigned block = 0; block < blocks; ++block) {
                    const auto frames = sizes[block % sizes.size()];
                    assert(render(scene, first, frames).activeDevices == 3);
                    const auto prefix =
                        std::min<uint64_t>(frames, first < start + 149 ? start + 149 - first : 0);
                    assert(scene.timingReady() == !prefix);
                    assert(scene.sourceStatus(0, 0).missingFrames >= prefix);
                    for (uint32_t channel = 0; channel < channels; ++channel)
                        for (uint32_t sample = 0; sample < frames; ++sample)
                            close(sink->samples[channel][sample],
                                  clockSignal(first + sample - 166, channel) *
                                      (sample < prefix ? 2 : 3));
                    first += frames;
                }
                assert(scene.sourceStatus(0, 0).missingFrames == 149);
                if (wrapCase)
                    assert(first - start > kCapacity / 2 + kBlock);
                // A scheduling discontinuity or loss of demand cannot replay old ring data.
                const auto resumedFrames = std::min(64U, quantum);
                first += 1000000;
                assert(render(scene, first, resumedFrames).validBlock);
                assert(!scene.timingReady());
                for (uint32_t sample = 0; sample < resumedFrames; ++sample)
                    close(sink->samples[0][sample], 2 * clockSignal(first + sample - 166, 0));
                sink->demand = false;
                const auto idle = render(scene, first + resumedFrames, resumedFrames);
                assert(idle.validBlock && !idle.activeDevices);
                sink->demand = true;
                assert(render(scene, first + 2 * resumedFrames, resumedFrames).validBlock);
                for (uint32_t sample = 0; sample < resumedFrames; ++sample)
                    close(sink->samples[0][sample],
                          2 * clockSignal(first + 2 * resumedFrames + sample - 166, 0));
            }
}
void compensationSurvivesPreparedSceneChanges() {
    auto source = std::make_shared<ClockSource>(2);
    auto capture = std::make_shared<ClockSource>(2);
    auto sink = std::make_shared<Sink>();
    auto histories = std::make_shared<SceneHistoryStore>();
    auto desired = compensatedDocument(2);
    SceneTiming timing{48000, 64, 166, 17};
    auto make = [&] {
        return std::make_unique<AudioScene>(
            desired, timing,
            [&](const auto&, const auto& binding, const auto&) {
                return ResolvedAudioSource{
                    binding.kind == SourceKind::InputDevice ? capture : source, {}};
            },
            [&](const auto& d) -> std::shared_ptr<AudioSink> {
                return d.id == "mixed" ? sink : nullptr;
            },
            histories);
    };
    auto current = make();
    uint64_t first = 100000;
    for (unsigned block = 0; block < 20; ++block, first += 64)
        render(*current, first);
    desired.devices[2].routing.gain = 0.5f;
    auto abandoned = make();
    render(*current, first);
    first += 64;
    abandoned.reset();
    auto next = make();
    render(*current, first);
    first += 64;
    const auto committedAt = first;
    current.swap(next);
    next.reset();
    for (unsigned block = 0; block < 8; ++block, first += 64) {
        render(*current, first);
        for (uint32_t sample = 0; sample < 64; ++sample) {
            const auto factor = first + sample < committedAt + 149 ? 3.0f : 2.5f;
            close(sink->samples[0][sample], factor * clockSignal(first + sample - 166, 0));
        }
    }
    // A longer consumer uses already retained history without cold-starting an
    // unchanged upstream. A changed upstream origin must use fresh history.
    timing.sourceDelayFrames = 230;
    current = make();
    render(*current, first);
    close(sink->samples[0][0], 2.5f * clockSignal(first - 230, 0));
    first += 64;
    timing.passThroughDelayFrames = 33;
    current = make();
    render(*current, first);
    close(sink->samples[0][0], 2 * clockSignal(first - 230, 0));
    for (unsigned block = 0; block < 8; ++block) {
        first += 64;
        render(*current, first);
    }
    capture->online = false;
    first += 64;
    render(*current, first);
    close(sink->samples[0][0], 1.5f * clockSignal(first - 230, 0));
    assert(current->timingReady() && current->deviceLatencyFrames(0) == 230);
    capture->online = true;
    first += 64;
    render(*current, first);
    close(sink->samples[0][0], 2.5f * clockSignal(first - 230, 0));
}
void removedPairPreservesDependentAudio() {
    auto upstream = device("pair-upstream", SourceKind::InputDevice, "physical", 8);
    auto downstream = device("pair-downstream", SourceKind::VirtualDevice, upstream.id, 8);
    downstream.routing.outputChannels = 2;
    downstream.routing.sources[0].connections = {{2, 0, 0.25f}, {6, 1, 0.75f}};
    const RouteDocument original{kRouteDocumentVersion, {upstream, downstream}};
    auto changed = original;
    removeDeviceOutputPair(changed, upstream.id, 2);
    auto source = std::make_shared<Source>(8);
    source->value = 0.1f;
    for (const auto* document : {&original, static_cast<const RouteDocument*>(&changed)}) {
        auto sink = std::make_shared<Sink>(2);
        AudioScene scene(
            *document, {48000, 64, 64, 32},
            [&](const auto&, const auto&, const auto&) {
                return ResolvedAudioSource{source, {}};
            },
            [&](const auto& definition) -> std::shared_ptr<AudioSink> {
                return definition.id == downstream.id ? sink : nullptr;
            });
        for (uint64_t first = 100000; first < 102048; first += 64)
            assert(render(scene, first).activeDevices == 2);
        for (uint32_t sample = 0; sample < 64; ++sample) {
            close(sink->samples[0][sample], document == &original ? 0.075f : 0);
            close(sink->samples[1][sample], 0.525f);
        }
    }
}

void maximumCompensationDelay() {
    auto source = std::make_shared<ClockSource>(2);
    auto sink = std::make_shared<Sink>();
    AudioScene scene(
        compensatedDocument(2), {48000, 256, kCapacity / 2, 0},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto& d) -> std::shared_ptr<AudioSink> {
            return d.id == "mixed" ? sink : nullptr;
        });
    const uint64_t start = 100000;
    for (uint64_t first = start; first < start + 256 * 200; first += 256) {
        render(scene, first, 256);
        for (uint32_t sample = 0; sample < 256; ++sample)
            close(sink->samples[0][sample], clockSignal(first + sample - kCapacity / 2, 0) *
                                                (first + sample < start + kCapacity / 2 ? 2 : 3));
    }
    assert(scene.sourceStatus(0, 0).missingFrames == kCapacity / 2);
}
void delayUnderflowIsSilence() {
    auto source = std::make_shared<ClockSource>(2);
    auto sink = std::make_shared<Sink>();
    AudioScene scene(
        compensatedDocument(2), {48000, 64, 166, 17},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto& d) -> std::shared_ptr<AudioSink> {
            return d.id == "mixed" ? sink : nullptr;
        });
    for (uint64_t first = 0; first < 384; first += 64) {
        render(scene, first);
        for (uint32_t sample = 0; sample < 64; ++sample)
            close(sink->samples[0][sample],
                  first + sample < 166 ? 0 : 3 * clockSignal(first + sample - 166, 0));
    }
}
void concurrentPreparationRetainsLiveHistory() {
    auto source = std::make_shared<ClockSource>(2);
    auto sink = std::make_shared<Sink>();
    auto histories = std::make_shared<SceneHistoryStore>();
    auto desired = compensatedDocument(2);
    auto make = [&] {
        return std::make_unique<AudioScene>(
            desired, SceneTiming{48000, 64, 166, 17},
            [&](const auto&, const auto&, const auto&) {
                return ResolvedAudioSource{source, {}};
            },
            [&](const auto& d) -> std::shared_ptr<AudioSink> {
                return d.id == "mixed" ? sink : nullptr;
            },
            histories);
    };
    RealtimePublication<AudioScene> scenes(32);
    scenes.stage(make())->commit();
    std::atomic<unsigned> blocks{0};
    std::jthread audio([&](std::stop_token stop) {
        uint64_t first = 100000;
        while (!stop.stop_requested()) {
            {
                auto scene = scenes.borrow();
                const auto result = render(*scene.get(), first);
                assert(result.validBlock && !result.failedGraphs && !result.failedSinks);
                if (first >= 100000 + 149) {
                    assert(scene->timingReady());
                    close(sink->samples[0][0], 3 * clockSignal(first - 166, 0));
                }
            }
            first += 64;
            if (++blocks % 16 == 0)
                std::this_thread::yield();
        }
    });
    const auto wait = [&](unsigned target) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (blocks.load() < target && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(blocks.load() >= target);
    };
    wait(20);
    for (unsigned edit = 0; edit < 16; ++edit) {
        desired.devices[0].name = "Live edit " + std::to_string(edit);
        auto next = scenes.stage(make());
        next->commit();
        wait(blocks.load() + 20);
        scenes.collect();
    }
    audio.request_stop();
    audio.join();
    scenes.collect();
}
void dependencyDemandAndRecovery() {
    auto source = std::make_shared<Source>();
    auto upstreamSink = std::make_shared<Sink>();
    auto downstreamSink = std::make_shared<Sink>();
    upstreamSink->demand = false;
    auto upstream = device("upstream");
    upstream.routing.gain = 0.5f;
    auto downstream = device("downstream", SourceKind::VirtualDevice, "upstream");
    downstream.sampleRate = 44100; // Endpoint rate does not retime virtual links.
    downstream.routing.gain = 0.5f;
    // Reverse document order verifies dependency sorting.
    RouteDocument desired{1, {downstream, upstream}};
    unsigned resolves = 0;
    AudioScene scene(
        desired, {},
        [&](const auto&, const auto&, const auto&) {
            ++resolves;
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto& definition) {
            return definition.id == "upstream" ? upstreamSink : downstreamSink;
        });
    assert(resolves == 1);
    assert(scene.deviceLatencyFrames(0) == 1024 && scene.deviceLatencyFrames(1) == 1024);
    const auto result = render(scene);
    assert(result.validBlock && result.activeDevices == 2 && !result.failedGraphs);
    assert(source->lastTime == frameNs(100000 - 1024, 48000));
    close(downstreamSink->samples[0][0], 0.05f);
    close(downstreamSink->samples[1][63], 0.1f);
    assert(scene.deviceStatus(1).active && scene.sourceStatus(0, 0).online);
    AudioMeterSnapshot meter;
    assert(scene.sourceMeters(1, 0, meter) && meter.channels == 2 && meter.frames == 64);
    close(meter.peak[1], 0.4f);
    assert(scene.deviceMeters(0, meter));
    close(meter.rms[1], 0.1f);

    source->online = false;
    render(scene, 100064);
    close(downstreamSink->samples[0][0], 0);
    assert(!scene.sourceStatus(1, 0).online && scene.sourceStatus(1, 0).missingFrames == 64);
    // No edit, resolver, or graph reconstruction when the input returns.
    source->online = true;
    render(scene, 100128);
    close(downstreamSink->samples[0][0], 0.05f);
    assert(resolves == 1 && scene.sourceStatus(1, 0).online);
    assert(scene.document().devices[1].bindings[0].selector == "physical");

    downstreamSink->demand = false;
    const auto reads = source->reads;
    assert(!render(scene).activeDevices && source->reads == reads);
    assert(scene.deviceMeters(0, meter) && meter.frames == 0 && meter.peak[0] == 0);
    assert(upstreamSink->idleCalls && downstreamSink->idleCalls);
    downstreamSink->demand = true;
    upstreamSink->healthy = false;
    assert(render(scene).failedSinks == 1);
    close(downstreamSink->samples[0][0], 0.05f); // Failed public sink cannot break links.
}
void unwiredSourcePreview() {
    auto route = device("preview");
    route.routing.sources[0].connections.clear();
    route.routing.sources[0].gain = 0;
    route.routing.sources[0].muted = true;
    auto disabled = device("disabled");
    disabled.routing.enabled = false;
    auto source = std::make_shared<Source>();
    auto sink = std::make_shared<Sink>();
    sink->demand = false;
    AudioScene scene(
        {1, {route, disabled}}, {},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto&) {
            return sink;
        });
    assert(!render(scene).activeDevices && !source->reads);
    assert(scene.requestedDevices(UINT32_MAX) == 1);
    const auto result = render(scene, 100064, 64, UINT32_MAX);
    assert(result.activeDevices == 1 && !result.failedGraphs && !result.failedSinks);
    assert(scene.demandedDevices() == 1 && source->reads == 1);
    AudioMeterSnapshot meter;
    assert(scene.sourceMeters(0, 0, meter) && meter.frames == 64 && meter.channels == 2);
    close(meter.peak[0], 0.2f);
    close(meter.peak[1], 0.4f);
    assert(scene.deviceMeters(0, meter));
    close(meter.peak[0], 0);
    close(sink->samples[1][0], 0);
    assert(scene.document().devices[0].routing.sources[0].connections.empty());
    assert(!render(scene, 100128).activeDevices && source->reads == 1);
    assert(scene.sourceMeters(0, 0, meter) && !meter.frames);
    sink->demand = true;
    assert(render(scene, 100192).activeDevices == 1 && source->reads == 2);
}
void silentMeterBlocksAndConcurrentObservers() {
    for (const auto channels : {2U, 8U, 64U}) {
        AudioMeter meter(channels);
        std::array<std::array<float, 64>, kMaximumRoutingChannels> samples{};
        std::array<const float*, kMaximumRoutingChannels> planes{};
        for (uint32_t channel = 0; channel < channels; ++channel) {
            samples[channel].fill(.001f * (channel + 1));
            planes[channel] = samples[channel].data();
        }
        const RoutingInput data{{planes.data(), channels}, 64};
        const auto verify = [&](const AudioMeterSnapshot& snapshot) {
            assert(snapshot.channels == channels);
            assert(snapshot.frames == 0 || snapshot.frames == 17 || snapshot.frames == 64);
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const auto expected = snapshot.frames == 64 ? samples[channel][0] : 0.f;
                close(snapshot.peak[channel], expected);
                close(snapshot.rms[channel], expected);
            }
        };
        AudioMeterSnapshot snapshot;
        assert(!meter.read(snapshot));
        meter.publish(data, 64);
        assert(meter.read(snapshot));
        verify(snapshot);
        meter.publishSilence(17);
        assert(meter.read(snapshot) && snapshot.frames == 17);
        verify(snapshot);
        meter.clear();
        assert(meter.read(snapshot) && !snapshot.frames);
        verify(snapshot);

        std::atomic<bool> stop{false};
        std::atomic<unsigned> observations{0};
        std::thread observer([&] {
            while (!stop.load()) {
                AudioMeterSnapshot current;
                if (meter.read(current)) {
                    verify(current);
                    ++observations;
                }
            }
        });
        heapEvents = 0;
        trackHeap = true;
        for (unsigned block = 0; block < 10000; ++block) {
            meter.publish(data, 64);
            meter.publishSilence(17);
            meter.clear();
        }
        trackHeap = false;
        assert(!heapEvents);
        while (!observations.load())
            std::this_thread::yield();
        stop = true;
        observer.join();
        meter.publishSilence(0);
        assert(meter.read(snapshot) && !snapshot.frames);
        verify(snapshot);
    }
}
void partialChannelAvailabilityRetainsMeters() {
    class Partial final : public AudioSource {
      public:
        bool online{true};
        uint32_t channels() const noexcept override {
            return 64;
        }
        AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                             AudioReadCursor&) const noexcept override {
            for (uint32_t channel = 0; channel < channels(); ++channel)
                std::fill_n(output.channels[channel], frames, channel == 63 ? .4f : 0.f);
            // Every frame is missing some channels, but channel 64 is valid.
            return {frames, online};
        }
    };
    auto source = std::make_shared<Partial>();
    auto sink = std::make_shared<Sink>(64);
    const auto route = device("partial", SourceKind::PassThrough, "", 64);
    AudioScene scene(
        {1, {route}}, {},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto&) {
            return sink;
        });
    const auto check = [&](bool online, uint32_t frames, uint64_t first) {
        source->online = online;
        assert(render(scene, first, frames).validBlock);
        assert(scene.sourceStatus(0, 0).online == online);
        AudioMeterSnapshot input, output;
        assert(scene.sourceMeters(0, 0, input) && input.frames == frames);
        assert(scene.deviceMeters(0, output) && output.frames == frames);
        for (uint32_t channel = 0; channel < 64; ++channel) {
            const auto expected = online && channel == 63 ? .4f : 0.f;
            close(input.peak[channel], expected);
            close(input.rms[channel], expected);
            close(output.peak[channel], expected);
            close(output.rms[channel], expected);
            for (uint32_t frame = 0; frame < frames; ++frame)
                close(sink->samples[channel][frame], expected);
        }
    };
    check(true, 64, 100000);
    check(false, 17, 100064);
    check(true, 32, 100081);
    check(false, 64, 100113);
    check(true, 64, 100177);
    sink->demand = false;
    assert(!render(scene, 100241).activeDevices);
    AudioMeterSnapshot idle;
    assert(scene.sourceMeters(0, 0, idle) && !idle.frames);
}
void missingBindingsAndDisabledSources() {
    auto external = device("external");
    auto disabled = device("disabled");
    disabled.routing.enabled = false;
    auto dependent = device("dependent", SourceKind::VirtualDevice, "disabled");
    auto sink = std::make_shared<Sink>();
    unsigned resolves = 0;
    AudioScene scene(
        {1, {external, disabled, dependent}}, {},
        [&](const auto&, const auto&, const auto&) {
            ++resolves;
            return ResolvedAudioSource{};
        },
        [&](const auto&) {
            return sink;
        });
    assert(resolves == 1 && scene.deviceLatencyFrames(1) == 0);
    assert(render(scene).activeDevices == 2);
    assert(scene.sourceStatus(0, 0).missingFrames == 64);
    assert(scene.sourceStatus(2, 0).missingFrames == 64);
    assert(!scene.deviceStatus(1).active);
    close(sink->samples[0][0], 0);
    AudioMeterSnapshot meter;
    for (const auto index : {0U, 2U}) {
        assert(scene.sourceMeters(index, 0, meter) && meter.frames == 64);
        close(meter.peak[0], 0);
        close(meter.rms[1], 0);
    }
    assert(scene.document().devices.size() == 3);
    external.routing.sources[0].enabled = false;
    external.routing.sources[0].muted = true;
    external.routing.sources[0].gain = 0.35f;
    resolves = 0;
    AudioScene inactive(
        {1, {external}}, {},
        [&](const auto&, const auto&, const auto&) {
            ++resolves;
            return ResolvedAudioSource{};
        },
        [&](const auto&) {
            return sink;
        });
    render(inactive);
    assert(!resolves && !inactive.sourceStatus(0, 0).missingFrames);
    assert(inactive.sourceMeters(0, 0, meter) && meter.frames == 64);
    close(meter.peak[1], 0);
    assert(inactive.document().devices[0].routing.sources[0].gain == 0.35f);
}
void passThroughAndBadSource() {
    auto source = std::make_shared<Source>();
    auto sink = std::make_shared<Sink>();
    auto route = device("pass", SourceKind::PassThrough, "");
    AudioScene scene(
        {1, {route}}, {44100, 64, 192, 32},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto&) {
            return sink;
        });
    render(scene);
    assert(source->lastTime == frameNs(100000 - 32, 44100));
    assert(scene.deviceLatencyFrames(0) == 32);
    source->malformed = true;
    render(scene);
    close(sink->samples[0][0], 0);
    assert(!scene.sourceStatus(0, 0).online && scene.sourceStatus(0, 0).missingFrames == 64);
    source->malformed = false;
    source->value = NAN;
    render(scene);
    close(sink->samples[0][0], 0);
    AudioMeterSnapshot meter;
    assert(scene.sourceMeters(0, 0, meter));
    close(meter.peak[0], 0);
    source->value = 8;
    render(scene);
    assert(sink->samples[0][0] <= 0.98001f);
    const auto blocks = sink->blocks;
    assert(!render(scene, 100000, 65).validBlock);
    assert(!render(scene, UINT64_MAX, 64).validBlock);
    assert(!render(scene, 0, 0).validBlock);
    assert(sink->blocks == blocks && !scene.deviceStatus(0).active);
    assert(scene.deviceMeters(0, meter) && meter.frames == 0);
    assert(!scene.deviceMeters(9, meter) && !scene.sourceMeters(0, 9, meter));
    assert(!scene.input(9, 0) && !scene.input(0, 9));
    rejects([&] {
        AudioScene invalid({1, {route}}, {48000, 16}, {}, {});
    });
    rejects([&] {
        AudioScene invalid({1, {route}}, {}, {}, [&](const auto&) {
            return std::make_shared<Sink>(4);
        });
    });
    rejects([&] {
        AudioScene invalid({1, {route}}, {},
                           [&](const auto&, const auto&, const auto&) {
                               return ResolvedAudioSource{std::make_shared<Source>(1), {}};
                           },
                           {});
    });
}
void maximumGraphAndConcurrentMeters() {
    RouteDocument desired;
    std::vector<std::shared_ptr<Sink>> sinks;
    auto source = std::make_shared<Source>(64);
    source->value = 0.001f;
    for (unsigned i = 0; i < kMaximumVirtualDevices; ++i) {
        desired.devices.push_back(device(
            "device-" + std::to_string(i), i ? SourceKind::VirtualDevice : SourceKind::InputDevice,
            i ? "device-" + std::to_string(i - 1) : "physical-64", 64));
        sinks.push_back(std::make_shared<Sink>(64));
        sinks.back()->demand = i == kMaximumVirtualDevices - 1;
    }
    AudioScene scene(
        desired, {},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, {}};
        },
        [&](const auto& definition) {
            return sinks[std::stoul(definition.id.substr(7))];
        });
    std::atomic<bool> stop{false};
    std::atomic<unsigned> observations{0};
    std::thread observer([&] {
        while (!stop.load()) {
            AudioMeterSnapshot meter;
            if (scene.deviceMeters(31, meter) && meter.frames) {
                assert(meter.channels == 64 && meter.frames == 64);
                const float base = meter.peak[0];
                assert(std::fabs(base - 0.001f) < 0.00001f || std::fabs(base - 0.002f) < 0.00001f);
                for (unsigned channel = 0; channel < 64; ++channel) {
                    close(meter.peak[channel], base * (channel + 1));
                    close(meter.rms[channel], base * (channel + 1));
                }
                ++observations;
            }
        }
    });
    constexpr unsigned kBlocks = 64;
    for (unsigned block = 0; block < kBlocks; ++block) {
        source->value = block % 2 ? 0.002f : 0.001f;
        const auto result = render(scene, 100000 + block * 64);
        assert(result.activeDevices == 32 && !result.failedGraphs && !result.failedSinks);
    }
    while (!observations.load())
        std::this_thread::yield();
    stop = true;
    observer.join();
    close(sinks.back()->samples[63][63], 0.128f);
    assert(source->reads == kBlocks); // One capture read for the whole dependency chain.
    assert(scene.deviceLatencyFrames(31) == 1024);
}
void publicationAndCursorOwnership() {
    const auto control = std::this_thread::get_id();
    unsigned destroyed = 0;
    struct Tracked {
        unsigned id;
        std::thread::id control;
        unsigned& destroyed;
        std::array<unsigned, 64> payload;
        Tracked(unsigned value, std::thread::id owner, unsigned& count)
            : id(value), control(owner), destroyed(count) {
            payload.fill(id);
        }
        ~Tracked() {
            assert(std::this_thread::get_id() == control);
            ++destroyed;
        }
    };
    RealtimePublication<Tracked> publication(1);
    auto stage = [&](unsigned id) {
        return publication.stage(std::make_unique<Tracked>(id, control, destroyed));
    };
    assert(!publication.borrow());
    {
        auto first = stage(1);
        first->commit();
    }
    {
        auto held = publication.borrow();
        assert(held->id == 1);
        {
            auto second = stage(2);
            second->commit();
            second->commit();
        }
        publication.collect();
        assert(!destroyed && held->id == 1 && publication.borrow()->id == 2);
        rejects([&] {
            stage(3);
        });                     // Bounded backpressure with an outstanding reader.
        assert(destroyed == 1); // Only the unpublishable candidate was discarded.
    }
    publication.collect();
    assert(destroyed == 2);
    {
        auto abandoned = stage(4);
    }
    assert(publication.borrow()->id == 2 && destroyed == 3);
    std::atomic<bool> stop{false}, started{false};
    std::thread reader([&] {
        started = true;
        while (!stop.load()) {
            trackHeap = true;
            heapEvents = 0;
            {
                auto guard = publication.borrow();
                for (const auto value : guard->payload)
                    assert(value == guard->id);
            }
            trackHeap = false;
            assert(!heapEvents);
        }
    });
    while (!started.load())
        std::this_thread::yield();
    for (unsigned id = 5; id < 505; ++id) {
        for (;;) {
            try {
                auto next = stage(id);
                next->commit();
                break;
            } catch (const std::runtime_error&) {
                std::this_thread::yield();
            }
        }
    }
    stop = true;
    reader.join();
    publication.collect();
    assert(publication.borrow()->id == 504);

    RealtimePublication<AudioScene> scenes;
    auto source = std::make_shared<Source>();
    auto cursor = std::make_shared<AudioReadCursor>();
    auto sink = std::make_shared<Sink>();
    RouteDocument desired{1, {device("persistent")}};
    auto prepare = [&] {
        return scenes.stage(std::make_unique<AudioScene>(
            desired, SceneTiming{},
            [&](const auto&, const auto&, const auto&) {
                return ResolvedAudioSource{source, cursor};
            },
            [&](const auto&) {
                return sink;
            }));
    };
    {
        auto next = prepare();
        next->commit();
    }
    {
        auto scene = scenes.borrow();
        render(*scene.get());
    }
    assert(cursor->generation == 1);
    desired.devices[0].routing.gain = 0.5f;
    auto candidate = prepare();
    source->online = false;
    {
        auto scene = scenes.borrow();
        render(*scene.get());
    }
    assert(cursor->generation == 2);
    candidate->commit();
    candidate.reset();
    source->online = true;
    {
        auto scene = scenes.borrow();
        render(*scene.get());
    }
    assert(cursor->generation == 3);
    close(sink->samples[0][0], 0.1f);
    scenes.collect();
}
} // namespace
void committedUpdatesDoNotReleaseNewerReservations() {
    RealtimePublication<int> publication;
    auto first = publication.stage(std::make_unique<int>(1));
    first->commit();
    auto second = publication.stage(std::make_unique<int>(2));
    first.reset();
    rejects([&] {
        publication.stage(std::make_unique<int>(3));
    });
    second->commit();
    auto third = publication.stage(std::make_unique<int>(3));
    second.reset();
    rejects([&] {
        publication.stage(std::make_unique<int>(4));
    });
    third->commit();
    assert(*publication.borrow().get() == 3);
}
int main(int argc, char** argv) {
    const std::string_view group = argc == 1 ? "all" : argv[1];
    if (argc > 2 ||
        (group != "all" && group != "base" && group != "timing" && group != "concurrency")) {
        std::fputs("Usage: loopz-audio-scene-tests [all|base|timing|concurrency]\n", stderr);
        return 2;
    }
    const auto run = [](const char* name, auto check) {
        const auto begun = std::chrono::steady_clock::now();
        check();
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - begun)
                                      .count();
        std::printf("%s: %lld ms\n", name, static_cast<long long>(milliseconds));
        std::fflush(stdout);
    };
    if (group == "all" || group == "timing") {
        run("Input budgets preserve mixed-path alignment", inputBudgetsPreserveAlignment);
        run("Aligned paths and ring wrap", alignedPathsAndHistoryWrap);
        run("Prepared scene continuity", compensationSurvivesPreparedSceneChanges);
        run("Maximum delay", maximumCompensationDelay);
        run("Frame-zero underflow", delayUnderflowIsSilence);
    }
    if (group == "all" || group == "concurrency")
        run("Concurrent preparation", concurrentPreparationRetainsLiveHistory);
    if (group == "all" || group == "base") {
        run("Pair removal preserves dependent audio", removedPairPreservesDependentAudio);
        run("Publication reservations", committedUpdatesDoNotReleaseNewerReservations);
        run("Dependency demand", dependencyDemandAndRecovery);
        run("Unwired source preview", unwiredSourcePreview);
        run("Silent meter blocks and concurrent observers",
            silentMeterBlocksAndConcurrentObservers);
        run("Partial channel availability retains meters", partialChannelAvailabilityRetainsMeters);
        run("Disabled and missing sources", missingBindingsAndDisabledSources);
        run("Malformed source isolation", passThroughAndBadSource);
        run("Maximum graph and concurrent meters", maximumGraphAndConcurrentMeters);
        run("Cursor publication", publicationAndCursorOwnership);
    }
    std::printf("Audio scene %.*s checks passed\n", static_cast<int>(group.size()), group.data());
}
