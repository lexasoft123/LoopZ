#include "scene_domains.hpp"
#include <loopz/timeline.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
using namespace loopz;
using namespace loopz::windows;
namespace {
thread_local bool trackHeap{};
thread_local unsigned heapEvents{};
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
struct Source final : AudioSource {
    float value;
    mutable double rate{};
    explicit Source(float sample) : value(sample) {}
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t, double r, RoutingOutput out, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        rate = r;
        for (size_t channel = 0; channel < out.channels.size(); ++channel)
            std::fill_n(out.channels[channel], frames, value * float(channel + 1));
        return {0, true};
    }
};
struct Sink final : AudioSink {
    uint32_t rate{};
    uint64_t blocks{};
    std::array<std::array<float, kBlock>, 2> samples{};
    uint32_t channels() const noexcept override {
        return 2;
    }
    bool requested() const noexcept override {
        return true;
    }
    bool publish(uint64_t, uint32_t r, RoutingInput input, uint32_t frames) noexcept override {
        rate = r;
        ++blocks;
        for (size_t channel = 0; channel < 2; ++channel)
            std::copy_n(input.channels[channel], frames, samples[channel].begin());
        return true;
    }
    void idle() noexcept override {}
};
VirtualDeviceDefinition device(std::string id, double rate,
                               SourceKind kind = SourceKind::InputDevice,
                               std::string selector = "fixture") {
    VirtualDeviceDefinition d;
    d.id = id;
    d.name = id;
    d.sampleRate = rate;
    d.routing.outputChannels = 2;
    d.routing.safetyLimiter = false;
    d.routing.sources = {{"source", 2, 1, true, false, {{0, 0, 1}, {1, 1, 1}}}};
    d.bindings = {{"source", kind, std::move(selector)}};
    return d;
}
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception&) {
        failed = true;
    }
    assert(failed);
}
} // namespace
int main() {
    RouteDocument doc;
    doc.devices = {device("downstream", 96000, SourceKind::VirtualDevice, "upstream"),
                   device("independent", 48000), device("upstream", 44100),
                   device("native-rate", 44100)};
    auto plan = planSceneDomains(doc);
    assert(plan.size() == 3 && plan[0].rate == 44100 && plan[0].devices == std::vector<size_t>{3});
    assert(plan[1].rate == 48000 && plan[1].devices == std::vector<size_t>{1});
    assert(plan[2].rate == 96000 && plan[2].devices == std::vector<size_t>({0, 2}));
    auto disabled = doc;
    disabled.devices[0].routing.sources[0].enabled = false;
    disabled.devices[0].routing.enabled = false;
    assert(planSceneDomains(disabled)[2].devices == plan[2].devices);
    auto merged = doc;
    merged.devices[3].sampleRate = 48000;
    auto grouped = planSceneDomains(merged);
    assert(grouped.size() == 2 && grouped[0].devices == std::vector<size_t>({1, 3}));
    auto transitive = doc;
    transitive.devices[2].bindings[0] = {"source", SourceKind::VirtualDevice, "independent"};
    assert(planSceneDomains(transitive).size() == 2 &&
           planSceneDomains(transitive)[1].devices == std::vector<size_t>({0, 1, 2}));
    auto high = doc;
    high.devices[3].sampleRate = 192000;
    const auto bandwidth = planSceneDomains(high);
    assert(bandwidth.size() == 3 && bandwidth[1].rate == 96000 &&
           bandwidth[1].devices == std::vector<size_t>({0, 2}) && bandwidth[2].rate == 192000);
    assert(planSceneDomains(RouteDocument{}).empty());
    rejects([&] {
        auto bad = doc;
        bad.devices[0].sampleRate = std::numeric_limits<double>::quiet_NaN();
        planSceneDomains(bad);
    });
    rejects([&] {
        auto bad = doc;
        bad.devices[0].sampleRate = 44100.5;
        planSceneDomains(bad);
    });
    rejects([&] {
        auto bad = doc;
        bad.devices[0].id = bad.devices[1].id;
        planSceneDomains(bad);
    });
    auto a = std::make_shared<Source>(.125f), c = std::make_shared<Source>(.3f),
         d = std::make_shared<Source>(.2f);
    std::map<std::string, std::shared_ptr<Sink>> sinks;
    for (const auto& item : doc.devices)
        sinks.emplace(item.id, std::make_shared<Sink>());
    SceneDomainSchedules schedules;
    auto histories = std::make_shared<SceneHistoryStore>();
    const auto source = [&](const VirtualDeviceDefinition& item, const SourceBinding&,
                            const SourceRouting&) -> ResolvedAudioSource {
        auto value = item.id == "upstream" ? a : item.id == "independent" ? c : d;
        return {value, std::make_shared<AudioReadCursor>()};
    };
    const auto sink = [&](const VirtualDeviceDefinition& item) -> std::shared_ptr<AudioSink> {
        return sinks.at(item.id);
    };
    SceneDomains scenes(doc, plan, source, sink, histories, schedules, false);
    scenes.activate();
    assert(scenes.rate(0) == 96000 && scenes.rate(1) == 48000 && scenes.rate(2) == 96000 &&
           scenes.rate(3) == 44100);
    for (uint64_t now = 2000000000; now < 2250000000; now += 1000000) {
        trackHeap = true;
        for (const auto& domain : scenes.domains()) {
            const auto step = domain.schedule->step(now);
            assert(step.valid);
            for (uint32_t block = 0; block < step.blocks; ++block)
                assert(domain.scene->render(step.firstFrame + block * 256, 256).validBlock);
        }
        trackHeap = false;
        assert(heapEvents == 0);
    }
    assert(a->rate == 96000 && c->rate == 48000 && d->rate == 44100);
    for (size_t index = 0; index < doc.devices.size(); ++index) {
        auto value = sinks.at(doc.devices[index].id);
        const auto expected = index == 0 || index == 2 ? .125f : index == 1 ? .3f : .2f;
        assert(value->rate == scenes.rate(index) && value->blocks > 0);
        for (size_t channel = 0; channel < 2; ++channel)
            for (const auto sample : value->samples[channel])
                assert(std::abs(sample - expected * float(channel + 1)) < .00001f);
        assert(scenes.deviceStatus(index).active && scenes.sourceStatus(index, 0).online);
        AudioMeterSnapshot meter;
        assert(scenes.deviceMeters(index, meter) && scenes.sourceMeters(index, 0, meter));
        assert(scenes.input(index, 0));
    }
    const auto retained = schedules.at(96000);
    SceneDomains replacement(doc, plan, source, sink, histories, schedules, false);
    assert(replacement.domains()[2].schedule == retained);
    std::puts(
        "Independent rates, connected bandwidth, disabled/transitive dependencies, diagnostics "
        "mapping, retained clocks and allocation-free multi-domain waveforms passed");
}
