#include <loopz/macos/public_timing.hpp>
#include <loopz/macos/scene_renderer.hpp>
#include <loopz/timeline.hpp>

#include <cassert>
#include <map>
#include <cstring>
#include <stdexcept>

namespace {
using namespace loopz;
using namespace loopz::macos;
VirtualDeviceDefinition device(const std::string& id) {
    VirtualDeviceDefinition result;
    result.id = result.name = id;
    result.routing.sources = {{"pass", 2, 1, true, false, defaultChannelConnections(2, 2)}};
    result.bindings = {{"pass", SourceKind::PassThrough, ""}};
    return result;
}
template <typename F> void rejects(F operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
void nativeIdentityValidationKeepsTimingFresh() {
    std::map<std::string, uint32_t> objects;
    std::map<uint32_t, PublicDeviceTiming> timings;
    std::map<uint32_t, bool> reportsTiming;
    uint32_t resolutions = 0, validations = 0, reads = 0;
    bool validIdentity = true, readable = true;
    PublicTimingCatalog catalog({[&](const auto& uid) {
                                     ++resolutions;
                                     const auto found = objects.find(uid);
                                     return found == objects.end() ? 0 : found->second;
                                 },
                                 [&](uint32_t object) {
                                     ++validations;
                                     if (!validIdentity)
                                         throw std::runtime_error("Identity changed");
                                     return reportsTiming.at(object);
                                 },
                                 [&](uint32_t object, bool measured) {
                                     ++reads;
                                     if (!readable)
                                         throw std::runtime_error(
                                             "Device disappeared during timing read");
                                     assert(measured == reportsTiming.at(object));
                                     return timings.at(object);
                                 }});
    for (uint32_t object = 1; object <= 32; ++object) {
        objects[std::to_string(object)] = object;
        reportsTiming[object] = true;
        timings[object] = {64, true, 1, true};
    }
    for (unsigned pass = 0; pass < 3; ++pass)
        for (uint32_t object = 1; object <= 32; ++object) {
            timings[object] = {64U << pass, pass != 1, pass + 1, true};
            const auto result = catalog.probe(std::to_string(object));
            assert(result && result->bufferFrames == (64U << pass));
            assert(result->running == (pass != 1) && result->session == pass + 1);
        }
    assert(resolutions == 96 && reads == 96 && validations == 32);
    // The same UID resolving to a new object must validate again before reading.
    objects["1"] = 33;
    reportsTiming[33] = false;
    timings[33] = {512, false, 0, false};
    assert(!catalog.probe("1")->measured && validations == 33);
    objects.erase("1");
    assert(!catalog.probe("1") && validations == 33 && reads == 97);
    objects["1"] = 33;
    assert(catalog.probe("1") && validations == 34);
    readable = false;
    rejects([&] {
        (void)catalog.probe("1");
    });
    readable = true;
    assert(catalog.probe("1") && validations == 35);
    catalog.invalidate();
    validIdentity = false;
    rejects([&] {
        (void)catalog.probe("1");
    });
    rejects([&] {
        (void)catalog.probe("1");
    });
    assert(validations == 37); // Failed validation never enters the cache.
    validIdentity = true;
    assert(catalog.probe("1") && validations == 38);
    catalog.invalidate(); // Audio service reset, even if object IDs are reused.
    assert(catalog.probe("1") && validations == 39);
    // Independent discovery churn stays bounded and does not prevent revalidation.
    for (uint32_t object = 34; object < 320; ++object) {
        const auto uid = std::to_string(object);
        objects[uid] = object;
        reportsTiming[object] = true;
        timings[object] = {32, true, object, true};
        assert(catalog.probe(uid)->session == object);
    }
    const auto before = validations;
    assert(catalog.probe("1") && validations == before + 1);
    rejects([] {
        (void)PublicTimingCatalog({});
    });
}
void independentFloors() {
    SceneTiming timing;
    timing.blockFrames = 64;
    auto small = device("small"), large = device("large"), chain = device("chain");
    chain.bindings = {{"pass", SourceKind::VirtualDevice, "small"}};
    RouteDocument document{kRouteDocumentVersion, {small, large, chain}};
    std::map<std::string, PublicDeviceTiming> profiles{
        {"com.lexasoft.loopz.device.small", {64, false}},
        {"com.lexasoft.loopz.device.large", {512, false}}};
    uint32_t queries = 0;
    PublicPassThroughTiming policy(
        timing, [&](const auto& uid) -> std::optional<PublicDeviceTiming> {
            ++queries;
            assert(uid.starts_with("com.lexasoft.loopz.device."));
            const auto found = profiles.find(uid);
            return found == profiles.end() ? std::nullopt : std::optional(found->second);
        });
    auto plan = planSceneTiming(document, timing, policy.floors(document));
    assert((plan.latencyFrames == std::vector<uint32_t>{192, 1088, 192}) && queries == 2);
    profiles.begin()->second = {512, true}; // "large" remains unchanged.
    profiles["com.lexasoft.loopz.device.small"] = {256, true};
    plan = planSceneTiming(document, timing, policy.floors(document));
    assert((plan.latencyFrames == std::vector<uint32_t>{576, 1088, 576}));
    profiles["com.lexasoft.loopz.device.small"] = {32, true};
    plan = planSceneTiming(document, timing, policy.floors(document));
    assert(plan.latencyFrames[0] == 576); // A running endpoint does not shrink its queue.
    profiles["com.lexasoft.loopz.device.small"].running = false;
    plan = planSceneTiming(document, timing, policy.floors(document));
    assert((plan.latencyFrames == std::vector<uint32_t>{160, 1088, 160}));
    profiles.erase("com.lexasoft.loopz.device.small");
    plan = planSceneTiming(document, timing, policy.floors(document));
    assert(plan.latencyFrames[0] == 1088); // Capture-free pre-publication default.
    profiles["com.lexasoft.loopz.device.small"] = {0, false};
    rejects([&] {
        (void)policy.floors(document);
    });
    profiles["com.lexasoft.loopz.device.small"] = {32768, false};
    rejects([&] {
        (void)policy.floors(document);
    });
    document.devices[0].routing.enabled = false;
    const auto before = queries;
    (void)policy.floors(document);
    assert(queries == before + 2); // One active candidate and one old-cache retirement check.
    const std::array<DevicePassThroughDelay, 2> duplicate{{{"small", 192}, {"small", 256}}};
    rejects([&] {
        (void)planSceneTiming(document, timing, duplicate);
    });
    const DevicePassThroughDelay unknown{"missing", 256};
    rejects([&] {
        (void)planSceneTiming(document, timing, {&unknown, 1});
    });
}
void conversionAndFrozenPreparation() {
    SceneTiming timing;
    timing.blockFrames = 64;
    auto small = device("small");
    small.sampleRate = 44100;
    RouteDocument document{kRouteDocumentVersion, {small}};
    PublicPassThroughTiming policy(timing, [](const auto&) {
        return std::optional(PublicDeviceTiming{64, false});
    });
    const auto floors = policy.floors(document);
    assert(floors.size() == 1 && floors[0].frames == 317); // Two periods + quantum + sinc wing.

    document.devices[0].sampleRate = 48000;
    SceneRenderer renderer(timing, AudioScene::SourceResolver{});
    uint32_t floor = 192;
    renderer.setTimingFloors([&](const auto&) {
        return std::vector<DevicePassThroughDelay>{{"small", floor}};
    });
    DeviceTransportPool pool;
    auto prepared = pool.prepare(document, renderer.inputLatencyFrames(document));
    floor = 1088; // A new public period arrived between planning and staging.
    rejects([&] {
        (void)renderer.prepare(document, prepared.devices());
    });
    assert(!renderer.observe());
    prepared = pool.prepare(document, renderer.inputLatencyFrames(document));
    auto activation = renderer.prepare(document, prepared.devices());
    pool.commit(std::move(prepared));
    activation->commit();
    auto scene = renderer.observe();
    assert(scene && scene->deviceLatencyFrames(0) == 1088);
    assert(pool.devices()[0].inputLatencyFrames == 1089);
}
void plannedPreparationPinsSourceAndEndpointTiming() {
    SceneTiming timing;
    timing.blockFrames = 64;
    auto desired = device("planned");
    desired.routing.sources.push_back({"mic", 2, 1, true, false, defaultChannelConnections(2, 2)});
    desired.bindings.push_back({"mic", SourceKind::InputDevice, "hardware"});
    RouteDocument document{kRouteDocumentVersion, {desired}};
    SceneRenderer renderer(timing, AudioScene::SourceResolver{});
    uint32_t floor = 192, input = 288, floorQueries = 0, inputQueries = 0;
    renderer.setTimingFloors([&](const auto&) {
        ++floorQueries;
        return std::vector<DevicePassThroughDelay>{{"planned", floor}};
    });
    renderer.setInputTiming([&](const auto&) {
        ++inputQueries;
        return std::vector<DeviceInputDelay>{{"planned", "mic", input}};
    });
    auto plan = renderer.plan(document);
    assert(floorQueries == 1 && inputQueries == 1 && plan->inputLatencyFrames()[0] == 289);
    DeviceTransportPool pool;
    auto mappings = pool.prepare(document, plan->inputLatencyFrames());
    floor = 1088;
    input = 512;              // The live clocks change after allocation, before DSP staging.
    document.devices.clear(); // The plan also owns its document snapshot.
    auto update = plan->prepare(mappings.devices());
    assert(floorQueries == 1 && inputQueries == 1 && !renderer.observe());
    rejects([&] {
        (void)plan->prepare(mappings.devices());
    });
    pool.commit(std::move(mappings));
    update->commit();
    assert(renderer.observe()->deviceLatencyFrames(0) == 288);
    auto next = renderer.plan({kRouteDocumentVersion, {desired}});
    assert(next->inputLatencyFrames()[0] == 1089);
}
void firstRunningPeriodReplacesIdleDefault() {
    SceneTiming timing;
    timing.blockFrames = 64;
    std::optional<PublicDeviceTiming> profile;
    PublicPassThroughTiming policy(timing, [&](const auto&) {
        return profile;
    });
    RouteDocument document{kRouteDocumentVersion, {device("negotiated")}};
    assert(policy.floors(document)[0].frames == 1088); // Not yet published.
    profile = PublicDeviceTiming{512, false};
    assert(policy.floors(document)[0].frames == 1088); // Published, not running.
    profile = PublicDeviceTiming{32, true};
    assert(policy.floors(document)[0].frames == 160); // First client negotiated 32.
    profile->bufferFrames = 512;
    assert(policy.floors(document)[0].frames == 1088); // Grow within the session.
    profile->bufferFrames = 32;
    assert(policy.floors(document)[0].frames == 1088); // Keep the active high-water mark.
    profile = PublicDeviceTiming{512, false};
    assert(policy.floors(document)[0].frames == 1088);
    profile = PublicDeviceTiming{32, true};
    assert(policy.floors(document)[0].frames == 160); // A new observed session.

    // A tentative removal must not convert an idle fallback into a running
    // high-water mark, nor forget an already observed running high-water mark.
    profile = PublicDeviceTiming{512, false};
    assert(policy.floors(document)[0].frames == 1088);
    profile = PublicDeviceTiming{32, true};
    (void)policy.floors({kRouteDocumentVersion, {}});
    assert(policy.floors(document)[0].frames == 160);
    profile->bufferFrames = 512;
    assert(policy.floors(document)[0].frames == 1088);
    profile->bufferFrames = 32;
    (void)policy.floors({kRouteDocumentVersion, {}});
    assert(policy.floors(document)[0].frames == 1088);
    profile = PublicDeviceTiming{32, true, 2}; // Driver observed a new session between polls.
    assert(policy.floors(document)[0].frames == 160);
    profile = PublicDeviceTiming{512, true, 3, false}; // No playback callback observed yet.
    assert(policy.floors(document)[0].frames == 160); // Keep the last measured period, not a guess.
    profile = PublicDeviceTiming{32, true, 3};
    assert(policy.floors(document)[0].frames == 160);
}
void unmeasuredRestartDoesNotInventALargerPeriod() {
    SceneTiming timing;
    timing.blockFrames = 64;
    std::optional<PublicDeviceTiming> profile{PublicDeviceTiming{64, true, 7}};
    PublicPassThroughTiming policy(timing, [&](const auto&) {
        return profile;
    });
    RouteDocument document{kRouteDocumentVersion, {device("restart")}};
    assert(policy.floors(document)[0].frames == 192);
    profile = PublicDeviceTiming{512, true, 8, false};
    assert(policy.floors(document)[0].frames == 192);
    profile->running = false;
    assert(policy.floors(document)[0].frames == 192);
    profile = PublicDeviceTiming{256, true, 8};
    assert(policy.floors(document)[0].frames ==
           576); // Actual larger callbacks still grow the queue.
    profile = PublicDeviceTiming{512, true, 9, false};
    assert(policy.floors(document)[0].frames == 576);
    profile = PublicDeviceTiming{32, true, 9};
    assert(policy.floors(document)[0].frames == 160); // A measured new session can shrink it.
    profile.reset();
    assert(policy.floors(document)[0].frames == 1088); // Disappearance forgets the observation.
    profile = PublicDeviceTiming{512, true, 10, false};
    assert(policy.floors(document)[0].frames == 1088);
    profile = PublicDeviceTiming{64, true, 10};
    assert(policy.floors(document)[0].frames == 192);
    profile = PublicDeviceTiming{512, true, 0, false}; // Legacy driver or a fresh unpublished clock.
    assert(policy.floors(document)[0].frames == 1088);
    profile = PublicDeviceTiming{512, true, 11, false};
    assert(policy.floors(document)[0].frames == 1088); // The legacy boundary forgot the old period.
}
void timingPropertyValidation() {
    const auto dictionary = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);
    const auto setNumber = [&](CFStringRef key, int64_t number) {
        const auto value = CFNumberCreate(nullptr, kCFNumberSInt64Type, &number);
        CFDictionarySetValue(dictionary, key, value);
        CFRelease(value);
    };
    const auto invalid = [&] {
        rejects([&] {
            (void)decodePublicDeviceTiming(dictionary);
        });
    };
    setNumber(CFSTR("version"), 1);
    setNumber(CFSTR("session"), 0);
    setNumber(CFSTR("writePeriodFrames"), 0);
    CFDictionarySetValue(dictionary, CFSTR("running"), kCFBooleanFalse);
    auto decoded = decodePublicDeviceTiming(dictionary);
    assert(!decoded.running && !decoded.measured && decoded.bufferFrames == 512 &&
           !decoded.session);
    setNumber(CFSTR("session"), 7);
    setNumber(CFSTR("writePeriodFrames"), 32);
    CFDictionarySetValue(dictionary, CFSTR("running"), kCFBooleanTrue);
    decoded = decodePublicDeviceTiming(dictionary);
    assert(decoded.running && decoded.measured && decoded.bufferFrames == 32 &&
           decoded.session == 7);
    setNumber(CFSTR("version"), 2);
    invalid();
    setNumber(CFSTR("version"), 1);
    setNumber(CFSTR("session"), 0);
    invalid(); // Measured callbacks require a session.
    setNumber(CFSTR("session"), int64_t(UINT32_MAX) + 1);
    invalid();
    setNumber(CFSTR("session"), 7);
    setNumber(CFSTR("writePeriodFrames"), -1);
    invalid();
    setNumber(CFSTR("writePeriodFrames"), 32769);
    invalid();
    setNumber(CFSTR("writePeriodFrames"), 32);
    CFDictionarySetValue(dictionary, CFSTR("running"), CFSTR("true"));
    invalid();
    CFDictionarySetValue(dictionary, CFSTR("running"), kCFBooleanTrue);
    const double fractional = 32.5;
    const auto number = CFNumberCreate(nullptr, kCFNumberDoubleType, &fractional);
    CFDictionarySetValue(dictionary, CFSTR("writePeriodFrames"), number);
    CFRelease(number);
    invalid();
    CFDictionaryRemoveValue(dictionary, CFSTR("writePeriodFrames"));
    invalid();
    CFRelease(dictionary);
    rejects([] {
        (void)decodePublicDeviceTiming(nullptr);
    });
    rejects([] {
        (void)decodePublicDeviceTiming(CFSTR("wrong type"));
    });
}
void rejectedIntentRetainsActiveFloor() {
    SceneTiming timing;
    timing.blockFrames = 64;
    PublicDeviceTiming profile{512, true};
    PublicPassThroughTiming policy(timing, [&](const auto&) {
        return std::optional(profile);
    });
    RouteDocument document{kRouteDocumentVersion, {device("still-running")}};
    assert(policy.floors(document)[0].frames == 1088);
    profile.bufferFrames = 32;
    (void)policy.floors({kRouteDocumentVersion, {}}); // Tentative removal while IO is still live.
    assert(policy.floors(document)[0].frames == 1088);
    profile.running = false;
    (void)policy.floors({kRouteDocumentVersion, {}});
    assert(policy.floors(document)[0].frames == 160);
    PublicPassThroughTiming longLead(
        timing,
        [&](const auto&) {
            return std::optional(profile);
        },
        4096);
    assert(longLead.floors(document)[0].frames ==
           4192); // 32-frame period + 4096 lead + 64 quantum.
}
class Link final : public DriverConfigurationLink {
  public:
    DriverConfigurationResult result{0, DriverConfigurationPhase::Committed, 0};
    uint32_t submissions{0};
    uint64_t session() const noexcept override {
        return 1;
    }
    int32_t submit(std::span<const DeviceTransport>, uint64_t revision) override {
        ++submissions;
        result = {revision, DriverConfigurationPhase::Committed, 0};
        return 0;
    }
    DriverConfigurationResult status(uint64_t) override {
        return result;
    }
    int32_t cancel(uint64_t) override {
        return 0;
    }
    void invalidate() noexcept override {}
};
void controllerUsesOneTimingSnapshotPerSubmission() {
    SceneTiming timing;
    timing.blockFrames = 64;
    SceneRenderer renderer(timing, AudioScene::SourceResolver{});
    uint32_t queries = 0, floor = 192;
    bool changing = true;
    renderer.setTimingFloors([&](const auto&) {
        ++queries;
        const auto sampled = floor;
        if (changing)
            floor = floor == 192 ? 1088 : 192;
        return std::vector<DevicePassThroughDelay>{{"changing", sampled}};
    });
    Link link;
    ConfigurationController controller(link, {}, &renderer, false);
    controller.setDesired({kRouteDocumentVersion, {device("changing")}});
    controller.poll(100);
    assert(link.submissions == 1 && queries == 1 && !renderer.observe());
    controller.poll(200);
    assert(controller.status().phase == RestorationPhase::Restored && queries == 1);
    assert(renderer.observe()->deviceLatencyFrames(0) == 192);
    assert(controller.devices()[0].inputLatencyFrames == 193);
    changing = false; // The new observation is reconciled in the next transaction.
    controller.poll(300);
    assert(link.submissions == 2 && queries == 3);
    assert(renderer.observe()->deviceLatencyFrames(0) == 192);
    controller.poll(400);
    assert(controller.status().phase == RestorationPhase::Restored);
    assert(renderer.observe()->deviceLatencyFrames(0) == 1088);
    assert(controller.devices()[0].inputLatencyFrames == 1089);
    controller.poll(500);
    assert(link.submissions == 2);
}
void metadataOnlyReconfiguration() {
    SceneTiming timing;
    timing.blockFrames = 64;
    RouteDocument document{kRouteDocumentVersion, {device("small"), device("large")}};
    SceneRenderer renderer(timing, AudioScene::SourceResolver{});
    PublicDeviceTiming small{512, false};
    PublicPassThroughTiming policy(timing, [&](const auto& uid) {
        return std::optional(uid.ends_with(".small") ? small : PublicDeviceTiming{512, true});
    });
    renderer.setTimingFloors([&](const auto& desired) {
        return policy.floors(desired);
    });
    Link link;
    ConfigurationController controller(link, {}, &renderer, false);
    controller.setDesired(document);
    uint64_t now = 0;
    const auto settle = [&] {
        for (unsigned step = 0; step < 20; ++step) {
            controller.poll(now += 100);
            if (controller.status().phase == RestorationPhase::Restored)
                return;
        }
        assert(false);
    };
    settle();
    assert(link.submissions == 1);
    const auto desiredGeneration = controller.status().desiredGeneration;
    const auto largeMapping = controller.devices()[1].region;
    const auto canonical = encodeRouteDocumentJSON(*controller.desired());
    auto old = renderer.observe();
    assert(old->deviceLatencyFrames(0) == 1088);
    small = {32, true}; // The first client negotiated a smaller period than the idle default.
    controller.poll(now += 100);
    assert(link.submissions == 2 && old->deviceLatencyFrames(0) == 1088);
    assert(renderer.observe()->deviceLatencyFrames(0) ==
           1088); // Native acknowledgement is pending.
    settle();
    assert(renderer.observe()->deviceLatencyFrames(0) == 160);
    assert(controller.devices()[0].inputLatencyFrames == 161);
    assert(controller.devices()[1].region == largeMapping);
    assert(controller.status().desiredGeneration == desiredGeneration);
    assert(encodeRouteDocumentJSON(*controller.desired()) == canonical);
    assert(old->deviceLatencyFrames(0) == 1088); // A borrowed prior program stays immutable.
    small.bufferFrames = 256;
    controller.poll(now += 100);
    settle();
    assert(link.submissions == 3 && controller.devices()[0].inputLatencyFrames == 577);
    small.bufferFrames = 32;
    controller.poll(now += 100);
    assert(link.submissions == 3); // No shrink or native churn during the running session.
    assert(controller.devices()[1].region == largeMapping);
    assert(controller.status().desiredGeneration == desiredGeneration);
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--native-unknown") == 0) {
        SceneTiming timing;
        timing.blockFrames = 64;
        PublicPassThroughTiming policy(timing);
        RouteDocument doc{kRouteDocumentVersion, {device("unpublished-timing-preflight-00f7a922")}};
        const auto floors = policy.floors(doc);
        assert(floors.size() == 1 && floors[0].frames == 1088);
        return 0;
    }
    independentFloors();
    nativeIdentityValidationKeepsTimingFresh();
    conversionAndFrozenPreparation();
    plannedPreparationPinsSourceAndEndpointTiming();
    firstRunningPeriodReplacesIdleDefault();
    unmeasuredRestartDoesNotInventALargerPeriod();
    timingPropertyValidation();
    rejectedIntentRetainsActiveFloor();
    metadataOnlyReconfiguration();
    controllerUsesOneTimingSnapshotPerSubmission();
}
