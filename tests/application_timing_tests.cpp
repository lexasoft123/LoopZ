#include <loopz/application_timing.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace {
using namespace loopz;
class TestSource final : public AudioSource {
  public:
    uint32_t channels() const noexcept override {
        return 2;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* channel : output.channels)
            std::fill_n(channel, frames, 0.0f);
        return {0, true};
    }
};
class TestSink final : public AudioSink {
  public:
    bool demand{false};
    uint32_t channels() const noexcept override {
        return 2;
    }
    bool requested() const noexcept override {
        return demand;
    }
    bool publish(uint64_t, uint32_t, RoutingInput, uint32_t) noexcept override {
        return true;
    }
    void idle() noexcept override {}
};
VirtualDeviceDefinition device(std::string id, SourceKind kind, std::string selector) {
    VirtualDeviceDefinition result;
    result.id = result.name = id;
    result.routing.sources = {{"source", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    result.bindings = {{"source", kind, selector}};
    return result;
}
template <class Action> void rejects(Action action) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    assert(failed);
}
void activeMixRetainsOnlyItsApplicationFloor() {
    auto app = device("app", SourceKind::Application, "bundle");
    auto relay = device("relay", SourceKind::VirtualDevice, "app");
    RouteDocument graph{kRouteDocumentVersion, {app, relay}};
    const auto source = std::make_shared<TestSource>();
    const auto appSink = std::make_shared<TestSink>(), relaySink = std::make_shared<TestSink>();
    const auto scene = [&](std::span<const DeviceInputDelay> budgets) {
        return std::make_unique<AudioScene>(
            graph, SceneTiming{48000, 32, 1024, 128},
            [&](const auto&, const auto&, const auto&) {
                return ResolvedAudioSource{source, std::make_shared<AudioReadCursor>()};
            },
            [&](const auto& definition) {
                return definition.id == "app" ? appSink : relaySink;
            },
            nullptr, std::span<const DevicePassThroughDelay>{}, budgets);
    };
    auto active = scene({});
    std::vector<DeviceInputDelay> lower{{"app", "source", 352}};
    appSink->demand = true;
    retainActiveApplicationTiming(graph, *active, lower);
    assert(lower[0].frames == 1024);
    appSink->demand = false;
    lower[0].frames = 352;
    retainActiveApplicationTiming(graph, *active, lower);
    assert(lower[0].frames == 352);
    relaySink->demand = true;
    retainActiveApplicationTiming(graph, *active, lower);
    assert(lower[0].frames == 1024); // Transitive public dependency pins its source.
    lower[0].frames = 1248;
    retainActiveApplicationTiming(graph, *active, lower);
    assert(lower[0].frames == 1248); // Required growth remains possible.
    graph.devices[0].bindings[0].selector = "new-bundle";
    lower[0].frames = 352;
    retainActiveApplicationTiming(graph, *active, lower);
    assert(lower[0].frames == 352);
    graph.devices[0].bindings[0].selector = "bundle";
    // A slower physical input sets common latency, not the app's own floor.
    graph.devices[0].bindings.push_back({"mic", SourceKind::InputDevice, "hardware"});
    graph.devices[0].routing.sources.push_back(
        {"mic", 2, 1, true, false, defaultChannelConnections(2, 2)});
    const std::vector<DeviceInputDelay> prepared{{"app", "source", 352}, {"app", "mic", 512}};
    active = scene(prepared);
    assert(active->deviceLatencyFrames(0) == 512);
    retainActiveApplicationTiming(graph, *active, lower);
    assert(lower[0].frames == 352);
}
} // namespace
int main() {
    using namespace loopz;
    activeMixRetainsOnlyItsApplicationFloor();
    const SceneTiming clock{48000, 32, 1024, 128};
    std::vector<InputTimingFacts> facts;
    unsigned queries = 0;
    ApplicationInputTiming timing(clock, 32, [&](const auto& selector) {
        assert(selector == "app.bundle");
        ++queries;
        return facts;
    });
    RouteDocument desired{kRouteDocumentVersion,
                          {device("one", SourceKind::Application, "app.bundle"),
                           device("two", SourceKind::Application, "app.bundle")}};
    auto delays = timing.delays(desired);
    assert(queries == 1 && delays.size() == 2 && delays[0].frames == 1024 && !delays[0].filter);
    facts = {{48000, 128, 2}};
    queries = 0;
    delays = timing.delays(desired);
    assert(queries == 1 && delays[0].frames == 352 && delays[1].frames == 352);
    facts.push_back({44100, 256, 32});
    assert(timing.delays(desired)[0].frames == 544);
    facts = {{48000, 1024, 0}};
    assert(timing.delays(desired)[0].frames == 1248);
    facts.clear();
    const RouteDocument empty;
    assert(timing.delays(empty, &desired).empty());
    assert(timing.delays(desired, &desired)[0].frames == 1248);
    assert(timing.delays(empty, &empty).empty());
    assert(timing.delays(desired, &empty)[0].frames == 1024);
    facts = {{48000, 128, 2}};
    auto app = desired.devices[0];
    auto mic = device("mic", SourceKind::InputDevice, "hardware");
    auto mix = device("mix", SourceKind::VirtualDevice, "mic");
    mix.bindings.push_back({"app", SourceKind::Application, "app.bundle"});
    mix.routing.sources.push_back({"app", 2, 1, true, false, defaultChannelConnections(2, 2)});
    RouteDocument graph{kRouteDocumentVersion, {mix, mic, app}};
    auto budgets = timing.delays(graph);
    budgets.push_back({"mic", "source", 384, ResamplingFilter::LinearPhase});
    assert(planSceneTiming(graph, clock, {}, budgets).latencyFrames ==
           std::vector<uint32_t>({384, 384, 352}));
    for (auto& definition : desired.devices)
        definition.routing.sources[0].enabled = false;
    queries = 0;
    assert(timing.delays(desired).empty() && queries == 0);
    desired.devices[0].routing.sources[0].enabled = true;
    facts = {{48000, 128, 2}, {std::numeric_limits<double>::quiet_NaN(), 128, 0}};
    rejects([&] {
        timing.delays(desired);
    });
    facts = std::vector<InputTimingFacts>(kMaximumRoutingSources + 1, {48000, 128, 0});
    rejects([&] {
        timing.delays(desired);
    });
    uint32_t invalidated = 0;
    ApplicationInputTiming recovering(
        clock, 32,
        [&](const auto&) {
            return facts;
        },
        [&](const auto& selector) {
            assert(selector == "app.bundle");
            ++invalidated;
        });
    assert(recovering.delays(desired)[0].frames == 1024 && invalidated == 1);
    facts = {{48000, 128, 2}};
    assert(recovering.delays(desired)[0].frames == 352);
    rejects([&] {
        ApplicationInputTiming invalid(clock, 32, {});
    });
    rejects([&] {
        ApplicationInputTiming invalid(clock, UINT32_MAX, [](const auto&) {
            return std::vector<InputTimingFacts>{};
        });
    });
}
