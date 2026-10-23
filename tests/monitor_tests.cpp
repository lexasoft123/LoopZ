#include <loopz/monitor.hpp>
#include <loopz/timeline.hpp>
#include <loopz/output_clock.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <string_view>

namespace {
thread_local bool trackHeap = false;
thread_local unsigned heapEvents = 0;
} // namespace
// Keep allocation instrumentation out of line: GCC otherwise diagnoses the
// malloc/free implementation of matching replacement new/delete as a mismatch.
#if defined(__GNUC__)
#define LOOPZ_HEAP_HOOK __attribute__((noinline))
#else
#define LOOPZ_HEAP_HOOK
#endif
LOOPZ_HEAP_HOOK void* operator new(std::size_t size) {
    if (trackHeap)
        ++heapEvents;
    if (auto* pointer = std::malloc(size ? size : 1))
        return pointer;
    throw std::bad_alloc();
}
LOOPZ_HEAP_HOOK void* operator new[](std::size_t size) {
    return ::operator new(size);
}
LOOPZ_HEAP_HOOK void operator delete(void* pointer) noexcept {
    if (trackHeap && pointer)
        ++heapEvents;
    std::free(pointer);
}
LOOPZ_HEAP_HOOK void operator delete[](void* pointer) noexcept {
    ::operator delete(pointer);
}
LOOPZ_HEAP_HOOK void operator delete(void* pointer, std::size_t) noexcept {
    ::operator delete(pointer);
}
LOOPZ_HEAP_HOOK void operator delete[](void* pointer, std::size_t) noexcept {
    ::operator delete(pointer);
}

#undef LOOPZ_HEAP_HOOK

