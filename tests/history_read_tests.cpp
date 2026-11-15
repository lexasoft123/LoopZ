#include <loopz/runtime/control.hpp>
#include <loopz/render_schedule.hpp>
#include <loopz/sample_clock.hpp>
#include <loopz/timeline.hpp>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace {
using namespace loopz;
constexpr uint32_t rate = 48000, block = 256, lookback = 1024, queue = 1024, delay = 1792;
constexpr uint64_t origin = 1000000000, duration = 48 * 3600 * 1000000000ULL;
struct Bus final : AudioSink {
    std::shared_ptr<MonitorBus> bus = std::make_shared<MonitorBus>(2, rate);
    uint32_t channels() const noexcept override {
        return 2;
    }
    bool requested() const noexcept override {
        return true;
    }
    bool publish(uint64_t first, uint32_t hz, RoutingInput in, uint32_t frames) noexcept override {
        return bus->publish(first, hz, in, frames);
    }
    void idle() noexcept override {
        bus->idle();
    }
};
RouteDocument document() {
    VirtualDeviceDefinition device;
    device.id = "song";
    device.name = "Song";
    device.routing.safetyLimiter = false;
    SourceRouting source;
    source.id = "native";
    source.connections = {{0, 0, 1}, {1, 1, 1}};
    device.routing.sources.push_back(source);
    device.bindings.push_back({"native", SourceKind::Application, "native:song", false});
    MonitorDefinition monitor;
    monitor.id = "main";
    monitor.deviceSelector = "out";
    monitor.connections = {{0, 0, 1}, {1, 1, 1}};
    device.monitors.push_back(monitor);
    RouteDocument result;
    result.devices.push_back(device);
    return result;
}
struct Bands {
    HistoryReadSnapshot scene, monitor;
};
// Extract the bands from ACTUAL AudioScene and MonitorProgram reads. The
// long metadata simulation below does not invent its own bounds policy.
Bands realPrograms(uint32_t producerChunk, uint32_t sourceRate = rate) {
    const auto doc = document();
    auto source = std::make_shared<AudioHistory>(2);
    auto sink = std::make_shared<Bus>();
    AudioScene scene(
        doc, {rate, block, lookback, 128},
        [&](const auto&, const auto&, const auto&) {
            return ResolvedAudioSource{source, std::make_shared<AudioReadCursor>()};
        },
        [&](const auto&) {
            return sink;
        });
    scene.activate();
    const auto timing = monitorTimingFromBudget(rate, rate, delay, queue, block, block, block);
    MonitorProgram monitor(doc.devices[0], doc.devices[0].monitors[0], sink->bus->source(), timing);
    std::array<float, 1024> input;
    input.fill(.25f);
    const std::array<const float*, 2> in{input.data(), input.data()};
    std::array<float, block> left{}, right{};
    const std::array<float*, 2> out{left.data(), right.data()};
    uint64_t producer = 0;
    for (uint32_t index = 0; index < 24; ++index) {
        const auto first = hostFrame(origin, rate) + 4096 + uint64_t(index) * block;
        const auto nowFrame = hostFrame(frameNs(first - block, rate) - origin, sourceRate);
        while (producer <= nowFrame) {
            assert(source->append({in, 1024}, producerChunk, origin + frameNs(producer, sourceRate),
                                  sourceRate, double(producer), true));
            producer += producerChunk;
        }
        const auto result = scene.render(first, block);
        assert(result.validBlock && !result.failedGraphs && !result.failedSinks);
        const auto observation = scene.sourceStatus(0, 0).read;
        assert(observation.valid && observation.appendFrames == producerChunk &&
               observation.bandExits == 0);
        if (index >= 8) {
            const auto presented = frameNs(first - block + queue, rate);
            const auto read = monitor.render(presented, rate, {out, block}, block, first);
            assert(read.validBlock && !read.missingFrames && read.sourceOnline);
            for (const auto sample : left)
                assert(std::abs(sample - .25f) < .00001f);
            assert(monitor.status().read.valid && monitor.status().read.bandExits == 0);
        }
    }
    const Bands result{scene.sourceStatus(0, 0).read, monitor.status().read};
    assert(std::abs(result.scene.lowerBoundFrames - 512. * sourceRate / rate) < .01);
    assert(std::abs(result.scene.upperBoundFrames -
                    (double(lookback) * sourceRate / rate +
                     std::max(2. * block * sourceRate / rate, double(producerChunk)))) < .01);
    assert(result.monitor.lowerBoundFrames == 768 && result.monitor.upperBoundFrames == 1536);
    return result;
}
// Opt-in exhaustive run: every publication/read mapping for 48h. Debug and
// sanitizer builds should use the default coarse checkpoints below instead.
void exhaustiveClocks(uint32_t chunk, double ppm, bool bias, const Bands& bands, bool wide) {
    SampleClockHistory source(512), bus(512);
    HistoryReadObservation sourceRead, monitorRead;
    sourceRead.bounds(wide ? 0 : bands.scene.lowerBoundFrames,
                      wide ? 32768 : bands.scene.upperBoundFrames, chunk);
    monitorRead.bounds(wide ? 0 : bands.monitor.lowerBoundFrames,
                       wide ? 32768 : bands.monitor.upperBoundFrames, block);
    RenderSchedule scene({rate, block, block, 4096});
    uint64_t sourceEnd = 0, busEnd = 0, wake = origin, sceneCalls = 0;
    const uint64_t sourceOrigin = origin + 5000;
    const auto physicalRate = rate * (1 + static_cast<long double>(ppm) / 1e6L);
    const auto periods = static_cast<uint64_t>(duration * physicalRate / (block * 1e9L));
    auto publish = [&](uint64_t until) {
        while (sourceOrigin + frameNs(sourceEnd, rate) <= until) {
            assert(
                source.append(sourceEnd, sourceOrigin + frameNs(sourceEnd, rate), sourceEnd != 0));
            sourceEnd += chunk;
        }
    };
    for (uint64_t period = 0; period <= periods; ++period) {
        const auto now = origin + static_cast<uint64_t>(period * block * 1e9L / physicalRate);
        while (wake <= now) {
            publish(wake);
            const auto scheduled = scene.step(wake);
            assert(scheduled.valid && !scheduled.reanchored);
            for (uint32_t index = 0; index < scheduled.blocks; ++index) {
                const auto first = scheduled.firstFrame + uint64_t(index) * block;
                ++sceneCalls;
                if (sceneCalls > 1024) {
                    const auto requested =
                        frameNs(first - lookback, rate) + (bias ? sceneCalls / 3 : 0);
                    const auto point = source.locate(source.snapshot(), requested, 0, rate, rate);
                    assert(point.valid);
                    sourceRead.observe(1, requested, sourceEnd, point.frame, point.offset, block,
                                       point.rate, rate, 32768, chunk);
                }
                assert(bus.append(first, frameNs(first, rate), busEnd != 0));
                busEnd = first + block;
            }
            assert(scheduled.nextWakeNs > wake);
            wake = scheduled.nextWakeNs;
        }
        publish(now);
        if (period <= 1024)
            continue;
        const int64_t jitter = (int64_t(period % 17) - 8) * 1000;
        const auto requested =
            now + frameNs(queue, rate) - frameNs(delay, rate) + jitter + (bias ? period / 3 : 0);
        const auto point = bus.locate(bus.snapshot(), requested, 0, rate, rate);
        assert(point.valid);
        monitorRead.observe(1, requested, busEnd, point.frame, point.offset, block, point.rate,
                            double(physicalRate), 32768, block);
    }
    const auto a = sourceRead.snapshot(), b = monitorRead.snapshot();
    std::printf(
        "48h production bands chunk=%u ppm=%+.0f bias=%d wide=%d faults scene=%llu monitor=%llu\n",
        chunk, ppm, bias, wide, static_cast<unsigned long long>(a.bandExits),
        static_cast<unsigned long long>(b.bandExits));
    assert(a.valid && b.valid);
    if (bias)
        assert(a.bandExits && b.bandExits);
    else
        assert(!a.bandExits && !b.bandExits);
}
// Retain actual clock-mapping/observer objects across the full 48h, inspecting
// a complete recent anchor window every 4096 device periods (~22s). Absolute
// source/device counters and injected bias advance to each checkpoint. This
// is clock coverage, not continuous PCM or every-period observer coverage.
void checkpointClocks(uint32_t chunk, double ppm, bool bias, const Bands& bands, bool wide) {
    SampleClockHistory source(512), bus(512);
    HistoryReadObservation sourceRead, monitorRead;
    sourceRead.bounds(wide ? 0 : bands.scene.lowerBoundFrames,
                      wide ? 32768 : bands.scene.upperBoundFrames, chunk);
    monitorRead.bounds(wide ? 0 : bands.monitor.lowerBoundFrames,
                       wide ? 32768 : bands.monitor.upperBoundFrames, block);
    const uint64_t sourceOrigin = origin + 5000;
    const auto physicalRate = rate * (1 + static_cast<long double>(ppm) / 1e6L);
    const auto periods = static_cast<uint64_t>(duration * physicalRate / (block * 1e9L));
    uint64_t sourceEnd = 0, busEnd = 0;
    auto appendWindow = [&](SampleClockHistory& clock, uint64_t last, uint32_t quantum,
                            uint64_t clockOrigin, uint64_t& end) {
        const auto first = last - 15 * quantum;
        for (auto frame = std::max(first, end); frame <= last; frame += quantum) {
            assert(clock.append(frame, clockOrigin + frameNs(frame, rate), end && frame == end));
            end = frame + quantum;
        }
    };
    for (uint64_t period = 4096;; period = std::min(period + 4096, periods)) {
        const auto now = origin + static_cast<uint64_t>(period * block * 1e9L / physicalRate);
        // Acquire the same latest CPU render quantum that the regular schedule
        // would have published, without running all intervening DSP periods.
        RenderSchedule scene({rate, block, block, 4096});
        const auto scheduled = scene.step(now);
        assert(scheduled.valid && scheduled.blocks == 1);
        const auto first = scheduled.firstFrame;
        auto sceneWake = frameNs(first - block, rate);
        if (hostFrame(sceneWake, rate) < first - block)
            ++sceneWake;
        const auto lastSource = hostFrame(sceneWake - sourceOrigin, rate) / chunk * chunk;
        appendWindow(source, lastSource, chunk, sourceOrigin, sourceEnd);
        appendWindow(bus, first, block, 0, busEnd);
        const auto sceneCalls = hostFrame(sceneWake - origin, rate) / block + 1;
        const auto sourceRequested = frameNs(first - lookback, rate) + (bias ? sceneCalls / 3 : 0);
        const auto a = source.locate(source.snapshot(), sourceRequested, 0, rate, rate);
        assert(a.valid);
        sourceRead.observe(1, sourceRequested, sourceEnd, a.frame, a.offset, block, a.rate, rate,
                           32768, chunk);
        const int64_t jitter = (int64_t(period % 17) - 8) * 1000;
        const auto monitorRequested =
            now + frameNs(queue, rate) - frameNs(delay, rate) + jitter + (bias ? period / 3 : 0);
        const auto b = bus.locate(bus.snapshot(), monitorRequested, 0, rate, rate);
        assert(b.valid);
        monitorRead.observe(1, monitorRequested, busEnd, b.frame, b.offset, block, b.rate,
                            double(physicalRate), 32768, block);
        if (period == periods)
            break;
    }
    const auto a = sourceRead.snapshot(), b = monitorRead.snapshot();
    std::printf("48h coarse checkpoints chunk=%u ppm=%+.0f bias=%d wide=%d faults scene=%llu "
                "monitor=%llu\n",
                chunk, ppm, bias, wide, static_cast<unsigned long long>(a.bandExits),
                static_cast<unsigned long long>(b.bandExits));
    assert(a.valid && b.valid);
    if (bias)
        assert(a.bandExits && b.bandExits);
    else
        assert(!a.bandExits && !b.bandExits);
}
struct Host final : singz::AudioHostBackend {
    singz::AudioHostStatus current;
    singz::AudioHostInventory enumerate() const override {
        return {};
    }
    singz::AudioHostResult open(const singz::AudioHostConfig&, singz::AudioHostRender,
                                void*) override {
        current.format = {48000, 8192, 256, 0, 2};
        current.latency.bufferFrames = current.diagnostics.outputBufferFrames = queue;
        current.diagnostics.outputPeriodFrames = block;
        current.state = singz::AudioHostState::Open;
        return {true, singz::AudioHostError::None, current.state, current.format, current.latency,
                {}};
    }
    singz::AudioHostResult start() override {
        current.state = singz::AudioHostState::Running;
        return {true, singz::AudioHostError::None, current.state, current.format, current.latency,
                {}};
    }
    void stop() noexcept override {
        current.state = singz::AudioHostState::Stopped;
    }
    singz::AudioHostStatus status() const noexcept override {
        return current;
    }
};
void controlKeys() {
    char directory[] = "/tmp/loopz-history-XXXXXX";
    assert(mkdtemp(directory));
    const auto path = std::filesystem::path(directory) / "routing.json";
    const auto doc = document();
    std::ofstream(path) << JSON{
        {"version", 1}, {"generation", "1"}, {"document", encodeDocument(doc)}};
    auto audio = std::make_unique<runtime::AudioRuntime>([] {
        return std::make_unique<Host>();
    });
    audio->bindNativeSource("native:song", std::make_shared<AudioHistory>(2));
    {
        runtime::Control control(path, std::move(audio));
        const auto state =
            control.request({{"version", 1}, {"operation", "state"}}).at("snapshot").at("runtime");
        const auto& monitor = state.at("monitors").at(0);
        assert(monitor.at("clockDomain") == "history");
        const auto& source = state.at("devices").at(0).at("sources").at(0);
        for (const auto* row : {&monitor, &source}) {
            const auto& read = row->at("historyRead");
            for (const auto* key : {"observations", "bandExits", "reanchors", "generationChanges",
                                    "windowObservations"})
                assert(read.at(key).is_string());
            for (const auto* key : {"headroomFrames", "minimumFrames", "maximumFrames",
                                    "meanFrames", "acquisitionFrames", "lowerBoundFrames",
                                    "upperBoundFrames", "ratio", "appendFrames"})
                assert(read.at(key).is_number());
            assert(read.at("valid").is_boolean() && read.at("outsideBand").is_boolean());
        }
    }
    std::filesystem::remove_all(directory);
}
} // namespace
int main(int argc, char** argv) {
    const bool wide = argc == 2 && std::strcmp(argv[1], "--wide-bounds-negative-control") == 0;
    const bool exhaustive = argc == 2 && std::strcmp(argv[1], "--exhaustive") == 0;
    assert(argc == 1 || wide || exhaustive);
    controlKeys();
    (void)realPrograms(1024, 96000); // Bounds and append slack use source, not render, frames.
    for (uint32_t chunk : {256u, 480u, 512u, 1024u}) {
        const auto bands = realPrograms(chunk);
        const double ppm = chunk == 480 || chunk == 1024 ? -50 : 50;
        const auto clocks = exhaustive ? exhaustiveClocks : checkpointClocks;
        clocks(chunk, ppm, false, bands, wide);
        clocks(chunk, ppm, true, bands, wide);
        if (wide)
            break; // The biased case above MUST reject this silent-fault control.
    }
    std::puts("Real scene/monitor bands, 48h faults, larger append chunks and Control JSON passed");
}
