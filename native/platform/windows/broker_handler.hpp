#pragma once
#include "broker_pipe.hpp"
#include "device_intent_store.hpp"
#include "broker_reconcile.hpp"
#include <optional>
#include <functional>

namespace loopz::windows {
struct BrokerCommandResult {
    JSON reply;
    bool reconcileRequested{};
};
// owner must come from BrokerPipe, never a request field. Success means durable
// acceptance, not device readiness. Reconciliation always reads the latest store
// snapshot, including when an old idempotent retry wakes the worker.
// Snapshot reads are owner-scoped, revision-bound pages below the pipe reply
// limit; they never mutate intent or authorize absence-based device deletion.
using BrokerProgressQuery = std::function<std::optional<BrokerDeviceProgress>(
    std::wstring_view owner, std::string_view mixId, DeviceIntentKind kind)>;
BrokerCommandResult handleBrokerMessage(DeviceIntentStore& store, const BrokerMessage& message,
                                        const BrokerProgressQuery& progress = {});
// One pipe/control owner. PnP work belongs on a separate worker, signaled only
// after commit. Lost replies never roll back accepted desired state.
void runBrokerRequests(BrokerPipe& pipe, DeviceIntentStore& store, HANDLE reconcileEvent,
                       const BrokerProgressQuery& progress = {});
} // namespace loopz::windows
