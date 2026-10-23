#include "broker_coordinator.hpp"
#include <mutex>
#include <algorithm>
#include <thread>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace loopz;
using namespace loopz::windows;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Event {
    HANDLE value{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Event() {
        require(value != nullptr, "Cannot create fixture event");
    }
    ~Event() {
        CloseHandle(value);
    }
};
RouteDocument document(std::string name = "First") {
    RouteDocument result;
    VirtualDeviceDefinition device;
    device.id = "coordinator-test";
    device.name = std::move(name);
    result.devices.push_back(std::move(device));
    return result;
}
struct Model {
    std::mutex mutex;
    DeviceIntentSnapshot snapshot;
    std::vector<DeviceIntentRequest> requests;
    bool loseFirst{}, blockFirst{}, blockReads{}, allowSinkStatus{};
    Event committed, readStarted;
    DeviceIntentSnapshot read(HANDLE cancel) {
        SetEvent(readStarted.value);
        if (blockReads) {
            require(WaitForSingleObject(cancel, 10000) == WAIT_OBJECT_0,
                    "Fixture read was not cancelled");
            throw std::runtime_error("Cancelled fixture read");
        }
        std::lock_guard lock(mutex);
        return snapshot;
    }
    std::string call(std::string_view bytes, HANDLE cancel) {
        const auto parsed = JSON::parse(bytes);
        if (parsed.at("action") == "status") {
            std::lock_guard lock(mutex);
            const bool sink = parsed.at("version") == 2U;
            require(!sink || (allowSinkStatus && parsed.size() == 3 &&
                              parsed.at("kind") == "appRoutingSink"),
                    "Unexpected routing sink status query");
            const auto kind =
                sink ? DeviceIntentKind::AppRoutingSink : DeviceIntentKind::MicrophoneMix;
            const auto id = sink ? std::string{} : parsed.at("mixId").get<std::string>();
            const auto found = std::find_if(snapshot.records.begin(), snapshot.records.end(),
                                            [&](const auto& row) {
                                                return row.kind == kind && row.mixId == id;
                                            });
            require(found != snapshot.records.end(), "Unexpected status identity");
            JSON device{{"phase", found->action == DeviceIntentAction::Remove ? "queued" : "ready"},
                        {"revision", found->expectedRevision + 1}};
            if (sink)
                device["kind"] = "appRoutingSink";
            else
                device["id"] = found->mixId;
            return JSON{
                {"ok", true}, {"revision", snapshot.revision}, {"device", std::move(device)}}
                .dump();
        }
        auto request = decodeDeviceIntent(bytes);
        bool first{};
        uint64_t revision{};
        {
            std::lock_guard lock(mutex);
            require(request.expectedRevision == snapshot.revision, "Coordinator sent stale CAS");
            first = requests.empty();
            requests.push_back(request);
            const auto found = std::find_if(
                snapshot.records.begin(), snapshot.records.end(), [&](const auto& row) {
                    return row.kind == request.kind && row.mixId == request.mixId;
                });
            if (found == snapshot.records.end())
                snapshot.records.push_back(request);
            else
                *found = request;
            revision = ++snapshot.revision;
        }
        if (first) {
            SetEvent(committed.value);
            if (blockFirst)
                require(WaitForSingleObject(cancel, 5000) == WAIT_OBJECT_0,
                        "New edit did not cancel old operation");
            if (loseFirst || blockFirst)
                throw std::runtime_error("Acceptance reply lost after commit");
        }
        return JSON{{"ok", true}, {"acceptedRevision", revision}}.dump();
    }
    BrokerCoordinatorTransport transport() {
        return {[this](HANDLE cancel) {
                    return read(cancel);
                },
                [this](std::string_view bytes, HANDLE cancel) {
                    return call(bytes, cancel);
                }};
    }
};
template <class Predicate>
BrokerCoordinatorState await(BrokerCoordinator& worker, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    for (;;) {
        auto state = worker.state();
        if (predicate(state))
            return state;
        require(std::chrono::steady_clock::now() < deadline, "Coordinator fixture timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace
int main() {
    for (const auto action : {DeviceIntentAction::Upsert, DeviceIntentAction::Remove}) {
        Model model;
        DeviceIntentRequest sink;
        sink.operationId = "00000000-0000-0000-0000-000000000001";
        sink.kind = DeviceIntentKind::AppRoutingSink;
        sink.action = action;
        model.snapshot.records.push_back(sink);
        model.snapshot.revision = 1;
        BrokerCoordinator worker(model.transport());
        worker.submit(document(), true, 30);
        const auto ready = await(worker, [](const auto& row) {
            return row.generation == 30 && row.examined;
        });
        require(ready.accepted && ready.error.empty() && ready.devices.size() == 1 &&
                    ready.devices.at("coordinator-test").phase == "ready",
                "Routing sink interfered with saved-mix readiness");
        worker.submit({}, true, 31);
        const auto removed = await(worker, [](const auto& row) {
            return row.generation == 31 && row.examined;
        });
        require(removed.accepted && removed.error.empty() && removed.devices.size() == 1 &&
                    removed.devices.at("coordinator-test").removing,
                "Routing sink interfered with explicit saved-mix removal");
        std::lock_guard lock(model.mutex);
        require(model.requests.size() == 2 && model.requests[0].expectedRevision == 1 &&
                    model.requests[1].expectedRevision == 2 &&
                    model.snapshot.records[0].kind == DeviceIntentKind::AppRoutingSink &&
                    model.snapshot.records[0].action == action &&
                    model.snapshot.records[0].expectedRevision == 0,
                "Saved-mix edits changed sink intent or lost shared revision ordering");
    }
    {
        Model model;
        model.blockFirst = true;
        BrokerCoordinator worker(model.transport());
        worker.submit(document(), true, 1);
        require(WaitForSingleObject(model.committed.value, 2000) == WAIT_OBJECT_0,
                "First operation not committed");
        worker.submit(document("Latest"), true, 2);
        const auto state = await(worker, [](const auto& row) {
            return row.generation == 2 && row.accepted;
        });
        require(state.error.empty() && state.devices.at("coordinator-test").phase == "ready",
                "Latest state not ready");
        std::lock_guard lock(model.mutex);
        require(model.requests.size() == 2 &&
                    model.requests[0].operationId != model.requests[1].operationId &&
                    model.requests[1].expectedRevision == 1 &&
                    model.snapshot.records[0].name == "Latest",
                "Superseded/lost reply recovery failed");
    }
    {
        Model model;
        model.loseFirst = true;
        BrokerCoordinator worker(model.transport());
        worker.submit(document(), true, 10);
        require(WaitForSingleObject(model.committed.value, 2000) == WAIT_OBJECT_0,
                "Lost-reply operation not committed");
        worker.submit(document(), true, 11);
        await(worker, [](const auto& row) {
            return row.generation == 11 && row.accepted;
        });
        {
            std::lock_guard lock(model.mutex);
            require(model.requests.size() == 1, "Accepted operation duplicated after lost reply");
        }
        worker.submit({}, false, 12);
        await(worker, [](const auto& row) {
            return row.generation == 12 && row.examined && !row.accepted;
        });
        {
            std::lock_guard lock(model.mutex);
            require(model.requests.size() == 1, "Unauthoritative document changed installed state");
        }
        worker.submit({}, true, 13);
        const auto state = await(worker, [](const auto& row) {
            return row.generation == 13 && row.accepted;
        });
        require(state.devices.at("coordinator-test").removing &&
                    state.devices.at("coordinator-test").phase == "queued",
                "Pending removal disappeared from status");
        std::lock_guard lock(model.mutex);
        require(model.requests.size() == 2 &&
                    model.requests.back().action == DeviceIntentAction::Remove,
                "Missing document mutated state or explicit empty document did not remove");
    }
    {
        Model model;
        model.blockReads = true;
        auto worker = std::make_unique<BrokerCoordinator>(model.transport());
        worker->submit(document(), true, 20);
        require(WaitForSingleObject(model.readStarted.value, 2000) == WAIT_OBJECT_0,
                "Read did not start");
        auto stopped = std::async(std::launch::async, [&] {
            worker.reset();
        });
        require(stopped.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "Stop did not cancel and join I/O");
        stopped.get();
    }
    {
        Model model;
        model.allowSinkStatus = true;
        BrokerCoordinator worker(model.transport());
        worker.submit(document(), true, 40, AppRoutingSinkDemand::Required);
        const auto ready = await(worker, [](const auto& row) {
            return row.generation == 40 && row.accepted;
        });
        require(ready.devices.size() == 1 && ready.routingSink &&
                    ready.routingSink->phase == "ready" && !ready.routingSink->removing,
                "Combined mix/sink intent did not converge");
        worker.submit({}, false, 41, AppRoutingSinkDemand::Required);
        await(worker, [](const auto& row) {
            return row.generation == 41 && row.accepted;
        });
        bool invalid = false;
        try {
            worker.submit({}, false, 42, AppRoutingSinkDemand::Released);
        } catch (const std::invalid_argument&) {
            invalid = true;
        }
        require(invalid, "Missing desired document authorized sink release");
        worker.submit({}, true, 43);
        const auto unknown = await(worker, [](const auto& row) {
            return row.generation == 43 && row.accepted;
        });
        require(!unknown.routingSink, "Unknown lease state queried sink ownership");
        worker.submit({}, true, 44, AppRoutingSinkDemand::Released);
        const auto removed = await(worker, [](const auto& row) {
            return row.generation == 44 && row.accepted;
        });
        require(removed.routingSink && removed.routingSink->removing &&
                    removed.routingSink->phase == "queued",
                "Explicit routing sink removal was not reported");
        std::lock_guard lock(model.mutex);
        require(model.requests.size() == 4 &&
                    model.requests[0].kind == DeviceIntentKind::MicrophoneMix &&
                    model.requests[1].kind == DeviceIntentKind::AppRoutingSink &&
                    model.requests[2].kind == DeviceIntentKind::MicrophoneMix &&
                    model.requests[2].action == DeviceIntentAction::Remove &&
                    model.requests[3].kind == DeviceIntentKind::AppRoutingSink &&
                    model.requests[3].action == DeviceIntentAction::Remove,
                "Sink demand interfered with saved mixes or unknown demand removed the sink");
        for (size_t i = 0; i < model.requests.size(); ++i)
            require(model.requests[i].expectedRevision == i,
                    "Shared mix/sink CAS revision ordering lost");
    }
    {
        Model model;
        model.allowSinkStatus = true;
        model.loseFirst = true;
        BrokerCoordinator worker(model.transport());
        worker.submit({}, false, 50, AppRoutingSinkDemand::Required);
        require(WaitForSingleObject(model.committed.value, 2000) == WAIT_OBJECT_0,
                "Sink request did not commit");
        worker.submit({}, false, 51, AppRoutingSinkDemand::Required);
        const auto recovered = await(worker, [](const auto& row) {
            return row.generation == 51 && row.accepted;
        });
        require(recovered.devices.empty() && recovered.routingSink &&
                    recovered.routingSink->phase == "ready",
                "Receipt-only recovery did not retain sink without a saved document");
        std::lock_guard lock(model.mutex);
        require(model.requests.size() == 1 &&
                    model.requests[0].kind == DeviceIntentKind::AppRoutingSink,
                "Sink lost reply duplicated acceptance or changed a microphone");
    }
    {
        Model model;
        model.allowSinkStatus = true;
        model.blockFirst = true;
        BrokerCoordinator worker(model.transport());
        worker.submit({}, false, 60, AppRoutingSinkDemand::Required);
        require(WaitForSingleObject(model.committed.value, 2000) == WAIT_OBJECT_0,
                "Blocked sink request did not commit");
        worker.submit({}, true, 61, AppRoutingSinkDemand::Released);
        const auto latest = await(worker, [](const auto& row) {
            return row.generation == 61 && row.accepted;
        });
        require(latest.routingSink && latest.routingSink->removing,
                "New release demand did not supersede blocked sink create");
        std::lock_guard lock(model.mutex);
        require(model.requests.size() == 2 &&
                    model.requests.back().action == DeviceIntentAction::Remove &&
                    model.requests.back().expectedRevision == 1,
                "Cancelled sink intent lost durable state/CAS");
    }
    std::cout << "PASS: asynchronous coordinator, generation guards, lost replies, explicit "
                 "removal and cancellation\n";
}
