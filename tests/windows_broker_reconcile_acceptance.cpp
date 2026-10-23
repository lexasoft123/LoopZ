// Opt-in administrative SQLite fixture. Device operations are simulated.
#include "broker_reconcile.hpp"
#include "device_operations.hpp"
#include <future>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <algorithm>

using namespace loopz;
using namespace loopz::windows;
namespace {
void require(bool value) {
    if (!value)
        throw std::runtime_error("Reconciliation regression failed");
}
DeviceIntentRequest request(unsigned id, uint64_t revision,
                            DeviceIntentAction action = DeviceIntentAction::Upsert) {
    DeviceIntentRequest result;
    char operation[37]{};
    std::snprintf(operation, sizeof(operation), "12345678-1234-1234-1234-%012u", id);
    result.operationId = operation;
    result.expectedRevision = revision;
    result.action = action;
    result.mixId = "mix";
    result.name = "Reconcile fixture";
    return result;
}
struct Adapter final : BrokerDeviceAdapter {
    unsigned calls{};
    bool fail{}, reboot{};
    std::function<void(const DeviceIntentRequest&)> hook;
    BrokerApplyResult apply(std::wstring_view, const DeviceIntentRequest& desired) override {
        ++calls;
        if (hook) {
            auto callback = std::move(hook);
            hook = {};
            callback(desired);
        }
        if (fail) {
            fail = false;
            throw std::runtime_error("Injected device failure");
        }
        return reboot ? BrokerApplyResult::RebootRequired : BrokerApplyResult::Applied;
    }
};
} // namespace
int main() {
    std::filesystem::path root;
    try {
        {
            WindowsBrokerDeviceAdapter actual(L"../oem1.inf");
            auto invalidSink = request(999, 0);
            invalidSink.kind = DeviceIntentKind::AppRoutingSink;
            bool refused = false;
            try {
                actual.apply(L"S-1-5-18", invalidSink);
            } catch (const std::invalid_argument&) {
                refused = true;
            }
            require(refused); // Invalid typed payload rejected before setup lock/PnP.
            invalidSink.mixId.clear();
            invalidSink.name.clear();
            refused = false;
            try {
                actual.apply(L"S-1-5-18", invalidSink);
            } catch (const std::invalid_argument&) {
                refused = true;
            }
            require(refused); // No caller-supplied package path reaches device registration.
        }
        {
            DeviceSetupLock held;
            auto blocked = std::async(std::launch::async, [] {
                try {
                    DeviceSetupLock second;
                    return false;
                } catch (const std::exception&) {
                    return true;
                }
            });
            require(blocked.get());
        }
        root = deviceIntentStoreRoot().parent_path() /
               (L"LoopZReconcileTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        require(!std::filesystem::exists(root));
        provisionDeviceIntentStore(root);
        const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(stop != nullptr);
        struct Event {
            HANDLE value;
            ~Event() {
                CloseHandle(value);
            }
        } close{stop};
        {
            DeviceIntentStore store(root), peer(root);
            constexpr auto owner = L"S-1-5-21-1-2-3-1001";
            constexpr auto other = L"S-1-5-21-1-2-3-1002";
            require(store.accept(owner, request(1, 0)) == 1);
            require(store.accept(other, request(1, 0)) == 1);
            Adapter adapter;
            std::vector<BrokerDeviceProgress> progress;
            BrokerReconciler worker(store, adapter, stop, [&](const auto& update) {
                progress.push_back(update);
            });
            require(!worker.pass() && adapter.calls == 2);
            require(!worker.pass() && adapter.calls == 2); // Unchanged ready work is cached.
            worker.invalidate();
            require(!worker.pass() && adapter.calls == 4);
            require(store.accept(owner, request(2, 1)) == 2);
            adapter.hook = [&](const auto&) {
                require(peer.accept(owner, request(3, 2)) == 3);
            };
            require(worker.pass());
            require(std::none_of(progress.begin(), progress.end(), [&](const auto& update) {
                return update.owner == owner && update.revision == 2 &&
                       update.phase == BrokerDevicePhase::Ready;
            }));
            require(!worker.pass());
            require(store.accept(owner, request(4, 3, DeviceIntentAction::Remove)) == 4);
            adapter.hook = [&](const auto& desired) {
                require(desired.action == DeviceIntentAction::Remove);
                require(peer.accept(owner, request(5, 4)) ==
                        5); // Undo while PnP removal is in flight.
            };
            require(worker.pass());
            const auto undone = store.snapshot(owner);
            require(undone.revision == 5 && undone.records.size() == 1 &&
                    undone.records[0].action == DeviceIntentAction::Upsert);
            require(!worker.pass());
            require(store.accept(owner, request(6, 5)) == 6);
            require(store.accept(other, request(2, 1)) == 2);
            adapter.fail = true;
            require(worker.pass());
            require(std::any_of(progress.begin(), progress.end(), [&](const auto& update) {
                return update.owner == owner && update.revision == 6 &&
                       update.phase == BrokerDevicePhase::Error;
            }));
            require(std::any_of(progress.begin(), progress.end(), [&](const auto& update) {
                return update.owner == other && update.revision == 2 &&
                       update.phase == BrokerDevicePhase::Ready;
            }));
            require(!worker.pass());
            require(store.accept(owner, request(7, 6, DeviceIntentAction::Remove)) == 7);
            adapter.reboot = true;
            require(!worker.pass());
            const auto calls = adapter.calls;
            require(!worker.pass() && adapter.calls == calls);
            require(store.snapshot(owner).records.size() ==
                    1); // Reboot never acknowledges deletion.
            require(progress.back().phase == BrokerDevicePhase::RebootRequired);
            adapter.reboot = false;
            worker.invalidate();
            require(!worker.pass());
            require(store.snapshot(owner).records.empty() && store.snapshot(owner).revision == 7);
            require(store.snapshot(other).records.size() == 1); // No implicit pruning.
            // Typed acceptance uses the same protected store and shared owner
            // revision as ordinary mixes; no direct-database seeding.
            constexpr auto typedOwner = L"S-1-5-21-1-2-3-1003";
            const auto sinkRequest = [&](unsigned id, uint64_t revision,
                                         DeviceIntentAction action) {
                auto sink = request(id, revision, action);
                sink.kind = DeviceIntentKind::AppRoutingSink;
                sink.mixId.clear();
                sink.name.clear();
                return sink;
            };
            require(store.accept(typedOwner, sinkRequest(1, 0, DeviceIntentAction::Upsert)) == 1);
            auto namedMix = request(2, 1);
            namedMix.mixId = "LoopZ.AppRoutingSink.v1";
            require(store.accept(typedOwner, namedMix) == 2);
            require(!worker.pass());
            require(std::any_of(progress.begin(), progress.end(), [&](const auto& update) {
                return update.owner == typedOwner &&
                       update.kind == DeviceIntentKind::AppRoutingSink && update.revision == 1 &&
                       update.phase == BrokerDevicePhase::Ready;
            }));
            require(std::any_of(progress.begin(), progress.end(), [&](const auto& update) {
                return update.owner == typedOwner &&
                       update.kind == DeviceIntentKind::MicrophoneMix &&
                       update.mixId == namedMix.mixId && update.revision == 2 &&
                       update.phase == BrokerDevicePhase::Ready;
            }));
            const auto typedCalls = adapter.calls;
            require(!worker.pass() && adapter.calls == typedCalls);
            require(store.accept(typedOwner, sinkRequest(3, 2, DeviceIntentAction::Remove)) == 3);
            adapter.hook = [&](const auto& desired) {
                require(desired.kind == DeviceIntentKind::AppRoutingSink &&
                        desired.action == DeviceIntentAction::Remove);
                require(store.accept(typedOwner, sinkRequest(4, 3, DeviceIntentAction::Upsert)) ==
                        4);
            };
            require(worker.pass());
            require(store.snapshot(typedOwner).records.size() == 2);
            require(!worker.pass());
            require(store.accept(typedOwner, sinkRequest(5, 4, DeviceIntentAction::Remove)) == 5);
            require(!worker.pass());
            const auto remaining = store.snapshot(typedOwner);
            require(remaining.records.size() == 1 &&
                    remaining.records[0].kind == DeviceIntentKind::MicrophoneMix &&
                    remaining.records[0].mixId == namedMix.mixId);
            SetEvent(stop);
            const auto stoppedCalls = adapter.calls;
            require(peer.accept(other, request(3, 2)) == 3);
            require(!worker.pass() && adapter.calls == stoppedCalls);
        }
        std::filesystem::remove_all(root);
        std::cout << "PASS: shared lock, latest-state coalescing, undo during removal, retries, "
                     "reboot tombstones, owner isolation and stop\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Fixture retained: " << root << L'\n';
        return 1;
    }
}
