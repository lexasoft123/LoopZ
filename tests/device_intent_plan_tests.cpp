#include <loopz/device_intent_plan.hpp>
#include <cassert>
#include <limits>
#include <iostream>
using namespace loopz;
namespace {
constexpr auto operation = "12345678-1234-1234-1234-123456789abc";
RouteDocument document(std::string id = "mix") {
    RouteDocument result;
    VirtualDeviceDefinition device;
    device.id = std::move(id);
    device.name = "Saved microphone";
    result.devices.push_back(std::move(device));
    return result;
}
template <class F> void rejects(F action) {
    bool rejected{};
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace
int main() {
    auto saved = document();
    DeviceIntentSnapshot actual;
    auto create = nextSavedDeviceIntent(saved, true, actual, operation);
    assert(create && create->action == DeviceIntentAction::Upsert &&
           create->expectedRevision == 0 && create->mixId == "mix");
    actual = {1, {*create}};
    assert(!nextSavedDeviceIntent(saved, true, actual, "")); // Accepted before a lost reply.
    auto edited = saved;
    edited.devices[0].name = "Renamed";
    edited.devices[0].sampleRate = 96000;
    edited.devices[0].routing.outputChannels = 8;
    actual.revision = 42;
    const auto edit = nextSavedDeviceIntent(edited, true, actual, operation);
    assert(edit && edit->expectedRevision == 42 && edit->name == "Renamed" &&
           edit->sampleRate == 96000 && edit->channels == 8);
    RouteDocument empty;
    assert(!nextSavedDeviceIntent(empty, false, actual, operation));
    const auto remove = nextSavedDeviceIntent(empty, true, actual, operation);
    assert(remove && remove->action == DeviceIntentAction::Remove &&
           remove->expectedRevision == 42);
    actual = {43, {*remove}};
    assert(!nextSavedDeviceIntent(empty, true, actual, operation));
    const auto undo = nextSavedDeviceIntent(saved, true, actual, operation);
    assert(undo && undo->action == DeviceIntentAction::Upsert && undo->expectedRevision == 43);
    saved.devices[0].routing.enabled = false;
    assert(nextSavedDeviceIntent(saved, true, {},
                                 operation)); // Disabled routing retains a microphone.
    RouteDocument replacement;
    actual = {32, {}};
    for (unsigned i = 0; i < 32; ++i) {
        auto device = document("new-" + std::to_string(i)).devices.front();
        replacement.devices.push_back(device);
        DeviceIntentRequest old;
        old.operationId = operation;
        old.mixId = "old-" + std::to_string(i);
        old.name = "Old microphone";
        actual.records.push_back(old);
    }
    const auto first = nextSavedDeviceIntent(replacement, true, actual, operation);
    assert(first &&
           first->action == DeviceIntentAction::Remove); // Free live quota before replacement.
    replacement.devices[0].name = std::string(4097, 'x');
    rejects([&] {
        nextSavedDeviceIntent(replacement, true, actual, operation);
    });
    auto invalid = document();
    invalid.devices[0].sampleRate = std::numeric_limits<double>::quiet_NaN();
    rejects([&] {
        nextSavedDeviceIntent(invalid, true, {}, operation);
    });
    invalid.version = 0;
    assert(!nextSavedDeviceIntent(invalid, false, actual, operation));
    DeviceIntentRequest sink;
    sink.operationId = operation;
    sink.kind = DeviceIntentKind::AppRoutingSink;
    actual = {1, {sink}};
    assert(!nextSavedDeviceIntent(empty, true, actual, operation));
    const auto withSink = nextSavedDeviceIntent(document(), true, actual, operation);
    assert(withSink && withSink->kind == DeviceIntentKind::MicrophoneMix &&
           withSink->mixId == "mix");
    actual.records.push_back(sink);
    rejects([&] {
        nextSavedDeviceIntent(empty, true, actual, operation);
    });
    auto matching = document();
    actual = {1, {*create, *create}};
    rejects([&] {
        nextSavedDeviceIntent(matching, true, actual, operation);
    });
    actual = {std::numeric_limits<uint64_t>::max(), {*create}};
    assert(!nextSavedDeviceIntent(matching, true, actual, ""));
    matching.devices[0].name = "Changed at exhausted revision";
    rejects([&] {
        nextSavedDeviceIntent(matching, true, actual, operation);
    });
    rejects([&] {
        nextSavedDeviceIntent(matching, true, {}, "bad operation");
    });
    {
        using Demand = AppRoutingSinkDemand;
        AppRoutingSinkFacts facts;
        assert(appRoutingSinkDemand(facts) == Demand::Unknown);
        facts.savedAuthoritative = true;
        facts.recoveryExamined = true;
        assert(appRoutingSinkDemand(facts) ==
               Demand::Unknown); // Empty journal alone is insufficient.
        facts.endpointsExamined = true;
        assert(appRoutingSinkDemand(facts) == Demand::Unknown); // Defaults still unknown.
        facts.defaultsExamined = true;
        assert(appRoutingSinkDemand(facts) == Demand::Released);
        facts.endpointOccupied = true; // Includes inactive live and manual clients.
        assert(appRoutingSinkDemand(facts) == Demand::Required);
        facts = {};
        facts.endpointOccupied = true;
        assert(appRoutingSinkDemand(facts) ==
               Demand::Required); // Partial positive evidence retains.
        facts = {};
        facts.defaultEndpoint = true;
        assert(appRoutingSinkDemand(facts) == Demand::Required);
        facts = {};
        facts.pendingReceipts = true;
        assert(appRoutingSinkDemand(facts) == Demand::Required);
        facts = {};
        facts.desiredApplications = true;
        assert(appRoutingSinkDemand(facts) == Demand::Unknown);
        facts.savedAuthoritative = true;
        assert(appRoutingSinkDemand(facts) == Demand::Required);
        // Loss of any proof invalidates a previously safe release decision.
        const AppRoutingSinkFacts releasable{true, true, false, false, true, false, true, false};
        auto missing = releasable;
        missing.savedAuthoritative = false;
        assert(appRoutingSinkDemand(missing) == Demand::Unknown);
        missing = releasable;
        missing.recoveryExamined = false;
        assert(appRoutingSinkDemand(missing) == Demand::Unknown);
        missing = releasable;
        missing.endpointsExamined = false;
        assert(appRoutingSinkDemand(missing) == Demand::Unknown);
        missing = releasable;
        missing.defaultsExamined = false;
        assert(appRoutingSinkDemand(missing) == Demand::Unknown);
        DeviceIntentSnapshot shared{1, {*create}};
        auto upsert = nextAppRoutingSinkIntent(Demand::Required, shared, operation);
        assert(upsert && upsert->kind == DeviceIntentKind::AppRoutingSink &&
               upsert->expectedRevision == 1 && upsert->mixId.empty() && upsert->name.empty() &&
               upsert->sampleRate == 48000 && upsert->channels == 2);
        shared.revision = 2;
        shared.records.push_back(*upsert);
        assert(!nextAppRoutingSinkIntent(Demand::Required, shared, ""));
        assert(!nextAppRoutingSinkIntent(Demand::Unknown, shared, ""));
        auto release = nextAppRoutingSinkIntent(Demand::Released, shared, operation);
        assert(release && release->kind == DeviceIntentKind::AppRoutingSink &&
               release->action == DeviceIntentAction::Remove && release->expectedRevision == 2);
        shared.revision = 3;
        shared.records.back() = *release;
        assert(!nextAppRoutingSinkIntent(Demand::Released, shared, ""));
        const auto sinkUndo = nextAppRoutingSinkIntent(Demand::Required, shared, operation);
        assert(sinkUndo && sinkUndo->action == DeviceIntentAction::Upsert &&
               sinkUndo->expectedRevision == 3);
        assert(shared.records.front().mixId == create->mixId &&
               shared.records.front().expectedRevision == create->expectedRevision);
        assert(!nextAppRoutingSinkIntent(Demand::Released, {}, ""));
        rejects([&] {
            nextAppRoutingSinkIntent(Demand::Required, {}, "invalid operation");
        });
        shared.records.push_back(shared.records.back());
        rejects([&] {
            nextAppRoutingSinkIntent(Demand::Required, shared, operation);
        });
        shared.records.pop_back();
        shared.records.back().expectedRevision = shared.revision;
        rejects([&] {
            nextAppRoutingSinkIntent(Demand::Required, shared, operation);
        });
        shared.records.back().expectedRevision = 2;
        shared.records.back().channels = 8;
        rejects([&] {
            nextAppRoutingSinkIntent(Demand::Required, shared, operation);
        });
        shared = {std::numeric_limits<uint64_t>::max(), {}};
        rejects([&] {
            nextAppRoutingSinkIntent(Demand::Required, shared, operation);
        });
        rejects([&] {
            nextAppRoutingSinkIntent(static_cast<Demand>(99), {}, operation);
        });
    }
    std::cout << "Saved intent planning passed: lost replies, explicit removal, undo, quota and "
                 "invalid-state guards\n";
}
