#import <Foundation/Foundation.h>
#include <loopz/macos/configuration_controller.hpp>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace loopz;
using namespace loopz::macos;
using Phase = DriverConfigurationPhase;

RouteDocument document(uint32_t channels = 2) {
    RouteDocument result;
    for (unsigned i = 0; i < 3; ++i) {
        VirtualDeviceDefinition device;
        device.id = "device-" + std::to_string(i);
        device.name = device.id;
        device.routing.outputChannels = channels;
        device.routing.sources = {
            {"pass", channels, 1, true, false, defaultChannelConnections(channels, channels)}};
        device.bindings = {{"pass", SourceKind::PassThrough, "", false}};
        result.devices.push_back(std::move(device));
    }
    return result;
}
class Link final : public DriverConfigurationLink {
  public:
    uint64_t incarnation{1}, latest{0}, submitCount{0}, cancelCount{0};
    unsigned rejections{0}, queryFailures{0}, cancelRejections{0};
    bool throwBeforeSubmit{false}, throwAfterSubmit{false}, commitDuringCancel{false};
    bool disconnectDuringStatus{false}, disconnectDuringSubmit{false};
    std::map<uint64_t, DriverConfigurationResult> outcomes;
    std::vector<DeviceTransport> staged;
    uint64_t session() const noexcept override {
        return incarnation;
    }
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override {
        ++submitCount;
        if (rejections) {
            --rejections;
            return -50;
        }
        if (throwBeforeSubmit) {
            throwBeforeSubmit = false;
            throw std::runtime_error("Submit request lost before acceptance");
        }
        assert(revision > latest);
        latest = revision;
        staged.assign(devices.begin(), devices.end());
        outcomes[revision] = {revision, Phase::Pending, 0};
        if (disconnectDuringSubmit) {
            disconnectDuringSubmit = false;
            incarnation = 0;
        }
        if (throwAfterSubmit) {
            throwAfterSubmit = false;
            throw std::runtime_error("Submit reply lost after acceptance");
        }
        return 0;
    }
    DriverConfigurationResult status(uint64_t revision) override {
        if (queryFailures) {
            --queryFailures;
            throw std::runtime_error("Transient status query failure");
        }
        if (disconnectDuringStatus) {
            disconnectDuringStatus = false;
            ++incarnation;
        }
        if (!revision)
            return latest ? outcomes.at(latest) : DriverConfigurationResult{0, Phase::Committed, 0};
        const auto found = outcomes.find(revision);
        return found == outcomes.end() ? DriverConfigurationResult{revision, Phase::Unknown, 0}
                                       : found->second;
    }
    int32_t cancel(uint64_t revision) override {
        ++cancelCount;
        if (cancelRejections) {
            --cancelRejections;
            return -50;
        }
        auto& result = outcomes.at(revision);
        if (commitDuringCancel)
            result.phase = Phase::Committed;
        else if (result.phase == Phase::Pending)
            result.phase = Phase::RollingBack;
        return 0;
    }
    void invalidate() noexcept override {
        incarnation = 0;
    }
    void finish(Phase phase = Phase::Committed) {
        outcomes.at(latest) = {latest, phase, phase == Phase::Committed ? 0 : -128};
    }
};

void settle(ConfigurationController& controller, Link& link, uint64_t& now) {
    for (unsigned i = 0; i < 100; ++i) {
        controller.poll(now);
        if (controller.status().phase == RestorationPhase::Restored)
            return;
        if (controller.status().transactionRevision) {
            const auto phase = link.outcomes.at(link.latest).phase;
            if (phase == Phase::Pending)
                link.finish();
            else if (phase == Phase::RollingBack)
                link.finish(Phase::Aborted);
        }
        now += 100;
    }
    assert(false && "Controller did not restore the desired graph");
}
void stableTimingInspectionIsBoundedButRecoveryBypassesIt() {
    struct Activation final : GraphActivation {
        mutable uint32_t inspections{0};
        uint32_t latency{129};
        struct Prepared final : PreparedGraphActivation {
            void commit() noexcept override {}
        };
        struct Plan final : GraphActivationPlan {
            std::vector<uint32_t> latencies;
            explicit Plan(std::vector<uint32_t> values) : latencies(std::move(values)) {}
            std::span<const uint32_t> inputLatencyFrames() const noexcept override {
                return latencies;
            }
            std::unique_ptr<PreparedGraphActivation>
            prepare(std::span<const DeviceTransport>) override {
                return std::make_unique<Prepared>();
            }
        };
        std::vector<uint32_t> inputLatencyFrames(const RouteDocument& route) const override {
            ++inspections;
            return std::vector<uint32_t>(route.devices.size(), latency);
        }
        std::unique_ptr<GraphActivationPlan> plan(const RouteDocument& route) override {
            return std::make_unique<Plan>(inputLatencyFrames(route));
        }
        std::unique_ptr<PreparedGraphActivation>
        prepare(const RouteDocument&, std::span<const DeviceTransport>) override {
            return std::make_unique<Prepared>();
        }
    } activation;
    Link link;
    ConfigurationController controller(link, {}, &activation, false, 250);
    controller.setDesired(document());
    uint64_t now = 0;
    settle(controller, link, now);
    const auto initial = activation.inspections;
    controller.poll(now += 100);
    assert(activation.inspections == initial + 1);
    controller.poll(now += 100);
    controller.poll(now += 100);
    assert(activation.inspections == initial + 1);
    controller.poll(now += 100);
    assert(activation.inspections == initial + 2);

    activation.latency = 257;
    controller.retryNow(++now);
    controller.poll(now);
    assert(link.submitCount == 2 && link.staged.front().inputLatencyFrames == 257);
    // The transaction plans fresh metadata rather than using a cached budget.
    link.finish();
    settle(controller, link, now);
    assert(controller.devices().front().inputLatencyFrames == 257);
}

