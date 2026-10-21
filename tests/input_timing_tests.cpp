#include <loopz/input_timing.hpp>
#include <loopz/timeline.hpp>

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
using namespace loopz;
VirtualDeviceDefinition device(std::string id, SourceKind kind, std::string selector) {
    VirtualDeviceDefinition result;
    result.id = id;
    result.name = id;
    result.routing.outputChannels = 2;
    result.routing.sources = {{"source", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    result.bindings = {{"source", kind, selector}};
    return result;
}
template <class Action> void rejects(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main() {
    using namespace loopz;
    const SceneTiming render{48000, 32, 1024, 128};
    assert(inputTimingBudget({48000, 32, 74}, render, 64) == 352U);
    assert(inputTimingBudget({48000, 64, 74}, render, 64) == 384U);
    assert(inputTimingBudget({48000, 512, 74}, render, 64) == 832U);
    assert(inputTimingBudget({48000, 32, 74}, {48000, 64, 1024, 128}, 64) == 448U);
    assert(inputTimingBudget({48000, 32, 74}, render, 64, ResamplingFilter::MinimumPhase) == 256U);
    assert(inputTimingBudget({48000, 64, 74}, render, 64, ResamplingFilter::MinimumPhase) == 288U);
    assert(inputTimingBudget({48000, 512, 74}, render, 64, ResamplingFilter::MinimumPhase) == 736U);
    for (double rate : {8000., 44100., 48000., 96000., 192000.}) {
        uint32_t previous = 0;
        for (uint32_t period : {16, 32, 64, 128, 256, 512, 1024}) {
            const auto result = inputTimingBudget({rate, period, 74}, render, 64);
            const auto causal =
                inputTimingBudget({rate, period, 74}, render, 64, ResamplingFilter::MinimumPhase);
            assert(result && *result >= previous && *result % render.blockFrames == 0);
            assert(*result * rate / render.renderRate >= period + 74);
            assert(causal && *causal <= *result && *causal % render.blockFrames == 0);
            assert(*causal >=
                   (period + 74) * double(render.renderRate) / rate + 64 + 2 * render.blockFrames);
            previous = *result;
        }
    }
    assert(!inputTimingBudget({NAN, 32, 74}, render, 64));
    assert(!inputTimingBudget({48000, 0, 74}, render, 64));
    assert(!inputTimingBudget({48000, 32, UINT32_MAX}, render, 64));
    assert(!inputTimingBudget({8000, kCapacity / 2, 74}, render, 64));
    assert(!inputTimingBudget({48000, 32, 74}, render, UINT32_MAX));
    assert(inputTimingBudget({192000, 128, 30000}, render, 32) == 7744U);
    assert(!inputTimingBudget({192000, 128, 32000}, render, 32));
    assert(!inputTimingBudget({192000, 128, 32000}, render, 32, ResamplingFilter::MinimumPhase));
    assert(!inputTimingBudget({48000, 32, 74}, render, 64, static_cast<ResamplingFilter>(99)));
    assert(
        !inputTimingBudget({8000, kCapacity / 2, 74}, render, 64, ResamplingFilter::MinimumPhase));

    auto input = device("input", SourceKind::InputDevice, "hardware");
    auto relay = device("relay", SourceKind::VirtualDevice, "input");
    auto mixed = device("mixed", SourceKind::VirtualDevice, "relay");
    mixed.bindings.push_back({"app", SourceKind::Application, "test.application"});
    mixed.routing.sources.push_back({"app", 2, 1, true, false, defaultChannelConnections(2, 2)});
    RouteDocument document{kRouteDocumentVersion, {mixed, relay, input}};
    std::vector<DeviceInputDelay> delays{{"input", "source", 352}};
    auto plan = planSceneTiming(document, render, {}, delays);
    assert(plan.latencyFrames == std::vector<uint32_t>({1024, 352, 352}));
    AudioScene scene(document, render, {}, {}, {}, {}, delays);
    assert(scene.deviceLatencyFrames(0) == 1024 && scene.deviceLatencyFrames(1) == 352);
    document.devices[0].routing.sources[1].enabled = false;
    assert(planSceneTiming(document, render, {}, delays).latencyFrames ==
           std::vector<uint32_t>({352, 352, 352}));
    delays[0].frames = 2048;
    assert(planSceneTiming(document, render, {}, delays).latencyFrames ==
           std::vector<uint32_t>({2048, 2048, 2048}));
    const std::vector<DeviceInputDelay> emptyIdentity{{"", "", 0}};
    rejects([&] {
        planSceneTiming(document, render, {}, emptyIdentity);
    });
    delays.push_back(delays[0]);
    rejects([&] {
        planSceneTiming(document, render, {}, delays);
    });
    delays.pop_back();
    delays[0].device = "absent";
    rejects([&] {
        planSceneTiming(document, render, {}, delays);
    });
    document.devices[0].routing.sources[1].enabled = true;
    delays = {{"input", "source", 128}, {"mixed", "app", 256}};
    assert(planSceneTiming(document, render, {}, delays).latencyFrames ==
           std::vector<uint32_t>({256, 128, 128}));
    delays[1].filter = ResamplingFilter::MinimumPhase;
    rejects([&] {
        planSceneTiming(document, render, {}, delays);
    });
    delays = {{"relay", "source", 32}};
    rejects([&] {
        planSceneTiming(document, render, {}, delays);
    });
    delays = {{"input", "source", kCapacity}};
    rejects([&] {
        planSceneTiming(document, render, {}, delays);
    });
}
