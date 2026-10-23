#include "broker_coordinator.hpp"
#include "broker_client.hpp"
#include <objbase.h>
#include <array>
#include <algorithm>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace loopz::windows {
namespace {
struct Event {
    HANDLE value;
    explicit Event(bool manual) : value(CreateEventW(nullptr, manual, FALSE, nullptr)) {
        if (!value)
            throw std::runtime_error("Cannot prepare microphone setup coordination");
    }
    ~Event() {
        CloseHandle(value);
    }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};
std::string operationId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("Cannot identify microphone setup operation");
    std::array<wchar_t, 40> text{};
    if (!StringFromGUID2(guid, text.data(), static_cast<int>(text.size())))
        throw std::runtime_error("Cannot encode microphone setup operation");
    std::string result;
    for (size_t i = 1; i < 37; ++i) {
        auto c = static_cast<char>(text[i]);
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
        result += c;
    }
    return result;
}
} // namespace
struct BrokerCoordinator::Impl {
    struct Work {
        RouteDocument document;
        bool authoritative;
        uint64_t generation;
        AppRoutingSinkDemand sinkDemand;
    };
    Event stop{true}, changed{false}, cancel{true};
    BrokerCoordinatorTransport transport;
    mutable std::mutex mutex;
    std::shared_ptr<const Work> current;
    BrokerCoordinatorState status;
    std::jthread thread;
    explicit Impl(BrokerCoordinatorTransport value) : transport(std::move(value)) {
        if (!transport.read && !transport.call) {
            transport.read = readDeviceBrokerSnapshot;
            transport.call = callDeviceBroker;
        }
        if (!transport.read || !transport.call)
            throw std::invalid_argument("Incomplete setup transport");
        thread = std::jthread([this] {
            run();
        });
    }
    ~Impl() {
        SetEvent(stop.value);
        SetEvent(cancel.value);
        SetEvent(changed.value);
        if (thread.joinable())
            thread.join();
    }
    bool superseded(const std::shared_ptr<const Work>& work) const {
        std::lock_guard lock(mutex);
        return current != work || WaitForSingleObject(stop.value, 0) == WAIT_OBJECT_0;
    }
    void publish(const std::shared_ptr<const Work>& work, BrokerCoordinatorState value) {
        std::lock_guard lock(mutex);
        if (current == work)
            status = std::move(value);
    }
    void run() noexcept {
        const std::array<HANDLE, 2> events{stop.value, changed.value};
        for (;;) {
            if (WaitForSingleObject(stop.value, 0) == WAIT_OBJECT_0)
                return;
            std::shared_ptr<const Work> work;
            {
                std::lock_guard lock(mutex);
                work = current;
                // A submission cannot be lost between capturing work and resetting events.
                ResetEvent(cancel.value);
                ResetEvent(changed.value);
            }
            DWORD delay = INFINITE;
            if (work &&
                (work->authoritative || work->sinkDemand == AppRoutingSinkDemand::Required)) {
                delay = 2000;
                BrokerCoordinatorState observed;
                observed.generation = work->generation;
                observed.examined = true;
                try {
                    bool converged = false;
                    for (unsigned attempt = 0; attempt < 512; ++attempt) {
                        if (superseded(work))
                            break;
                        const auto snapshot = transport.read(cancel.value);
                        if (superseded(work))
                            break;
                        auto request = nextSavedDeviceIntent(work->document, work->authoritative,
                                                             snapshot, operationId());
                        if (!request)
                            request =
                                nextAppRoutingSinkIntent(work->sinkDemand, snapshot, operationId());
                        if (request) {
                            if (superseded(work))
                                break;
                            const auto reply = JSON::parse(
                                transport.call(encodeDeviceIntent(*request).dump(), cancel.value));
                            if (!reply.value("ok", false))
                                throw std::runtime_error("Microphone setup edit was not accepted");
                            if (!reply.contains("acceptedRevision") ||
                                !reply.at("acceptedRevision").is_number_unsigned() ||
                                reply.at("acceptedRevision").get<uint64_t>() !=
                                    snapshot.revision + 1)
                                throw std::runtime_error(
                                    "Unexpected microphone setup acceptance revision");
                            // Always re-read durable state, including after a lost reply/retry.
                            continue;
                        }
                        for (const auto& desired : snapshot.records) {
                            const bool sink = desired.kind == DeviceIntentKind::AppRoutingSink;
                            // Legacy/default submissions must neither mutate nor query leases.
                            if (sink && work->sinkDemand == AppRoutingSinkDemand::Unknown)
                                continue;
                            if (superseded(work))
                                break;
                            const JSON query = sink ? JSON{{"version", 2U},
                                                           {"action", "status"},
                                                           {"kind", "appRoutingSink"}}
                                                    : JSON{{"version", 1},
                                                           {"action", "status"},
                                                           {"mixId", desired.mixId}};
                            const auto reply =
                                JSON::parse(transport.call(query.dump(), cancel.value));
                            if (!reply.value("ok", false) ||
                                reply.at("revision") != snapshot.revision)
                                throw std::runtime_error(
                                    "Microphone setup changed during status refresh");
                            const auto& device = reply.at("device");
                            if ((sink ? device.value("kind", std::string{}) != "appRoutingSink"
                                      : device.at("id") != desired.mixId) ||
                                !device.at("phase").is_string())
                                throw std::runtime_error(
                                    "Unexpected microphone setup device status");
                            if (device.at("phase") == "unrequested" &&
                                desired.action == DeviceIntentAction::Remove)
                                continue;
                            if (device.at("revision") != desired.expectedRevision + 1)
                                throw std::runtime_error("Stale microphone setup device status");
                            const auto phase = device.at("phase").get<std::string>();
                            if (phase != "queued" && phase != "applying" && phase != "ready" &&
                                phase != "error" && phase != "reboot_required")
                                throw std::runtime_error("Unknown microphone setup device phase");
                            auto error = device.value("error", std::string{});
                            if (error.size() > 512)
                                throw std::runtime_error(
                                    "Microphone setup diagnostic exceeds its limit");
                            BrokerDeviceState state{phase, std::move(error),
                                                    desired.expectedRevision + 1,
                                                    desired.action == DeviceIntentAction::Remove};
                            if (sink)
                                observed.routingSink = std::move(state);
                            else
                                observed.devices.emplace(desired.mixId, std::move(state));
                        }
                        if (superseded(work))
                            break;
                        observed.accepted = true;
                        publish(work, std::move(observed));
                        converged = true;
                        break;
                    }
                    if (!converged && !superseded(work))
                        throw std::runtime_error("Microphone setup did not converge");
                } catch (const std::exception& error) {
                    observed.accepted = false;
                    observed.error = std::string(error.what()).substr(0, 512);
                    publish(work, std::move(observed));
                    delay = 5000;
                } catch (...) {
                    observed.accepted = false;
                    observed.error = "Unexpected microphone setup failure";
                    publish(work, std::move(observed));
                    delay = 5000;
                }
            }
            if (work && !work->authoritative &&
                work->sinkDemand != AppRoutingSinkDemand::Required) {
                BrokerCoordinatorState paused;
                paused.generation = work->generation;
                paused.examined = true;
                publish(work, std::move(paused));
            }
            if (work && superseded(work))
                continue;
            const auto waited = WaitForMultipleObjects(static_cast<DWORD>(events.size()),
                                                       events.data(), FALSE, delay);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
                return;
        }
    }
};
BrokerCoordinator::BrokerCoordinator(BrokerCoordinatorTransport transport)
    : impl_(std::make_unique<Impl>(std::move(transport))) {}
BrokerCoordinator::~BrokerCoordinator() = default;
void BrokerCoordinator::submit(const RouteDocument& saved, bool authoritative, uint64_t generation,
                               AppRoutingSinkDemand demand) {
    if ((demand != AppRoutingSinkDemand::Unknown && demand != AppRoutingSinkDemand::Required &&
         demand != AppRoutingSinkDemand::Released) ||
        (!authoritative && demand == AppRoutingSinkDemand::Released))
        throw std::invalid_argument(
            "Sink release requires authoritative desired state and examined recovery");
    auto work = std::make_shared<Impl::Work>(Impl::Work{saved, authoritative, generation, demand});
    std::lock_guard lock(impl_->mutex);
    impl_->current = std::move(work);
    impl_->status = {generation, false, false, {}, {}, {}};
    SetEvent(impl_->cancel.value);
    SetEvent(impl_->changed.value);
}
BrokerCoordinatorState BrokerCoordinator::state() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->status;
}
} // namespace loopz::windows
