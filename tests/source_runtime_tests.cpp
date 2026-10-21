#include <loopz/audio_history.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/source_registry.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <map>
#include <thread>

namespace {
thread_local bool watchHeap = false;
thread_local unsigned heapEvents = 0;
} // namespace
void* operator new(size_t count) {
    if (watchHeap)
        ++heapEvents;
    if (auto* value = std::malloc(count ? count : 1))
        return value;
    throw std::bad_alloc();
}
void* operator new[](size_t count) {
    return ::operator new(count);
}
void operator delete(void* value) noexcept {
    if (watchHeap && value)
        ++heapEvents;
    std::free(value);
}
void operator delete[](void* value) noexcept {
    ::operator delete(value);
}
void operator delete(void* value, size_t) noexcept {
    ::operator delete(value);
}
void operator delete[](void* value, size_t) noexcept {
    ::operator delete(value);
}
namespace {
using namespace loopz;
void close(float value, float expected) {
    assert(std::isfinite(value) && std::fabs(value - expected) < 0.00002f);
}
template <typename F> void noHeap(F operation) {
    heapEvents = 0;
    watchHeap = true;
    operation();
    watchHeap = false;
    assert(!heapEvents);
}
struct Planes {
    std::vector<float> storage;
    std::array<float*, 64> write{};
    std::array<const float*, 64> read{};
    uint32_t channels, frames;
    Planes(uint32_t count, uint32_t capacity)
        : storage(count * capacity), channels(count), frames(capacity) {
        for (unsigned i = 0; i < count; ++i) {
            write[i] = storage.data() + i * frames;
            read[i] = write[i];
        }
    }
    RoutingInput input(uint32_t count = 0) const {
        return {{read.data(), count ? count : channels}, frames};
    }
    RoutingOutput output(uint32_t count = 0) {
        return {{write.data(), count ? count : channels}, frames};
    }
    void fill(float base) {
        for (unsigned ch = 0; ch < channels; ++ch)
            std::fill_n(write[ch], frames, base * (ch + 1));
    }
};
void historyChannelsAndRecovery() {
    for (const auto count : {1U, 2U, 8U, 64U}) {
        AudioHistory history(count, 1024);
        Planes input(count, 512), output(count, 256);
        AudioReadCursor a, b;
        input.fill(0.01f);
        assert(!history.read(1000000000, 48000, output.output(), 64, a).online);
        noHeap([&] {
            assert(history.append(input.input(), 512, 1000000000, 48000, 0, true));
        });
        for (auto* reader : {&a, &b}) {
            noHeap([&] {
                const auto result = history.read(1000000000, 48000, output.output(), 256, *reader);
                assert(result.online && !result.missingFrames);
            });
            for (unsigned ch = 0; ch < count; ++ch)
                for (unsigned i = 0; i < 256; ++i)
                    close(output.write[ch][i], (ch + 1) * 0.01f);
        }
        const auto generation = a.generation;
        noHeap([&] {
            history.stop();
        });
        assert(!history.status().online);
        assert(!history.read(1000000000, 48000, output.output(), 64, a).online);
        noHeap([&] {
            assert(history.append(input.input(), 512, 1000000000, 48000, 0, true));
        });
        assert(!history.read(1000000000, 48000, output.output(), 64, a).missingFrames);
        assert(a.generation != generation);
        assert(!history.read(1000000000, 48000, output.output(), 64, b).missingFrames);
        if (count > 1) {
            // The hardware narrows without changing saved logical channel count.
            assert(history.append(input.input(1), 512, 2000000000, 44100, 0, true));
            const auto result = history.read(2000000000, 44100, output.output(), 64, a);
            assert(result.online && result.missingFrames == 64 && history.channels() == count);
            for (unsigned i = 0; i < 64; ++i) {
                close(output.write[0][i], 0.01f);
                close(output.write[count - 1][i], 0);
            }
            assert(!history.read(2000000000, 44100, output.output(1), 64, b).missingFrames);
            assert(history.append(input.input(), 512, 3000000000, 48000, 0, true));
            assert(!history.read(3000000000, 48000, output.output(), 64, a).missingFrames);
            close(output.write[count - 1][0], count * 0.01f);
            assert(history.status().discontinuities == 2);
        }
        input.write[0][0] = NAN;
        input.write[count - 1][1] = INFINITY;
        assert(history.append(input.input(), 512, 4000000000, 48000, 0, true));
        assert(!history.read(4000000000, 48000, output.output(), 64, a).missingFrames);
        close(output.write[0][0], 0);
        close(output.write[count - 1][1], 0);
        auto invalid = output.output();
        invalid.capacityFrames = 2;
        std::fill_n(output.write[0], 256, 9);
        assert(!history.read(4000000000, 48000, invalid, 64, a).online);
        close(output.write[0][0], 0);
        close(output.write[0][1], 0);
        close(output.write[0][2], 9);
    }
}
void historyClocksAndOverwrites() {
    AudioHistory history(2, 1024);
    Planes in(2, 256), out(2, 256);
    AudioReadCursor cursor;
    for (unsigned block = 0; block < 12; ++block) {
        for (unsigned ch = 0; ch < 2; ++ch)
            for (unsigned i = 0; i < 256; ++i)
                in.write[ch][i] = (block * 256 + i) * 0.001f + ch;
        assert(history.append(in.input(), 256, 1000000000 + frameNs(block * 256, 48000), 48000,
                              block * 256, true));
    }
    const auto missing = history.read(1000000000, 48000, out.output(), 64, cursor);
    assert(missing.online && missing.missingFrames == 64);
    for (unsigned ch = 0; ch < 2; ++ch)
        for (unsigned i = 0; i < 64; ++i)
            close(out.write[ch][i], 0);
    const auto begin = 1000000000 + frameNs(10 * 256, 48000);
    assert(!history.read(begin, 44100, out.output(), 64, cursor).missingFrames);
    for (unsigned i = 0; i < 64; ++i)
        close(out.write[1][i], 2.56f + 1 + i * 48000.0f / 44100 * 0.001f);
    const auto before = cursor.generation;
    in.fill(0.1f);
    assert(
        history.append(in.input(), 256, 1000000000 + frameNs(12 * 256, 48000), 48000, 99999, true));
    assert(history.read(begin, 44100, out.output(), 64, cursor).missingFrames == 64);
    assert(cursor.generation != before);
    assert(history.read(UINT64_MAX, 48000, out.output(), 64, cursor).missingFrames == 64);
    assert(!history.append(in.input(), 0, 100, 48000));
    assert(!history.append(in.input(), 64, UINT64_MAX, 48000));
    assert(!history.append(in.input(), 64, 100, NAN));
}
void measuredInputClock() {
    AudioHistory history(2, 4096);
    Planes input(2, 256), output(2, 64);
    constexpr double actualRate = 48000.96;
    input.fill(0.1f);
    uint64_t time = 0;
    for (unsigned block = 0; block < 512; ++block) {
        time = 1000000000 + static_cast<uint64_t>(block * 256 * 1e9L / actualRate);
        assert(history.append(input.input(), 256, time, 48000,
                              static_cast<double>(block) * 256 - 256, true));
    }
    const auto status = history.status();
    assert(status.discontinuities == 0 && std::fabs(status.measuredRate - actualRate) < 0.02);
    AudioReadCursor cursor;
    assert(!history.read(time, 44100, output.output(), 64, cursor).missingFrames);
    close(output.write[1][63], 0.2f);
}
void concurrentHistory() {
    AudioHistory history(8, 1024);
    Planes in(8, 128), out(8, 64);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> latest{0};
    std::atomic<unsigned> observations{0};
    std::thread producer([&] {
        for (unsigned block = 0; block < 1000; ++block) {
            const auto time = 1000000000 + frameNs(block * 128, 48000);
            in.fill(block % 2 ? 0.01f : 0.02f);
            noHeap([&] {
                assert(history.append(in.input(), 128, time, 48000, block * 128, true));
            });
            latest.store(time);
        }
        done = true;
    });
    AudioReadCursor cursor;
    while (!done.load() || !observations.load()) {
        const auto time = latest.load();
        if (!time)
            continue;
        noHeap([&] {
            history.read(time, 48000, out.output(), 64, cursor);
        });
        for (unsigned i = 0; i < 64; ++i) {
            const auto base = out.write[0][i];
            // Bandlimited interpolation can ring around the producer's steps;
            // it must remain bounded and preserve every channel relationship.
            assert(base >= -0.005f && base <= 0.025f);
            for (unsigned ch = 0; ch < 8; ++ch)
                close(out.write[ch][i], base * (ch + 1));
        }
        ++observations;
    }
    producer.join();
    assert(observations.load());
}
class Endpoint final : public SourceEndpoint {
  public:
    const uint32_t count;
    std::atomic<bool> ready{false};
    bool live{true}, allChannels{true}, throwOpen{false}, throwHealth{false}, retainPolicies{false};
    bool retainMutePolicies{false};
    unsigned failures{0}, opens{0}, closes{0}, updates{0};
    uint32_t available{UINT32_MAX};
    uint64_t* completionTime{nullptr};
    uint64_t openDelay{0};
    SourceOpenPolicy policy{};
    std::vector<InputTimingFacts> timing;
    mutable uint32_t timingQueries{0};
    std::shared_ptr<const SourceDiagnostics> diagnostic;
    std::shared_ptr<const SourceDiagnostics> diagnostics() const override {
        return diagnostic;
    }
    std::vector<InputTimingFacts> inputTiming() const override {
        ++timingQueries;
        return timing;
    }
    void clearInputTiming() noexcept override {
        timing.clear();
    }
    std::vector<InputTimingFacts> retainedInputTiming() const override {
        return timing;
    }
    explicit Endpoint(uint32_t channels) : count(channels) {}
    uint32_t channels() const noexcept override {
        return count;
    }
    bool open(SourceOpenPolicy requested, std::string& error) override {
        ++opens;
        policy = requested;
        if (completionTime)
            *completionTime += openDelay;
        if (throwOpen)
            throw std::runtime_error("provider open exception");
        if (failures) {
            --failures;
            error = "device missing";
            return false;
        }
        ready = true;
        return true;
    }
    void close() noexcept override {
        ++closes;
        ready = false;
    }
    bool healthy(std::string& error) override {
        if (throwHealth)
            throw std::runtime_error("provider health exception");
        if (!live)
            error = "device disconnected";
        return live;
    }
    bool complete() const noexcept override {
        return allChannels && available >= policy.requiredChannels;
    }
    bool updatePolicy(SourceOpenPolicy requested) noexcept override {
        if (!retainPolicies || !ready.load() ||
            (requested.muteOriginal != policy.muteOriginal && !retainMutePolicies) ||
            !requested.requiredChannels || requested.requiredChannels > count)
            return false;
        policy = requested;
        ++updates;
        return true;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override {
        ++cursor.generation;
        for (unsigned ch = 0; ch < output.channels.size(); ++ch)
            std::fill_n(output.channels[ch], frames, 0.1f * (ch + 1));
        return {0, ready.load()};
    }
};
class Sink final : public AudioSink {
  public:
    bool demand{true};
    std::array<float, 64> last{};
    uint32_t count;
    explicit Sink(unsigned channels = 2) : count(channels) {}
    uint32_t channels() const noexcept override {
        return count;
    }
    bool requested() const noexcept override {
        return demand;
    }
    bool publish(uint64_t, uint32_t, RoutingInput input, uint32_t) noexcept override {
        for (unsigned ch = 0; ch < count; ++ch)
            last[ch] = input.channels[ch][0];
        return true;
    }
    void idle() noexcept override {}
};
VirtualDeviceDefinition device(std::string id, SourceKind kind, const std::string& selector,
                               unsigned channels = 2) {
    VirtualDeviceDefinition result;
    result.id = id;
    result.name = id;
    result.routing.outputChannels = channels;
    result.routing.sources = {
        {"source", channels, 1, true, false, defaultChannelConnections(channels, channels)}};
    result.bindings = {{"source", kind, selector, false}};
    return result;
}
void sharedSourcesAndPolicy() {
    std::vector<std::shared_ptr<Endpoint>> endpoints;
    SourceRegistry registry([&](auto, const auto&, unsigned channels) {
        auto endpoint = std::make_shared<Endpoint>(channels);
        endpoints.push_back(endpoint);
        return endpoint;
    });
    RouteDocument document{1,
                           {device("one", SourceKind::Application, "app"),
                            device("two", SourceKind::Application, "app")}};
    auto first = std::make_shared<Sink>(), second = std::make_shared<Sink>();
    const auto prepare = [&] {
        registry.prepare(document);
        return std::make_unique<AudioScene>(
            document, SceneTiming{},
            [&](const auto& d, const auto& b, const auto& r) {
                return registry.resolve(d, b, r);
            },
            [&](const auto& d) {
                return d.id == "one" ? first : second;
            });
    };
    auto scene = prepare();
    assert(endpoints.size() == 1 && !endpoints[0]->opens);
    endpoints[0]->retainPolicies = true;
    assert(scene->input(0, 0)->source == scene->input(1, 0)->source);
    assert(scene->input(0, 0)->cursor != scene->input(1, 0)->cursor);
    noHeap([&] {
        scene->render(100000, 64);
    });
    registry.poll(scene.get(), 0);
    assert(registry.restored() && endpoints[0]->opens == 1 && !endpoints[0]->policy.muteOriginal);
    assert(!registry.status()[0].diagnostics);
    auto diagnostic = std::make_shared<SourceDiagnostics>();
    diagnostic->history.enabled = true;
    diagnostic->history.requestedFrames = 123456;
    endpoints[0]->diagnostic = diagnostic;
    assert(registry.status()[0].diagnostics->history.requestedFrames == 123456);
    endpoints[0]->diagnostic.reset();
    noHeap([&] {
        scene->render(100064, 64);
    });
    close(first->last[0], 0.1f);
    close(second->last[1], 0.2f);
    const auto cursor = scene->input(0, 0)->cursor;
    document.devices[1].bindings[0].muteOriginal = true;
    auto replacement = prepare();
    assert(replacement->input(0, 0)->cursor == cursor && endpoints.size() == 1 &&
           endpoints[0]->opens == 1);
    // A pending scene has no acquisition side effects. A just-committed
    // scene also preserves capture until its first coherent demand snapshot.
    registry.poll(replacement.get(), 99);
    assert(endpoints[0]->opens == 1 && endpoints[0]->ready.load());
    registry.poll(scene.get(), 100);
    assert(endpoints[0]->opens == 1);
    replacement->render(100128, 64);
    registry.poll(replacement.get(), 200);
    assert(endpoints[0]->opens == 2 && endpoints[0]->policy.muteOriginal);
    first->demand = false;
    replacement->render(100192, 64);
    registry.poll(replacement.get(), 300);
    assert(endpoints[0]->opens == 2); // Second route keeps the shared capture alive.
    second->demand = false;
    replacement->render(100256, 64);
    registry.poll(replacement.get(), 400);
    assert(!registry.status()[0].connected && !endpoints[0]->ready.load());
    second->demand = true;
    replacement->render(100320, 64);
    registry.poll(replacement.get(), 500);
    assert(endpoints[0]->opens == 3);
}
void previewDoesNotMuteOriginalPlayback() {
    std::shared_ptr<Endpoint> endpoint;
    SourceRegistry registry([&](auto, const auto&, unsigned channels) {
        endpoint = std::make_shared<Endpoint>(channels);
        endpoint->retainPolicies = endpoint->retainMutePolicies = true;
        return endpoint;
    });
    RouteDocument document{1,
                           {device("one", SourceKind::Application, "app"),
                            device("two", SourceKind::Application, "app"),
                            device("downstream", SourceKind::VirtualDevice, "one")}};
    for (size_t i = 0; i < 2; ++i)
        document.devices[i].bindings[0].muteOriginal = true;
    std::array sinks{std::make_shared<Sink>(), std::make_shared<Sink>(), std::make_shared<Sink>()};
    for (auto& sink : sinks)
        sink->demand = false;
    registry.prepare(document);
    AudioScene scene(
        document, {},
        [&](const auto& d, const auto& b, const auto& r) {
            return registry.resolve(d, b, r);
        },
        [&](const auto& d) {
            return sinks[d.id == "one" ? 0 : d.id == "two" ? 1 : 2];
        });
    // Metering a downstream virtual source also previews its upstream app.
    noHeap([&] {
        scene.render(100000, 64, 4);
    });
    registry.poll(&scene, 0, 0, 4);
    assert(endpoint->opens == 1 && endpoint->ready && !endpoint->policy.muteOriginal);
    assert(registry.status()[0].demanded && !registry.status()[0].muteOriginal);
    sinks[2]->demand = true;
    scene.render(100064, 64, 4);
    registry.poll(&scene, 1, 0, 4);
    assert(endpoint->policy.muteOriginal && endpoint->opens == 1 && endpoint->updates == 1);
    sinks[2]->demand = false;
    sinks[1]->demand = true;
    scene.render(100128, 64, 7);
    registry.poll(&scene, 2, 0, 7);
    assert(endpoint->policy.muteOriginal && endpoint->opens == 1 && endpoint->updates == 1);
    sinks[1]->demand = false;
    scene.render(100192, 64, 7);
    registry.poll(&scene, 3, 0, 7);
    assert(!endpoint->policy.muteOriginal && endpoint->opens == 1 && endpoint->updates == 2);
    // An actual client moving to a staged mapping retains its original policy.
    registry.poll(&scene, 4, 2, 7);
    assert(endpoint->policy.muteOriginal && endpoint->updates == 3 && endpoint->opens == 1);
    registry.poll(&scene, 5, 0, 7);
    assert(!endpoint->policy.muteOriginal && endpoint->updates == 4 && endpoint->opens == 1);
    noHeap([&] {
        scene.render(100256, 64);
    });
    registry.poll(&scene, 6);
    assert(!endpoint->ready && !registry.status()[0].demanded);
    document.devices[0].routing.enabled = false;
    document.devices[1].routing.enabled = false;
    AudioScene disabled(
        document, {},
        [&](const auto& d, const auto& b, const auto& r) {
            return registry.resolve(d, b, r);
        },
        [&](const auto& d) {
            return sinks[d.id == "one" ? 0 : d.id == "two" ? 1 : 2];
        });
    disabled.render(100320, 64, UINT32_MAX);
    registry.poll(&disabled, 7, 0, UINT32_MAX);
    assert(endpoint->opens == 1 && !endpoint->ready && !registry.status()[0].demanded);
}
void sharedChannelDemandRetainsHistory() {
    for (const auto available : {8U, 16U}) {
        std::shared_ptr<Endpoint> endpoint;
        SourceRegistry registry([&](auto, const auto&, unsigned channels) {
            endpoint = std::make_shared<Endpoint>(channels);
            endpoint->retainPolicies = true;
            endpoint->available = available;
            return endpoint;
        });
        RouteDocument document{1,
                               {device("narrow", SourceKind::InputDevice, "shared-input", 2),
                                device("wide", SourceKind::InputDevice, "shared-input", 16)}};
        auto narrow = std::make_shared<Sink>(), wide = std::make_shared<Sink>(16);
        wide->demand = false;
        registry.prepare(document);
        AudioScene scene(
            document, {},
            [&](const auto& d, const auto& b, const auto& r) {
                return registry.resolve(d, b, r);
            },
            [&](const auto& d) {
                return d.id == "narrow" ? narrow : wide;
            });
        scene.render(100000, 64);
        registry.poll(&scene, 0);
        assert(endpoint->channels() == 16 && endpoint->opens == 1 && !endpoint->closes &&
               endpoint->policy.requiredChannels == 2 && registry.restored());
        for (uint32_t change = 0; change < 64; ++change) {
            wide->demand = change % 2 == 0;
            noHeap([&] {
                assert(scene.render(100064 + change * 64, 64).validBlock);
            });
            const auto cursor = scene.input(0, 0)->cursor;
            const auto generation = cursor->generation;
            registry.poll(&scene, change + 1);
            assert(scene.input(0, 0)->cursor == cursor && cursor->generation == generation);
            const auto status = registry.status();
            assert(status.size() == 1 && status[0].connected && endpoint->opens == 1 &&
                   !endpoint->closes && endpoint->ready.load());
            const auto required = wide->demand ? 16U : 2U;
            assert(endpoint->policy.requiredChannels == required &&
                   status[0].requiredChannels == required &&
                   status[0].complete == (available >= required) &&
                   registry.restored() == (available >= required));
            close(narrow->last[0], 0.1f);
            close(narrow->last[1], 0.2f);
        }
        assert(endpoint->updates == 64);
        // Actual provider loss still closes and reopens; retaining a policy
        // must never override a failed health check or the last reader leaving.
        endpoint->live = false;
        registry.poll(&scene, 100);
        assert(endpoint->closes == 1 && !registry.status()[0].connected);
        endpoint->live = true;
        registry.retryNow(101);
        registry.poll(&scene, 101);
        assert(endpoint->opens == 2 && registry.restored());
        narrow->demand = false;
        scene.render(200000, 64);
        registry.poll(&scene, 102);
        assert(endpoint->closes == 2 && !registry.status()[0].connected);
    }
}
void missingRetryAndCapabilities() {
    std::shared_ptr<Endpoint> endpoint;
    SourceRegistry registry([&](auto, const auto&, unsigned channels) {
        endpoint = std::make_shared<Endpoint>(channels);
        return endpoint;
    });
    RouteDocument document{1,
                           {device("narrow", SourceKind::InputDevice, "usb", 2),
                            device("wide", SourceKind::InputDevice, "usb", 64)}};
    auto narrow = std::make_shared<Sink>(), wide = std::make_shared<Sink>(64);
    registry.prepare(document);
    AudioScene scene(
        document, {},
        [&](const auto& d, const auto& b, const auto& r) {
            return registry.resolve(d, b, r);
        },
        [&](const auto& d) {
            return d.id == "narrow" ? narrow : wide;
        });
    assert(endpoint->channels() == 64 && scene.input(0, 0)->source == scene.input(1, 0)->source);
    endpoint->failures = 2;
    scene.render(100000, 64);
    registry.poll(&scene, 0);
    assert(!registry.restored() && endpoint->opens == 1);
    registry.poll(&scene, 99);
    assert(endpoint->opens == 1);
    registry.poll(&scene, 100);
    assert(endpoint->opens == 2);
    registry.poll(&scene, 299);
    assert(endpoint->opens == 2);
    registry.poll(&scene, 300);
    assert(endpoint->opens == 3 && registry.restored());
    assert(endpoint->policy.requiredChannels == 64);
    endpoint->allChannels = false;
    registry.poll(&scene, 400);
    assert(!registry.restored() && registry.status()[0].connected &&
           !registry.status()[0].complete);
    endpoint->allChannels = true;
    assert(registry.restored());
    endpoint->live = false;
    registry.poll(&scene, 500);
    assert(!registry.restored() && !registry.status()[0].connected);
    scene.render(100064, 64);
    close(wide->last[0], 0);
    endpoint->live = true;
    registry.retryNow(501);
    registry.poll(&scene, 501);
    scene.render(100128, 64);
    close(wide->last[0], 0.1f);
    assert(registry.restored() && scene.document().devices.size() == 2);
    const auto old = endpoint->opens;
    wide->demand = false;
    scene.render(100192, 64);
    registry.poll(&scene, 600);
    assert(endpoint->opens == old + 1 && endpoint->policy.requiredChannels == 2);
    registry.stop();
    assert(!registry.status()[0].connected);
}
void acquisitionDeadlineAndExceptions() {
    uint64_t clock = 0;
    std::shared_ptr<Endpoint> endpoint;
    SourceRegistry registry(
        [&](auto, const auto&, unsigned channels) {
            endpoint = std::make_shared<Endpoint>(channels);
            endpoint->completionTime = &clock;
            return endpoint;
        },
        [&] {
            return clock;
        });
    RouteDocument doc{1, {device("slow", SourceKind::Application, "slow-app")}};
    registry.prepare(doc);
    auto sink = std::make_shared<Sink>();
    AudioScene scene(
        doc, {},
        [&](const auto& d, const auto& b, const auto& r) {
            return registry.resolve(d, b, r);
        },
        [&](const auto&) {
            return sink;
        });
    scene.render(100000, 64);
    endpoint->openDelay = 10001;
    registry.poll(&scene, 0);
    assert(!registry.status()[0].connected && !endpoint->ready.load());
    scene.render(100064, 64);
    close(sink->last[0], 0);
    endpoint->openDelay = 0;
    registry.poll(&scene, clock);
    registry.poll(&scene, clock += 100);
    assert(registry.restored());
    endpoint->live = false;
    registry.poll(&scene, clock += 100);
    endpoint->throwOpen = true;
    registry.retryNow(clock);
    registry.poll(&scene, clock);
    assert(!registry.restored());
    endpoint->throwOpen = false;
    endpoint->live = true;
    registry.retryNow(clock);
    registry.poll(&scene, clock);
    assert(registry.restored());
    registry.poll(nullptr, clock);
    assert(!registry.status()[0].connected);
}
void independentAcquisitionDeadlines() {
    // One control poll can acquire several sources. Each source receives its
    // own ten seconds, even after an earlier open consumed most or all of it.
    for (const bool firstExpires : {false, true}) {
        uint64_t clock = 0;
        std::map<std::string, std::shared_ptr<Endpoint>> endpoints;
        SourceRegistry registry(
            [&](auto, const std::string& selector, unsigned channels) {
                auto endpoint = std::make_shared<Endpoint>(channels);
                endpoint->completionTime = &clock;
                endpoint->openDelay = selector == "one"   ? (firstExpires ? 10001 : 9000)
                                      : selector == "two" ? 2000
                                                          : 5;
                endpoints[selector] = endpoint;
                return endpoint;
            },
            [&] {
                return clock;
            });
        RouteDocument doc{1,
                          {device("one", SourceKind::Application, "one"),
                           device("two", SourceKind::Application, "two"),
                           device("three", SourceKind::InputDevice, "three")}};
        std::map<std::string, std::shared_ptr<Sink>> sinks;
        registry.prepare(doc);
        AudioScene scene(
            doc, {},
            [&](const auto& d, const auto& b, const auto& r) {
                return registry.resolve(d, b, r);
            },
            [&](const auto& d) {
                auto sink = std::make_shared<Sink>();
                sinks[d.id] = sink;
                return sink;
            });
        scene.render(100000, 64);
        registry.poll(&scene, 0);
        const auto status = registry.status();
        assert(status.size() == 3 && status[0].connected == !firstExpires);
        assert(status[1].connected && status[2].connected);
        assert(registry.restored() == !firstExpires);
        for (const auto& source : status)
            assert(source.attempts == 1);
        noHeap([&] {
            scene.render(100064, 64);
        });
        close(sinks["one"]->last[0], firstExpires ? 0 : 0.1f);
        close(sinks["two"]->last[0], 0.1f);
        close(sinks["three"]->last[0], 0.1f);
        if (firstExpires) {
            endpoints["one"]->openDelay = 0;
            registry.poll(&scene, clock);
            registry.poll(&scene, clock += 100);
            assert(registry.restored() && endpoints["one"]->opens == 2);
            assert(endpoints["two"]->opens == 1 && endpoints["three"]->opens == 1);
        }
    }
}
void independentAppsAndInputs() {
    std::map<std::string, std::shared_ptr<Endpoint>> endpoints;
    SourceRegistry registry([&](auto, const std::string& selector, unsigned channels) {
        auto endpoint = std::make_shared<Endpoint>(channels);
        endpoints[selector] = endpoint;
        return endpoint;
    });
    auto mix = device("mix", SourceKind::Application, "app-0");
    mix.routing.sources.clear();
    mix.bindings.clear();
    for (unsigned i = 0; i < 6; ++i) {
        const auto id = "source-" + std::to_string(i);
        const auto selector = (i < 4 ? "app-" : "input-") + std::to_string(i);
        mix.routing.sources.push_back({id, 2, 0.1f, true, false, defaultChannelConnections(2, 2)});
        mix.bindings.push_back(
            {id, i < 4 ? SourceKind::Application : SourceKind::InputDevice, selector, i == 2});
    }
    auto downstream = device("record", SourceKind::VirtualDevice, "mix");
    RouteDocument document{1, {mix, downstream}};
    registry.prepare(document);
    auto upstreamSink = std::make_shared<Sink>(), output = std::make_shared<Sink>();
    upstreamSink->demand = false;
    AudioScene scene(
        document, {},
        [&](const auto& d, const auto& b, const auto& r) {
            return registry.resolve(d, b, r);
        },
        [&](const auto& d) {
            return d.id == "mix" ? upstreamSink : output;
        });
    assert(endpoints.size() == 6);
    endpoints.at("app-0")->failures = 1;
    scene.render(100000, 64);
    assert(scene.demandedDevices() == 3);
    registry.poll(&scene, 0);
    assert(!registry.restored());
    for (const auto& [id, endpoint] : endpoints)
        assert(endpoint->opens == 1);
    scene.render(100064, 64);
    close(output->last[0], 0.05f);
    close(output->last[1], 0.1f);
    assert(endpoints.at("app-2")->policy.muteOriginal);
    assert(!endpoints.at("app-1")->policy.muteOriginal);
    registry.poll(&scene, 100);
    assert(registry.restored());
    scene.render(100128, 64);
    close(output->last[0], 0.06f);
    endpoints.at("app-1")->throwHealth = true;
    registry.poll(&scene, 200);
    assert(!registry.restored());
    scene.render(100192, 64);
    close(output->last[0], 0.05f);
    endpoints.at("app-1")->throwHealth = false;
    registry.retryNow(201);
    registry.poll(&scene, 201);
    assert(registry.restored());
    scene.render(100256, 64);
    close(output->last[0], 0.06f);
    assert(endpoints.at("input-4")->opens == 1 && endpoints.at("input-5")->opens == 1);
    output->demand = false;
    scene.render(100320, 64);
    registry.poll(&scene, 300);
    for (const auto& status : registry.status())
        assert(!status.demanded && !status.connected && !status.complete);
}
void rejectsAliasedBackendOwnership() {
    auto endpoint = std::make_shared<Endpoint>(2);
    SourceRegistry registry([&](auto, const auto&, unsigned) {
        return endpoint;
    });
    RouteDocument document{1,
                           {device("one", SourceKind::Application, "one"),
                            device("two", SourceKind::Application, "two")}};
    registry.prepare(document);
    bool rejected = false;
    try {
        AudioScene scene(document, {},
                         [&](const auto& d, const auto& b, const auto& r) {
                             return registry.resolve(d, b, r);
                         },
                         {});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected && !endpoint->opens);
}
class PausedEndpoint final : public SourceEndpoint {
  public:
    mutable std::atomic<bool> entered{false}, release{false}, pause{false};
    bool live{true};
    uint32_t channels() const noexcept override {
        return 2;
    }
    bool open(SourceOpenPolicy, std::string&) override {
        return true;
    }
    void close() noexcept override {}
    bool healthy(std::string&) override {
        return live;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        if (pause.load()) {
            entered = true;
            while (!release.load())
                std::this_thread::yield();
        }
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0.4f);
        return {0, true};
    }
};
void staleReadAcrossReconnect(bool filtered) {
    auto endpoint = std::make_shared<PausedEndpoint>();
    SourceRegistry registry([&](auto, const auto&, unsigned) {
        return endpoint;
    });
    RouteDocument document{1, {device("read-race", SourceKind::Application, "app")}};
    registry.prepare(document);
    auto sink = std::make_shared<Sink>();
    AudioScene scene(
        document, {},
        [&](const auto& d, const auto& b, const auto& r) {
            return registry.resolve(d, b, r);
        },
        [&](const auto&) {
            return sink;
        });
    scene.render(100000, 64);
    registry.poll(&scene, 0);
    endpoint->pause = true;
    const auto source = scene.input(0, 0)->source;
    Planes output(2, 64);
    AudioReadResult result{0, true};
    std::thread reader([&] {
        AudioReadCursor cursor;
        result = filtered ? source->readWithFilter(1000000000, 48000, output.output(), 64, cursor,
                                                   ResamplingFilter::LinearPhase)
                          : source->read(1000000000, 48000, output.output(), 64, cursor);
    });
    while (!endpoint->entered.load())
        std::this_thread::yield();
    endpoint->live = false;
    registry.poll(&scene, 100);
    endpoint->live = true;
    registry.retryNow(101);
    registry.poll(&scene, 101);
    assert(registry.restored());
    endpoint->release = true;
    reader.join();
    assert(!result.online && result.missingFrames == 64);
    for (const auto value : output.storage)
        close(value, 0);
    endpoint->pause = false;
    AudioReadCursor cursor;
    assert(source->read(1000000000, 48000, output.output(), 64, cursor).online);
    close(output.write[0][0], 0.4f);
}
void preparedFilterSelectionHasNoCallbackHeap() {
    AudioHistory history(2, 1024, ResamplingFilter::MinimumPhase);
    Planes input(2, 512), output(2, 32);
    input.fill(.1f);
    assert(history.append(input.input(), 512, 1000000000, 48000, 0, true));
    AudioReadCursor cursor;
    for (const auto filter : {ResamplingFilter::MinimumPhase, ResamplingFilter::LinearPhase,
                              ResamplingFilter::MinimumPhase}) {
        noHeap([&] {
            const auto result = history.readWithFilter(1000000000 + frameNs(300, 48000), 48000,
                                                       output.output(), 32, cursor, filter);
            assert(result.online && !result.missingFrames);
        });
        close(output.write[0][0], .1f);
    }
    noHeap([&] {
        const auto rejected = history.readWithFilter(1000000000, 48000, output.output(), 32, cursor,
                                                     static_cast<ResamplingFilter>(99));
        assert(!rejected.online && rejected.missingFrames == 32);
    });
    close(output.write[0][0], 0);
}
void timingFactsRequireAdmittedCompleteCapture() {
    auto endpoint = std::make_shared<Endpoint>(2);
    endpoint->timing = {{48000, 128, 2}};
    SourceRegistry registry([&](auto, const auto&, unsigned) {
        return endpoint;
    });
    RouteDocument document{1, {device("timing", SourceKind::Application, "tap")}};
    registry.prepare(document);
    auto sink = std::make_shared<Sink>();
    AudioScene scene(
        document, {},
        [&](const auto& d, const auto& b, const auto& r) {
            return registry.resolve(d, b, r);
        },
        [&](const auto&) {
            return sink;
        });
    assert(registry.inputTiming(SourceKind::Application, "tap").empty());
    assert(endpoint->timingQueries == 0);
    const auto* managed = scene.input(0, 0);
    Planes samples(2, 64);
    managed->source->read(100000, 48000, samples.output(), 64, *managed->cursor);
    assert(registry.status()[0].missingReadFrames == 64);
    scene.render(100000, 64);
    registry.poll(&scene, 0);
    assert(registry.inputTiming(SourceKind::Application, "tap").size() == 1);
    assert(endpoint->timingQueries == 1);
    assert(registry.status()[0].missingReadFrames == 128);
    endpoint->allChannels = false;
    assert(registry.inputTiming(SourceKind::Application, "tap").empty());
    endpoint->allChannels = true;
    endpoint->live = false;
    registry.poll(&scene, 100);
    assert(registry.inputTiming(SourceKind::Application, "tap").size() == 1);
    assert(endpoint->timingQueries == 1);
    endpoint->live = true;
    managed->source->readWithFilter(100000, 48000, samples.output(), 64, *managed->cursor,
                                    ResamplingFilter::LinearPhase);
    assert(registry.status()[0].missingReadFrames == 192);
    registry.retryNow(200);
    registry.poll(&scene, 200);
    assert(registry.inputTiming(SourceKind::Application, "tap").size() == 1);
    assert(registry.status()[0].missingReadFrames == 192);
    sink->demand = false;
    scene.render(100064, 64);
    registry.poll(&scene, 300);
    assert(!endpoint->ready);
    assert(registry.inputTiming(SourceKind::Application, "tap").size() == 1);
    registry.rejectInputTiming(SourceKind::Application, "tap");
    assert(endpoint->timing.empty());
    assert(registry.inputTiming(SourceKind::Application, "tap").empty());
    registry.stop();
    assert(registry.inputTiming(SourceKind::Application, "tap").empty());
}
} // namespace
int main() {
    historyChannelsAndRecovery();
    historyClocksAndOverwrites();
    concurrentHistory();
    measuredInputClock();
    sharedSourcesAndPolicy();
    previewDoesNotMuteOriginalPlayback();
    sharedChannelDemandRetainsHistory();
    missingRetryAndCapabilities();
    acquisitionDeadlineAndExceptions();
    independentAcquisitionDeadlines();
    staleReadAcrossReconnect(false);
    staleReadAcrossReconnect(true);
    preparedFilterSelectionHasNoCallbackHeap();
    timingFactsRequireAdmittedCompleteCapture();
    independentAppsAndInputs();
    rejectsAliasedBackendOwnership();
    std::puts("Shared capture sources: multichannel history, clocks, concurrent readers, "
              "deduplication, mute ownership, partial capability and recovery passed");
}
