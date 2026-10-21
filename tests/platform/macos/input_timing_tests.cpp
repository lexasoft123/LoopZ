#include <loopz/macos/input_timing.hpp>
#include <loopz/macos/scene_renderer.hpp>
#include <loopz/timeline.hpp>

#include <cassert>
#include <stdexcept>

namespace {
using namespace loopz;
RouteDocument document() {
    VirtualDeviceDefinition device;
    device.id = "mix";
    device.name = "Mix";
    device.routing.sources = {{"one", 2, 1, true, false, defaultChannelConnections(2, 2)},
                              {"two", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    device.bindings = {{"one", SourceKind::InputDevice, "same-device"},
                       {"two", SourceKind::InputDevice, "same-device"}};
    return {kRouteDocumentVersion, {device}};
}
} // namespace

int main() {
    using namespace loopz;
    using namespace loopz::macos;
    uint32_t queries = 0;
    std::optional<InputTimingFacts> facts{{48000, 32, 74}};
    PhysicalInputTiming policy({48000, 32, 1024, 128}, 64, [&](const auto& uid) {
        assert(uid == "same-device");
        ++queries;
        return facts;
    });
    auto desired = document();
    auto delays = policy.delays(desired);
    assert(queries == 1 && delays.size() == 2 && delays[0].frames == 352 &&
           delays[1].frames == 352);
    facts->bufferFrames = 1024;
    assert(policy.delays(desired)[0].frames == 1344);
    facts.reset();
    const auto committed = desired;
    const RouteDocument empty;
    assert(policy.delays(empty, &committed).empty());
    assert(policy.delays(desired, &committed)[0].frames == 1344);
    assert(policy.delays(empty, &empty).empty());
    assert(policy.delays(desired, &empty)[0].frames == 1024);
    facts = InputTimingFacts{48000, 64, 74};
    assert(policy.delays(desired)[0].frames == 384);
    facts->bufferFrames = 1024;
    assert(policy.delays(desired)[0].frames == 1344);
    desired.devices[0].routing.enabled = false;
    queries = 0;
    assert(policy.delays(desired).empty() && !queries);
    desired.devices[0].routing.enabled = true;
    facts.reset();
    // An uncommitted removal must not forget a large budget still used by the
    // active graph, even if its native metadata becomes temporarily unavailable.
    assert(policy.delays(desired)[0].frames == 1344);
    facts = InputTimingFacts{48000, 64, UINT32_MAX};
    bool rejected = false;
    try {
        policy.delays(desired);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);

    facts = InputTimingFacts{48000, 32, 74};
    SceneRenderer renderer({48000, 32, 1024, 128}, AudioScene::SourceResolver{});
    renderer.setInputTiming([&](const auto& route) {
        return policy.delays(route);
    });
    assert(renderer.inputLatencyFrames(desired) == std::vector<uint32_t>({353}));
    DeviceTransportPool pool;
    pool.apply(desired, renderer.inputLatencyFrames(desired));
    auto prepared = renderer.prepare(desired, pool.devices());
    prepared->commit();
    assert(renderer.observe()->deviceLatencyFrames(0) == 352);

    // A property change between metadata planning and preparation cannot publish
    // a graph against mappings prepared for the previous hardware budget.
    facts->bufferFrames = 512;
    rejected = false;
    try {
        renderer.prepare(desired, pool.devices());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected && renderer.observe()->deviceLatencyFrames(0) == 352);

    queries = 0;
    facts = InputTimingFacts{48000, 32, 74};
    PhysicalInputTiming causal(
        {48000, 32, 1024, 128}, 64,
        [&](const auto&) {
            ++queries;
            return facts;
        },
        ResamplingFilter::MinimumPhase);
    auto fast = causal.delays(desired);
    assert(queries == 1 && fast.size() == 2 && fast[0].frames == 256 && fast[1].frames == 256);
    facts->bufferFrames = 1024;
    assert(causal.delays(desired)[0].frames == 1248);
    facts.reset();
    assert(causal.delays(empty, &committed).empty());
    assert(causal.delays(desired, &committed)[0].frames == 1248);
    assert(causal.delays(empty, &empty).empty());
    assert(causal.delays(desired, &empty)[0].frames == 1024);
    facts = InputTimingFacts{48000, 64, 74};
    assert(causal.delays(desired)[0].frames == 288);
    auto mixedDocument = desired;
    mixedDocument.devices[0].bindings.push_back({"app", SourceKind::Application, "app.bundle"});
    mixedDocument.devices[0].routing.sources.push_back(
        {"app", 2, 1, true, false, defaultChannelConnections(2, 2)});
    auto solo = desired.devices[0];
    solo.id = solo.name = "solo";
    mixedDocument.devices.push_back(solo);
    queries = 0;
    const auto mixedDelays = causal.delays(mixedDocument);
    assert(queries == 1 && mixedDelays.size() == 4);
    assert(mixedDelays[0].frames == 384 && mixedDelays[1].frames == 384);
    assert(mixedDelays[0].filter == ResamplingFilter::LinearPhase);
    assert(mixedDelays[2].frames == 288 && mixedDelays[3].frames == 288);
    assert(mixedDelays[2].filter == ResamplingFilter::MinimumPhase);
    // Retain the largest selected budget for one shared hardware identity,
    // regardless of which independent logical route was visited last.
    facts->bufferFrames = 1024;
    const auto large = causal.delays(mixedDocument);
    assert(large[0].frames == 1344 && large[2].frames == 1248);
    facts.reset();
    assert(causal.delays(mixedDocument)[0].frames == 1344);
    assert(causal.delays(mixedDocument)[2].frames == 1344);
    PhysicalInputTiming bounded(
        {48000, 32, 1024, 128}, 32,
        [](const auto&) {
            return std::optional{InputTimingFacts{48000, 32, 74}};
        },
        ResamplingFilter::MinimumPhase);
    auto identities = document();
    constexpr auto maximumIdentities = 2 * kMaximumVirtualDevices * kMaximumRoutingSources;
    for (uint32_t identity = 0; identity < maximumIdentities - 1; ++identity) {
        for (auto& binding : identities.devices[0].bindings)
            binding.selector = "hardware-" + std::to_string(identity);
        assert(bounded.delays(identities).size() == 2);
    }
    identities.devices[0].bindings[0].selector = "last-allowed";
    identities.devices[0].bindings[1].selector = "over-capacity";
    bool overCapacity = false;
    try {
        bounded.delays(identities);
    } catch (const std::runtime_error&) {
        overCapacity = true;
    }
    assert(overCapacity);
    bool invalidFilter = false;
    try {
        PhysicalInputTiming invalid({48000, 32, 1024, 128}, 64, {},
                                    static_cast<ResamplingFilter>(99));
    } catch (const std::invalid_argument&) {
        invalidFilter = true;
    }
    assert(invalidFilter);
}