namespace {
using namespace loopz;
void near(float actual, float expected) {
    assert(std::isfinite(actual) && std::fabs(actual - expected) < 0.0001f);
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
template <typename Operation> auto realtime(Operation operation) {
    heapEvents = 0;
    trackHeap = true;
    auto result = operation();
    trackHeap = false;
    assert(heapEvents == 0);
    return result;
}
struct Planes {
    std::vector<std::vector<float>> samples;
    std::vector<float*> writable;
    std::vector<const float*> readable;
    explicit Planes(uint32_t channels, uint32_t frames = kBlock)
        : samples(channels, std::vector<float>(frames)), writable(channels), readable(channels) {
        for (uint32_t channel = 0; channel < channels; ++channel)
            readable[channel] = writable[channel] = samples[channel].data();
    }
    RoutingInput input() const {
        return {readable, static_cast<uint32_t>(samples[0].size())};
    }
    RoutingOutput output() {
        return {writable, static_cast<uint32_t>(samples[0].size())};
    }
    void fill(float value) {
        for (auto& plane : samples)
            std::fill(plane.begin(), plane.end(), value);
    }
};
struct PublicSink final : AudioSink {
    const uint32_t count;
    bool demand{false}, healthy{true};
    unsigned blocks{0};
    Planes samples;
    explicit PublicSink(uint32_t channels = 2) : count(channels), samples(channels) {}
    uint32_t channels() const noexcept override {
        return count;
    }
    bool requested() const noexcept override {
        return demand;
    }
    bool publish(uint64_t, uint32_t, RoutingInput input, uint32_t frames) noexcept override {
        ++blocks;
        for (uint32_t channel = 0; channel < count; ++channel)
            std::copy_n(input.channels[channel], frames, samples.writable[channel]);
        return healthy;
    }
    void idle() noexcept override {}
};
VirtualDeviceDefinition device(uint32_t channels = 2) {
    VirtualDeviceDefinition result;
    result.id = "mix";
    result.name = "Stream Mix";
    result.routing.outputChannels = channels;
    result.monitors = {{"headphones", "hardware", channels, 0.5f, true, false,
                        defaultChannelConnections(channels, channels)}};
    return result;
}
void publish(MonitorBus& bus, Planes& planes, uint64_t first = 48000, uint32_t blocks = 16) {
    for (uint32_t block = 0; block < blocks; ++block)
        assert(realtime([&] {
            return bus.publish(first + block * kBlock, bus.renderRate(), planes.input(), kBlock);
        }));
}
void directMonitorPreservesTheCompleteMix() {
    auto route = device();
    route.bindings = {{"mic-a", SourceKind::InputDevice, "hardware", false},
                      {"mic-b", SourceKind::InputDevice, "hardware", false}};
    route.routing.sources = {
        {"mic-a", 8, 0.8f, true, false, {{2, 0, 1}, {3, 1, 0.75f}, {2, 1, 0.25f}}},
        {"mic-b", 8, 0.6f, true, false, {{2, 0, 0.5f}, {6, 1, 1}}}};
    route.routing.gain = 0.75f;
    route.monitors[0].connections = {{0, 1, 1}, {1, 0, 0.5f}};
    assert(directMonitorInputChannels(route, "hardware") == 8);
    for (double rate : {44100., 48000., 96000.}) {
        for (bool muted : {false, true}) {
            route.monitors[0].muted = muted;
            DirectMonitorProgram direct(route, route.monitors[0], "hardware", 4, rate);
            RoutingGraph deviceGraph(rate, route.routing);
            RoutingGraph monitorGraph(rate, monitorRouting(route, route.monitors[0]));
            Planes physical(4, 701), actual(2, 701), expected(2, 701), zeroPadded(8), mixed(2),
                rendered(2);
            for (uint32_t frame = 0; frame < 701; ++frame) {
                physical.samples[2][frame] = frame % 3 ? 0.3f : 4.f;
                physical.samples[3][frame] = frame % 2 ? -0.7f : 0.2f;
            }
            const auto result = realtime([&] {
                return direct.render(physical.input(), actual.output(), 701, 7000);
            });
            assert(result.validBlock && !result.missingFrames && !result.failedChunks);
            for (uint32_t offset = 0; offset < 701; offset += kBlock) {
                const auto count = std::min(kBlock, 701 - offset);
                for (uint32_t channel = 0; channel < 4; ++channel)
                    std::copy_n(physical.readable[channel] + offset, count,
                                zeroPadded.writable[channel]);
                const RoutingInput sources[]{zeroPadded.input(), zeroPadded.input()};
                assert(deviceGraph.render(sources, mixed.output(), count, 7000 + offset));
                const auto mix = mixed.input();
                assert(monitorGraph.render({&mix, 1}, rendered.output(), count, 7000 + offset));
                for (uint32_t channel = 0; channel < 2; ++channel)
                    std::copy_n(rendered.readable[channel], count,
                                expected.writable[channel] + offset);
            }
            for (uint32_t channel = 0; channel < 2; ++channel)
                for (uint32_t frame = 0; frame < 701; ++frame)
                    near(actual.samples[channel][frame], expected.samples[channel][frame]);
            assert(direct.status().callbacks == 1 && !direct.status().invalidBlocks);
            assert(direct.equivalentDevice(route));
            auto changed = route;
            changed.name = "Metadata only";
            changed.sampleRate = 192000;
            assert(direct.equivalentDevice(changed));
            changed.routing.gain = 0.5f;
            assert(!direct.equivalentDevice(changed));
            changed = route;
            changed.routing.sources[0].connections[0].gain = 0.4f;
            assert(!direct.equivalentDevice(changed));
            AudioMeterSnapshot meter;
            assert(direct.meters(meter) && meter.channels == 2 && meter.frames == 189);
            if (muted)
                near(meter.peak[0], 0);
        }
    }
    route.bindings[1].kind = SourceKind::Application;
    assert(!directMonitorInputChannels(route, "hardware"));
    route.routing.sources[1].muted = true;
    assert(directMonitorInputChannels(route, "hardware") == 8);
    route.routing.sources[0].connections.clear();
    assert(!directMonitorInputChannels(route, "hardware"));
    rejects([&] {
        DirectMonitorProgram invalid(route, route.monitors[0], "hardware", 4, 48000);
    });
}

void independentRatesWiringAndMeters() {
    auto route = device();
    auto bus = std::make_shared<MonitorBus>(2, 48000);
    Planes input(2);
    std::fill(input.samples[0].begin(), input.samples[0].end(), 0.2f);
    std::fill(input.samples[1].begin(), input.samples[1].end(), 0.4f);
    publish(*bus, input);
    auto headphones = route.monitors[0];
    headphones.connections = {{0, 1, 1}, {1, 0, 0.5f}};
    auto speakers = headphones;
    speakers.gain = 0.25f;
    MonitorProgram a(route, headphones, bus->source(), {48000, 48000, 1024});
    MonitorProgram b(route, speakers, bus->source(), {48000, 44100, 2048});
    Planes outputA(2, 701), outputB(2, 1024);
    const auto timeA = frameNs(50048, 48000);
    const auto timeB = frameNs(51072, 48000);
    const auto ra = realtime([&] {
        return a.render(timeA, 48000, outputA.output(), 701, 100);
    });
    const auto rb = realtime([&] {
        return b.render(timeB, 44100, outputB.output(), 1024, 800);
    });
    assert(ra.validBlock && ra.sourceOnline && !ra.missingFrames && !ra.failedChunks);
    assert(rb.validBlock && rb.sourceOnline && !rb.missingFrames && !rb.failedChunks);
    for (const auto& plane : outputA.samples)
        for (float value : plane)
            near(value, 0.1f);
    for (const auto& plane : outputB.samples)
        for (float value : plane)
            near(value, 0.05f);
    AudioMeterSnapshot meter;
    assert(a.meters(meter) && meter.channels == 2 && meter.frames == 189);
    near(meter.peak[0], 0.1f);
    near(meter.rms[1], 0.1f);
    // Reading the same history again does not consume the second monitor's data.
    assert(realtime([&] {
               return a.render(timeA, 48000, outputA.output(), 701, 100);
           }).missingFrames == 0);
    auto status = a.status();
    assert(status.callbacks == 2 && status.missingFrames == 0 && status.graphFailures == 0);
}
void changingSignalRateConversion() {
    auto route = device();
    auto bus = std::make_shared<MonitorBus>(2, 48000);
    Planes input(2), output(2, 701);
    for (uint32_t block = 0; block < 16; ++block) {
        for (uint32_t frame = 0; frame < kBlock; ++frame) {
            input.samples[0][frame] = (block * kBlock + frame) * 0.0001f;
            input.samples[1][frame] = -input.samples[0][frame];
        }
        assert(bus->publish(48000 + block * kBlock, 48000, input.input(), kBlock));
    }
    // Three DSP chunks include noninteger sample positions at both boundaries.
    MonitorProgram monitor(route, route.monitors[0], bus->source(), {48000, 44100, 1000});
    auto result = realtime([&] {
        return monitor.render(frameNs(50000, 48000), 44100, output.output(), 701, 44000);
    });
    assert(result.validBlock && !result.missingFrames && !result.failedChunks);
    for (uint32_t frame = 0; frame < 701; ++frame) {
        const float expected = static_cast<float>((1000 + frame * 48000.0 / 44100) * 0.00005);
        near(output.samples[0][frame], expected);
        near(output.samples[1][frame], -expected);
    }
}

void publicationChunksPreserveSignalAndClock() {
    auto route = device();
    auto bus = std::make_shared<MonitorBus>(2, 48000);
    Planes input(2);
    for (uint32_t block = 0; block < 16; ++block) {
        for (uint32_t frame = 0; frame < kBlock; ++frame) {
            input.samples[0][frame] = (block * kBlock + frame) * 0.0001f;
            input.samples[1][frame] = -input.samples[0][frame];
        }
        assert(bus->publish(48000 + block * kBlock, 48000, input.input(), kBlock));
    }
    for (const auto rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        const auto frames = rate / 100;
        const auto time = frameNs(50000, 48000);
        MonitorProgram whole(route, route.monitors[0], bus->source(), {48000, double(rate), 1000});
        MonitorProgram partitioned(route, route.monitors[0], bus->source(), {48000, double(rate), 1000});
        Planes expected(2, frames), actual(2, frames);
        const auto full = realtime([&] {
            return whole.render(time, rate, expected.output(), frames, 44000);
        });
        assert(full.validBlock && !full.missingFrames && !full.failedChunks);
        for (uint32_t offset = 0; offset < frames; offset += 256) {
            const auto count = std::min(frames - offset, 256u);
            std::array<float*, 2> planes{actual.writable[0] + offset, actual.writable[1] + offset};
            const auto part = realtime([&] {
                return partitioned.render(time + frameNs(offset, rate), rate,
                                          {planes, count}, count, 44000 + offset);
            });
            assert(part.validBlock && !part.missingFrames && !part.failedChunks);
        }
        for (uint32_t channel = 0; channel < 2; ++channel)
            for (uint32_t frame = 0; frame < frames; ++frame)
                assert(std::isfinite(actual.samples[channel][frame]) &&
                       std::abs(actual.samples[channel][frame] - expected.samples[channel][frame]) < 1e-7f);
    }
}

struct ConstantSource final : AudioSource {
    mutable unsigned reads{0};
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        ++reads;
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0.4f);
        return {0, true};
    }
};
void monitorDemandRunsVirtualDependenciesWithoutPublicClients() {
    auto upstream = device(), downstream = device();
    upstream.id = "upstream";
    upstream.monitors.clear();
    upstream.routing.sources = {{"mic", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    upstream.bindings = {{"mic", SourceKind::InputDevice, "physical-input"}};
    downstream.id = "downstream";
    downstream.routing.sources = {
        {"virtual", 2, 0.5f, true, false, defaultChannelConnections(2, 2)}};
    downstream.bindings = {{"virtual", SourceKind::VirtualDevice, "upstream"}};
    auto source = std::make_shared<ConstantSource>();
    MonitorBuses buses(48000);
    auto capture = std::make_shared<PublicSink>();
    std::shared_ptr<MonitoringSink> downstreamSink;
    AudioScene scene(
        {1, {downstream, upstream}}, {},
        [&](const auto&, const auto&, const auto&) -> ResolvedAudioSource {
            return {source, {}};
        },
        [&](const auto& definition) -> std::shared_ptr<AudioSink> {
            auto sink = buses.decorate(definition, capture);
            if (definition.id == "downstream")
                downstreamSink = sink;
            return sink;
        });
    assert(scene.requestedDevices() == 3);
    for (uint32_t block = 0; block < 16; ++block) {
        auto result = realtime([&] {
            return scene.render(48000 + block * kBlock, kBlock);
        });
        assert(result.validBlock && result.activeDevices == 2 && !result.failedGraphs);
    }
    assert(source->reads == 16 && scene.demandedDevices() == 3);
    MonitorProgram monitor(downstream, downstream.monitors[0], downstreamSink->bus()->source(), {});
    Planes output(2, 64);
    auto result = realtime([&] {
        return monitor.render(frameNs(50048, 48000), 48000, output.output(), 64, 0);
    });
    assert(result.validBlock && !result.missingFrames);
    near(output.samples[0][0], 0.1f);
}
void largeCallbackAndPhysicalChannelChanges() {
    auto route = device(64);
    auto bus = std::make_shared<MonitorBus>(64, 48000);
    Planes input(64), narrow(2, 8192), wide(64, 64);
    input.fill(0.4f);
    publish(*bus, input, 48000, 64);
    MonitorProgram monitor(route, route.monitors[0], bus->source(), {48000, 48000, 1024});
    const auto time = frameNs(50048, 48000);
    auto result = realtime([&] {
        return monitor.render(time, 48000, narrow.output(), 8192, 0);
    });
    assert(result.validBlock && !result.missingFrames && !result.failedChunks);
    for (const auto& plane : narrow.samples)
        for (float sample : plane)
            near(sample, 0.2f);
    AudioMeterSnapshot meter;
    assert(monitor.meters(meter) && meter.channels == 64);
    near(meter.peak[1], 0.2f);
    near(meter.peak[2], 0);
    near(meter.rms[63], 0);
    // The desired map survives a narrower profile and returns unchanged.
    assert(realtime([&] {
               return monitor.render(time, 48000, wide.output(), 64, 0);
           }).validBlock);
    for (const auto& plane : wide.samples)
        near(plane[0], 0.2f);
    auto stereo = device();
    auto stereoBus = std::make_shared<MonitorBus>(2, 48000);
    Planes stereoInput(2), extraOutputs(8, 64);
    stereoInput.fill(0.4f);
    publish(*stereoBus, stereoInput);
    MonitorProgram stereoMonitor(stereo, stereo.monitors[0], stereoBus->source(), {});
    extraOutputs.fill(9);
    assert(stereoMonitor.render(time, 48000, extraOutputs.output(), 64, 0).validBlock);
    near(extraOutputs.samples[1][0], 0.2f);
    for (uint32_t channel = 2; channel < 8; ++channel)
        near(extraOutputs.samples[channel][0], 0);
}
void absenceAndRestartAreSilenceNotHardwareFailure() {
    auto route = device();
    auto bus = std::make_shared<MonitorBus>(2, 48000);
    Planes input(2), output(2, 128);
    input.fill(0.4f);
    MonitorProgram monitor(route, route.monitors[0], bus->source(), {});
    const auto time = frameNs(50048, 48000);
    auto absent = monitor.render(time, 48000, output.output(), 128, 0);
    assert(absent.validBlock && !absent.sourceOnline && absent.missingFrames == 128);
    publish(*bus, input);
    assert(monitor.render(time, 48000, output.output(), 128, 128).missingFrames == 0);
    near(output.samples[0][0], 0.2f);
    bus->idle();
    auto stopped = monitor.render(time, 48000, output.output(), 128, 256);
    assert(stopped.validBlock && !stopped.sourceOnline && stopped.missingFrames == 128);
    for (const auto& plane : output.samples)
        for (float sample : plane)
            near(sample, 0);
    // Identical hardware positions/times after restart cannot resurrect old data.
    input.fill(0.6f);
    publish(*bus, input);
    assert(monitor.render(time, 48000, output.output(), 128, 384).missingFrames == 0);
    near(output.samples[0][0], 0.3f);
    auto underrun = monitor.render(frameNs(90000, 48000), 48000, output.output(), 128, 512);
    assert(underrun.validBlock && underrun.sourceOnline && underrun.missingFrames == 128);
    auto muted = route.monitors[0];
    muted.muted = true;
    MonitorProgram silence(route, muted, bus->source(), {});
    assert(silence.render(time, 48000, output.output(), 128, 0).validBlock);
    near(output.samples[0][0], 0);
}
void cachePreservesHistoryAndPendingDoesNotMutate() {
    MonitorBuses buses(48000);
    auto route = device();
    auto capture = std::make_shared<PublicSink>();
    auto current = buses.decorate(route, capture);
    assert(current->requested());
    route.monitors[0].muted = true;
    route.sampleRate = 44100;
    auto pendingCapture = std::make_shared<PublicSink>();
    auto pending = buses.decorate(route, pendingCapture);
    assert(pending->bus() == current->bus() && pending->requested());
    Planes input(2), output(2, 64);
    input.fill(0.4f);
    capture->healthy = false;
    for (uint32_t block = 0; block < 16; ++block)
        assert(!realtime([&] {
            return current->publish(48000 + block * 256, 48000, input.input(), 256);
        }));
    MonitorProgram monitor(route, route.monitors[0], pending->bus()->source(), {});
    pending.reset(); // An aborted configuration must not idle the committed bus.
    AudioReadCursor cursor;
    auto read =
        current->bus()->source()->read(frameNs(49024, 48000), 48000, output.output(), 64, cursor);
    assert(read.online && !read.missingFrames);
    near(output.samples[0][0], 0.4f);
    assert(capture->blocks == 16 && pendingCapture->blocks == 0);
    route.monitors[0].enabled = false;
    auto disabled = buses.decorate(route, pendingCapture);
    assert(!disabled->requested());
    pendingCapture->demand = true;
    assert(disabled->requested());
    auto resized = device(8);
    assert(buses.decorate(resized, std::make_shared<PublicSink>(8))->bus() != current->bus());
    auto another = device();
    another.id = "another";
    assert(buses.decorate(another, std::make_shared<PublicSink>())->bus() != current->bus());
}
void malformedInputAndMeter() {
    auto route = device();
    auto bus = std::make_shared<MonitorBus>(2, 48000);
    rejects([&] {
        MonitorBus invalid(2, 0);
    });
    rejects([&] {
        MonitorBuses invalid(192001);
    });
    rejects([&] {
        MonitorProgram invalid(route, route.monitors[0], nullptr, {});
    });
    rejects([&] {
        MonitorProgram invalid(route, route.monitors[0], bus->source(), {48000, 0, 0});
    });
    rejects([&] {
        MonitorProgram invalid(route, route.monitors[0], bus->source(), {48000, 48000, kCapacity});
    });
    rejects([&] {
        MonitoringSink invalid(nullptr, bus, true);
    });
    rejects([&] {
        MonitoringSink invalid(std::make_shared<PublicSink>(8), bus, true);
    });
    Planes input(2), output(2, 128);
    input.fill(0.4f);
    input.samples[0][0] = std::numeric_limits<float>::quiet_NaN();
    publish(*bus, input);
    MonitorProgram monitor(route, route.monitors[0], bus->source(), {});
    const auto time = frameNs(50048, 48000);
    for (double wrongRate : {0.0, 44100.0, std::numeric_limits<double>::quiet_NaN()}) {
        output.fill(9);
        auto result = realtime([&] {
            return monitor.render(time, wrongRate, output.output(), 128, 0);
        });
        assert(!result.validBlock);
        for (const auto& plane : output.samples)
            for (float value : plane)
                near(value, 0);
    }
    output.fill(9);
    assert(!monitor.render(1, 48000, output.output(), 128, 0).validBlock);
    assert(!monitor.render(time, 48000, output.output(), 129, 0).validBlock);
    assert(!monitor.render(time, 48000, output.output(), 128, UINT64_MAX).validBlock);
    for (const auto wrongClock :
         {-1., 48000 * 0.994, 48000 * 1.006, std::numeric_limits<double>::quiet_NaN()}) {
        output.fill(9);
        assert(!monitor.render(time, 48000, output.output(), 128, 0, wrongClock).validBlock);
        assert(monitor.status().measuredOutputRate == 0);
        for (const auto& plane : output.samples)
            for (const auto value : plane)
                near(value, 0);
    }
    assert(!bus->publish(UINT64_MAX, 48000, input.input(), 256));
    assert(!bus->publish(48000, 44100, input.input(), 256));
    AudioMeter meter(2);
    AudioMeterSnapshot observation;
    rejects([] {
        AudioMeter invalid(0);
    });
    rejects([] {
        AudioMeter invalid(65);
    });
    assert(!meter.read(observation));
    meter.publish(input.input(), 256);
    assert(meter.read(observation));
    near(observation.peak[0], 0.4f);
    assert(std::isfinite(observation.rms[0]));
    meter.publish(input.input(), 0);
    assert(meter.read(observation) && observation.frames == 0);
    near(observation.peak[0], 0);
}
void physicalClockDrift(double outputRate, bool quantized, double ppm) {
    constexpr uint32_t frames = 64, sourceRate = 48000;
    constexpr uint64_t originNs = 1000000000;
    constexpr uint32_t delay = 1024;
    const auto actual = outputRate * (1 + ppm / 1e6);
    const auto frequency = outputRate == 8000 ? 3000. : 20000.;
    auto route = device();
    route.monitors[0].gain = 1;
    auto bus = std::make_shared<MonitorBus>(2, sourceRate);
    MonitorProgram corrected(route, route.monitors[0], bus->source(),
                             {sourceRate, outputRate, delay});
    MonitorProgram nominal(route, route.monitors[0], bus->source(),
                           {sourceRate, outputRate, delay});
    OutputClock clock;
    Planes input(2), correctedAudio(2, frames), nominalAudio(2, frames);
    uint64_t published = sourceRate;
    long double correctedError = 0, nominalError = 0, energy = 0;
    double maximumError = 0;
    // Independent physical output and graph clocks. Publish only the history
    // available by each callback, then read the same bus with both algorithms.
    const auto blocks = static_cast<uint64_t>(std::ceil(outputRate * 0.5 / frames));
    for (uint64_t index = 0; index < blocks; ++index) {
        const auto elapsedNs = static_cast<long double>(index * frames) * 1e9L / actual;
        const auto host =
            originNs + static_cast<uint64_t>(
                           quantized ? std::round(elapsedNs * 24 / 1000) * 1000 / 24 : elapsedNs);
        const auto through = sourceRate + static_cast<uint64_t>(elapsedNs * sourceRate / 1e9L);
        while (published <= through) {
            for (uint32_t frame = 0; frame < kBlock; ++frame) {
                const auto value = static_cast<float>(
                    0.25 *
                    std::sin(2 * std::acos(-1.) * frequency *
                             static_cast<double>(published + frame - sourceRate) / sourceRate));
                input.samples[0][frame] = input.samples[1][frame] = value;
            }
            assert(realtime([&] {
                return bus->publish(published, sourceRate, input.input(), kBlock);
            }));
            published += kBlock;
        }
        const auto rate = realtime([&] {
            return clock.observe(index * frames, host, frames, outputRate);
        });
        const auto measured = realtime([&] {
            return corrected.render(host, outputRate, correctedAudio.output(), frames,
                                    index * frames, rate);
        });
        const auto old = realtime([&] {
            return nominal.render(host, outputRate, nominalAudio.output(), frames, index * frames);
        });
        if (elapsedNs < 150000000)
            continue;
        assert(measured.validBlock && measured.sourceOnline && !measured.missingFrames &&
               !measured.failedChunks && old.validBlock && !old.missingFrames);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const auto time = (elapsedNs + frame * 1e9L / actual) / 1e9L -
                              static_cast<long double>(delay) / sourceRate;
            const auto expected = 0.25 * std::sin(2 * std::acos(-1.) * frequency * time);
            const auto difference = correctedAudio.samples[0][frame] - expected;
            correctedError += difference * difference;
            const auto oldDifference = nominalAudio.samples[0][frame] - expected;
            nominalError += oldDifference * oldDifference;
            energy += expected * expected;
            maximumError = std::max(maximumError, static_cast<double>(std::abs(difference)));
            near(correctedAudio.samples[0][frame], correctedAudio.samples[1][frame]);
        }
    }
    const auto ratio = std::sqrt(correctedError / nominalError);
    const auto snr = 10 * std::log10(energy / correctedError);
    std::printf("monitor %.0f Hz %.0f ppm, %s timestamps: SNR %.2Lf dB, error ratio %.5Lf, "
                "maximum %.8f\n",
                outputRate, ppm, quantized ? "42 ns" : "1 ns", snr, ratio, maximumError);
    assert(std::isfinite(snr) && snr > (quantized ? 45 : 75));
    assert(ratio < (quantized ? 0.5 : 0.03));
}
void concurrentHistoryReadersAndMeterObservers() {
    auto route = device();
    auto bus = std::make_shared<MonitorBus>(2, 48000);
    MonitorProgram a(route, route.monitors[0], bus->source(), {});
    MonitorProgram b(route, route.monitors[0], bus->source(), {48000, 44100, 1024});
    std::atomic<uint64_t> latest{0};
    std::atomic<bool> finished{false};
    std::atomic<unsigned> ready{0};
    auto reader = [&](MonitorProgram& monitor) {
        Planes output(2, 64);
        ++ready;
        while (ready.load() < 3)
            std::this_thread::yield();
        uint64_t position = 0;
        do {
            const auto first = latest.load();
            if (first) {
                const auto result = realtime([&] {
                    return monitor.render(frameNs(first + 1024, 48000), monitor.timing().outputRate,
                                          output.output(), 64, position);
                });
                assert(result.validBlock && !result.failedChunks);
                for (uint32_t frame = 0; frame < 64; ++frame) {
                    // Producer always writes equal channels. A racing history
                    // overwrite may zero a frame, but must never tear its planes.
                    near(output.samples[0][frame], output.samples[1][frame]);
                    // A windowed-sinc response can overshoot a discontinuous
                    // positive step. It must stay finite and channel-coherent.
                    assert(output.samples[0][frame] >= -0.1f && output.samples[0][frame] <= 0.4f);
                }
                position += 64;
            }
        } while (!finished.load());
    };
    std::thread first([&] {
        reader(a);
    }),
        second([&] {
            reader(b);
        });
    std::thread observer([&] {
        ++ready;
        do {
            AudioMeterSnapshot meter;
            if (a.meters(meter)) {
                assert(meter.channels == 2 && meter.frames == 64);
                near(meter.peak[0], meter.peak[1]);
                near(meter.rms[0], meter.rms[1]);
            }
        } while (!finished.load());
    });
    while (ready.load() < 3)
        std::this_thread::yield();
    Planes input(2);
    for (uint32_t block = 0; block < 800; ++block) {
        input.fill(block % 2 ? 0.4f : 0.6f);
        const auto position = 48000 + block * kBlock;
        assert(realtime([&] {
            return bus->publish(position, 48000, input.input(), kBlock);
        }));
        latest.store(position);
    }
    finished.store(true);
    first.join();
    second.join();
    observer.join();
}
} // namespace
int main(int argc, char** argv) {
    directMonitorPreservesTheCompleteMix();
    if (argc == 2 && std::string_view(argv[1]) == "clock-signal") {
        for (const auto rate : {8000., 44100., 48000., 192000.}) {
            physicalClockDrift(rate, false, rate == 8000 ? -200 : 200);
            physicalClockDrift(rate, true, rate == 8000 ? -200 : 200);
        }
        std::puts("Measured monitor clock signal comparisons passed");
        return 0;
    }
    assert(argc == 1);
    // Diagnostic reads remain opt-in and report unavailable audio without
    // allocating in the realtime read path.
    for (const bool enabled : {false, true}) {
        MonitorBus bus(2, 48000, 32768, enabled);
        const auto history = std::dynamic_pointer_cast<const AudioHistory>(bus.source());
        assert(history && history->readDiagnostics().enabled == enabled);
        AudioReadCursor cursor;
        Planes output(2);
        realtime([&] {
            return history->read(1000000000, 48000, output.output(), kBlock, cursor);
        });
        const auto diagnostics = history->readDiagnostics();
        assert(diagnostics.reads == (enabled ? 1 : 0));
        assert(diagnostics.missingFrames[static_cast<size_t>(AudioHistoryMissingReason::Offline)] ==
               (enabled ? kBlock : 0));
    }
    independentRatesWiringAndMeters();
    changingSignalRateConversion();
    publicationChunksPreserveSignalAndClock();
    monitorDemandRunsVirtualDependenciesWithoutPublicClients();
    largeCallbackAndPhysicalChannelChanges();
    absenceAndRestartAreSilenceNotHardwareFailure();
    cachePreservesHistoryAndPendingDoesNotMutate();
    malformedInputAndMeter();
    concurrentHistoryReadersAndMeterObservers();
    std::puts("Monitor history, clocks, routing, demand, meters and recovery tests passed");
}
