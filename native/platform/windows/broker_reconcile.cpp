#include "broker_reconcile.hpp"
#include "device_operations.hpp"
#include <devguid.h>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace loopz::windows {
namespace {
std::optional<DeviceIntentRequest> latest(DeviceIntentStore& store, std::wstring_view owner,
                                          std::string_view mix, DeviceIntentKind kind) {
    const auto snapshot = store.snapshot(owner);
    const auto found =
        std::find_if(snapshot.records.begin(), snapshot.records.end(), [&](const auto& record) {
            return record.kind == kind && record.mixId == mix;
        });
    if (found == snapshot.records.end())
        return {};
    return *found;
}
bool same(const DeviceIntentRequest& a, const DeviceIntentRequest& b) {
    return a.operationId == b.operationId && a.expectedRevision == b.expectedRevision;
}
} // namespace
WindowsBrokerDeviceAdapter::WindowsBrokerDeviceAdapter(std::wstring installedInf)
    : installedInf_(std::move(installedInf)) {}
BrokerApplyResult WindowsBrokerDeviceAdapter::apply(std::wstring_view owner,
                                                    const DeviceIntentRequest& desired) {
    // Validate the typed payload even for trusted maintenance/test callers.
    (void)encodeDeviceIntent(desired);
    DeviceSetupLock lock;
    if (desired.action == DeviceIntentAction::Remove)
        return (desired.kind == DeviceIntentKind::AppRoutingSink
                    ? removeAppRoutingSink(owner)
                    : removeMixDevice(owner, desired.mixId))
                       .rebootRequired
                   ? BrokerApplyResult::RebootRequired
                   : BrokerApplyResult::Applied;
    struct Devices {
        HDEVINFO value{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr)};
        ~Devices() {
            if (value != INVALID_HANDLE_VALUE)
                SetupDiDestroyDeviceInfoList(value);
        }
    } devices;
    if (devices.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot open setup device set");
    if (desired.kind == DeviceIntentKind::AppRoutingSink) {
        const auto result = upsertAppRoutingSink(devices.value, owner, installedInf_);
        if (result.rebootRequired)
            return BrokerApplyResult::RebootRequired;
        finalizeAppRoutingSink(devices.value, owner);
        return BrokerApplyResult::Applied;
    }
    VirtualDeviceDefinition mix;
    mix.id = desired.mixId;
    mix.name = desired.name;
    mix.sampleRate = desired.sampleRate;
    mix.routing.outputChannels = desired.channels;
    const auto result = upsertMixDevice(devices.value, owner, mix, installedInf_);
    if (result.rebootRequired)
        return BrokerApplyResult::RebootRequired;
    finalizeMixDevice(devices.value, owner, mix);
    return BrokerApplyResult::Applied;
}
BrokerReconciler::BrokerReconciler(DeviceIntentStore& store, BrokerDeviceAdapter& adapter,
                                   HANDLE stopEvent, Progress progress)
    : store_(store), adapter_(adapter), stop_(stopEvent), progress_(std::move(progress)) {
    if (!stop_ || stop_ == INVALID_HANDLE_VALUE || WaitForSingleObject(stop_, 0) == WAIT_FAILED ||
        !progress_)
        throw std::invalid_argument("Invalid setup reconciliation owner");
}
void BrokerReconciler::invalidate() {
    completed_.clear();
}
bool BrokerReconciler::pass() {
    bool retry = false;
    for (const auto& owner : store_.ownersWithDesiredState()) {
        const auto snapshot = store_.snapshot(owner);
        for (const auto& desired : snapshot.records) {
            if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0)
                return false;
            const auto revision = desired.expectedRevision + 1;
            const Key key{owner, deviceIntentStorageKey(desired.kind, desired.mixId)};
            if (completed_.contains(key) && completed_.at(key) == revision)
                continue;
            auto current = latest(store_, owner, desired.mixId, desired.kind);
            if (!current || !same(*current, desired)) {
                retry = true;
                continue;
            }
            const auto publish = [&](BrokerDevicePhase phase, std::string error = {}) {
                progress_({owner, desired.mixId, revision, phase, std::move(error), desired.kind});
            };
            publish(BrokerDevicePhase::Applying);
            BrokerApplyResult applied;
            try {
                applied = adapter_.apply(owner, desired);
            } catch (const std::exception& error) {
                current = latest(store_, owner, desired.mixId, desired.kind);
                completed_.erase(key);
                if (current && same(*current, desired))
                    publish(BrokerDevicePhase::Error, std::string(error.what()).substr(0, 512));
                retry = true;
                continue;
            }
            current = latest(store_, owner, desired.mixId, desired.kind);
            if (!current || !same(*current, desired)) {
                completed_.erase(key);
                if (current)
                    progress_({owner,
                               desired.mixId,
                               current->expectedRevision + 1,
                               BrokerDevicePhase::Queued,
                               {},
                               desired.kind});
                retry = true;
                continue;
            }
            if (applied == BrokerApplyResult::RebootRequired) {
                completed_[key] = revision;
                publish(BrokerDevicePhase::RebootRequired);
            } else if (desired.action == DeviceIntentAction::Remove) {
                completed_.erase(key);
                if (store_.removalApplied(owner, desired.mixId, revision, desired.kind))
                    publish(BrokerDevicePhase::Removed);
                else
                    retry = true; // A delayed completion cannot erase a newer edit.
            } else {
                completed_[key] = revision;
                publish(BrokerDevicePhase::Ready);
            }
        }
    }
    return retry;
}
} // namespace loopz::windows