void rejectionRollbackAndQuarantine() {
    Link link;
    ConfigurationController controller(link);
    assert(controller.status().phase == RestorationPhase::NoDesiredDocument);
    controller.poll(0);
    assert(!link.submitCount); // No implicit empty graph that deletes old devices.
    auto desired = document();
    controller.setDesired(desired);
    uint64_t now = 0;
    settle(controller, link, now);
    const auto old = controller.devices();
    assert(old.size() == 3);
    auto wider = document(8);
    link.rejections = 2;
    controller.setDesired(wider);
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::Retry);
    assert(controller.devices()[0].region == old[0].region);
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::Retry);
    controller.poll(now += 200);
    assert(controller.status().phase == RestorationPhase::Applying);
    const auto tentative = link.staged;
    link.queryFailures = 1;
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::Applying);
    assert(!tentative[0].region->header().retired.load());
    assert(link.submitCount == 4); // A failed query cannot overlap a submission.
    link.finish(Phase::Aborted);
    controller.poll(now += 1000);
    assert(tentative[0].region->header().retired.load());
    for (const auto& active : old)
        assert(!active.region->header().retired.load());
    // Quarantine can invalidate just one old mapping. Its siblings stay owned.
    controller.retryNow(now);
    controller.poll(now);
    old[0].region->retire();
    link.finish(Phase::Quarantined);
    controller.poll(now += 100);
    settle(controller, link, now);
    assert(controller.desired()->devices.size() == 3);
    assert(controller.active()->devices[0].routing.outputChannels == 8);
    for (size_t i = 0; i < old.size(); ++i)
        assert(controller.devices()[i].region != old[i].region);
    // A retired active mapping also triggers reconciliation without an edit.
    const auto stable = controller.devices();
    stable[1].region->retire();
    controller.poll(now += 100);
    settle(controller, link, now);
    assert(controller.devices()[0].region == stable[0].region);
    assert(controller.devices()[1].region != stable[1].region);
    assert(controller.devices()[2].region == stable[2].region);
}
void newerIntentAndCancelRace() {
    for (const bool race : {false, true}) {
        Link link;
        ConfigurationController controller(link);
        controller.setDesired(document());
        uint64_t now = 0;
        settle(controller, link, now);
        controller.setDesired(document(4));
        controller.poll(now += 100);
        const auto superseded = controller.status().transactionRevision;
        controller.setDesired(document(8));
        link.commitDuringCancel = race;
        controller.poll(now += 100);
        assert(link.cancelCount == 1);
        assert(controller.desired()->devices[0].routing.outputChannels == 8);
        if (!race) {
            assert(controller.status().phase == RestorationPhase::Cancelling);
            controller.poll(now += 100);
            assert(link.cancelCount == 1);
            link.finish(Phase::Aborted);
        } else {
            assert(controller.active()->devices[0].routing.outputChannels == 4);
            assert(controller.status().phase != RestorationPhase::Restored);
        }
        settle(controller, link, now);
        assert(controller.status().transactionRevision != superseded);
        assert(controller.active()->devices[0].routing.outputChannels == 8);
        const auto count = link.submitCount;
        controller.setDesired(document(8));
        controller.poll(now += 100);
        assert(link.submitCount == count);
    }
}
void sessionChangesAndUncertainSubmits() {
    Link link;
    ConfigurationController controller(link);
    controller.setDesired(document());
    uint64_t now = 0;
    link.throwBeforeSubmit = true;
    controller.poll(now);
    assert(controller.status().phase == RestorationPhase::Applying);
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::Retry);
    link.throwAfterSubmit = true;
    controller.retryNow(now);
    controller.poll(now);
    assert(controller.status().phase == RestorationPhase::Applying);
    const auto staged = link.staged;
    link.queryFailures = 1;
    controller.poll(now += 100);
    assert(!staged[0].region->header().retired.load());
    link.finish();
    controller.poll(now += 1000);
    assert(controller.status().phase == RestorationPhase::Restored);
    const auto active = controller.devices();
    controller.setDesired(document(4));
    controller.poll(now += 100);
    const auto lost = link.staged;
    link.incarnation = 0;
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::WaitingForDriver);
    assert(lost[0].region->header().retired.load());
    assert(!active[0].region->header().retired.load());
    // Reconnect to a driver with an abandoned approval from the predecessor.
    link.incarnation = 2;
    controller.poll(now += 100);
    assert(link.cancelCount == 1 && link.submitCount == 3);
    link.finish(Phase::Aborted);
    settle(controller, link, now);
    assert(controller.active()->devices[0].routing.outputChannels == 4);
    // An acknowledgement arriving across an incarnation change is stale.
    controller.setDesired(document(8));
    controller.poll(now += 100);
    const auto transient = link.staged;
    link.finish();
    link.disconnectDuringStatus = true;
    controller.poll(now += 100);
    assert(controller.status().phase != RestorationPhase::Restored);
    assert(transient[0].region->header().retired.load());
    assert(controller.active()->devices[0].routing.outputChannels == 4);
    settle(controller, link, now);
    assert(controller.active()->devices[0].routing.outputChannels == 8);
    controller.setDesired(document(16));
    link.disconnectDuringSubmit = true;
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::WaitingForDriver);
    assert(link.staged[0].region->header().retired.load());
}
void cancelledAndCompactedReplies() {
    Link link;
    ConfigurationController controller(link);
    controller.setDesired(document());
    uint64_t now = 0;
    controller.poll(now);
    controller.setDesired(document(8));
    link.cancelRejections = 1;
    controller.poll(now += 100);
    assert(link.cancelCount == 1 && controller.status().phase == RestorationPhase::Applying);
    controller.poll(now += 100);
    assert(link.cancelCount == 2 && controller.status().phase == RestorationPhase::Cancelling);
    link.finish(Phase::Aborted);
    settle(controller, link, now);
    assert(controller.active()->devices[0].routing.outputChannels == 8);
    controller.setDesired(document(16));
    controller.poll(now += 100);
    const auto unconfirmed = link.staged;
    // A peer that loses this exclusive owner's history cannot authorize a
    // commit by returning a newer, unrelated revision.
    link.outcomes.erase(link.latest);
    ++link.latest;
    link.outcomes[link.latest] = {link.latest, Phase::Committed, 0};
    controller.poll(now += 100);
    assert(!link.incarnation && controller.status().phase == RestorationPhase::WaitingForDriver);
    assert(unconfirmed[0].region->header().retired.load());
    assert(controller.desired()->devices[0].routing.outputChannels == 16);
    link.incarnation = 2;
    settle(controller, link, now);
    assert(controller.active()->devices[0].routing.outputChannels == 16);
}
void maximumSavedIntent() {
    RouteDocument full;
    const auto example = document().devices[0];
    for (unsigned index = 0; index < kMaximumVirtualDevices; ++index) {
        auto item = example;
        item.id = "full-" + std::to_string(index);
        item.name = item.id;
        item.routing.enabled = index != 7;
        full.devices.push_back(std::move(item));
    }
    full.devices[0].routing.sources.push_back(
        {"app", 2, 0.5f, true, false, defaultChannelConnections(2, 2)});
    full.devices[0].bindings.push_back({"app", SourceKind::Application, "example.app", true});
    full.devices[1].routing.sources.push_back(
        {"device-source", 2, 1, true, true, defaultChannelConnections(2, 2)});
    full.devices[1].bindings.push_back(
        {"device-source", SourceKind::InputDevice, "hardware", false});
    full.devices[2].routing.sources.push_back(
        {"virtual-source", 2, 1, false, false, defaultChannelConnections(2, 2)});
    full.devices[2].bindings.push_back(
        {"virtual-source", SourceKind::VirtualDevice, "full-0", false});
    full.devices[0].monitors.push_back(
        {"monitor", "hardware-output", 2, 0.25f, true, true, defaultChannelConnections(2, 2)});
    Link link;
    link.rejections = 3;
    ConfigurationController controller(link);
    controller.setDesired(full);
    uint64_t now = 0;
    settle(controller, link, now);
    assert(controller.devices().size() == 31 && controller.desired()->devices.size() == 32);
    assert(encodeRouteDocumentJSON(*controller.active()) == encodeRouteDocumentJSON(full));
    const auto previous = controller.devices();
    link.incarnation = 0;
    controller.poll(now += 100);
    assert(controller.status().phase == RestorationPhase::WaitingForDriver);
    link.incarnation = 2;
    link.latest = 0;
    link.outcomes.clear();
    settle(controller, link, now);
    assert(encodeRouteDocumentJSON(*controller.active()) == encodeRouteDocumentJSON(full));
    for (size_t index = 0; index < previous.size(); ++index)
        assert(controller.devices()[index].region == previous[index].region);
    // Availability recovery retains disabled sources/devices, mute/gain,
    // monitors, and virtual dependencies as well as the visible device list.
    assert(!controller.active()->devices[7].routing.enabled);
    assert(controller.active()->devices[0].monitors[0].gain == 0.25f);
    assert(controller.active()->devices[2].bindings[1].selector == "full-0");
}
void persistenceAndFailedEdits() {
    char pattern[] = "/tmp/loopz-controller-XXXXXX";
    const auto directory = ::mkdtemp(pattern);
    assert(directory);
    const auto path = std::filesystem::path(directory) / "routes.json";
    auto desired = document();
    saveRouteDocumentFile(path, desired);
    Link link;
    {
        ConfigurationController controller(link, path);
        assert(controller.desired()->devices.size() == 3);
        uint64_t now = 0;
        settle(controller, link, now);
        controller.saveDesired(document(8));
        assert(loadRouteDocumentFile(path).document.devices[0].routing.outputChannels == 8);
        const auto generation = controller.status().desiredGeneration;
        auto invalid = document(8);
        invalid.devices[0].sampleRate = 96001;
        bool rejected = false;
        try {
            controller.saveDesired(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected && controller.status().desiredGeneration == generation);
        assert(loadRouteDocumentFile(path).document.devices[0].routing.outputChannels == 8);
        const auto held = path.string() + ".held";
        std::filesystem::rename(path, held);
        std::filesystem::create_directory(path);
        rejected = false;
        try {
            controller.saveDesired(document(16));
        } catch (const std::exception&) {
            rejected = true;
        }
        assert(rejected && controller.status().desiredGeneration == generation);
        assert(controller.desired()->devices[0].routing.outputChannels == 8);
        std::filesystem::remove(path);
        std::filesystem::rename(held, path);
        // Corruption recovers the previous full document without rewriting it.
        {
            std::ofstream file(path);
            file << "broken";
        }
        controller.reloadDesired();
        assert(controller.status().recoveredBackup);
        assert(controller.desired()->devices[0].routing.outputChannels == 2);
        {
            std::ofstream file(path);
            file << "{\"version\":999}";
        }
        rejected = false;
        try {
            controller.reloadDesired();
        } catch (const UnsupportedRouteVersion&) {
            rejected = true;
        }
        assert(rejected && controller.desired()->devices.size() == 3);
        assert(controller.desired()->devices[0].routing.outputChannels == 2);
    }
    // Process reconstruction restores saved intent rather than a prior runtime
    // subset. Use an intact file and a new controller/driver session.
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".backup");
    saveRouteDocumentFile(path, document(8));
    Link nextLink;
    {
        ConfigurationController recreated(nextLink, path);
        uint64_t now = 0;
        settle(recreated, nextLink, now);
        assert(recreated.devices().size() == 3);
        assert(recreated.active()->devices[2].routing.outputChannels == 8);
    }
    for (const auto& device : nextLink.staged)
        assert(device.region->header().retired.load());
    std::filesystem::remove_all(directory);
}
} // namespace
int main() {
    @autoreleasepool {
        stableTimingInspectionIsBoundedButRecoveryBypassesIt();
        rejectionRollbackAndQuarantine();
        newerIntentAndCancelRace();
        sessionChangesAndUncertainSubmits();
        cancelledAndCompactedReplies();
        maximumSavedIntent();
        persistenceAndFailedEdits();
        std::puts(
            "Configuration controller recovery, uncertain replies, intent and persistence passed");
    }
}
