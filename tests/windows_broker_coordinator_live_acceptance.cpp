// Opt-in adjunct to the service fixture. Its parent owns an initially absent
// machine store and one unique test mix; no real user's document is submitted.
#include "broker_coordinator.hpp"
#include "broker_client.hpp"
#include "driver_identity.hpp"
#include "endpoint_roles.hpp"
#include <array>
#include <thread>
#include <stdexcept>
#include <iostream>
namespace loopz::windows {
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
struct Impersonation {
    explicit Impersonation(HANDLE token) {
        require(ImpersonateLoggedOnUser(token), "Cannot enter restricted worker context");
    }
    ~Impersonation() {
        if (!RevertToSelf())
            std::terminate();
    }
};
std::map<std::string, EndpointRole> ids(std::wstring_view owner, std::string_view id) {
    std::map<std::string, EndpointRole> result;
    for (const auto& [uid, endpoint] : loopzEndpoints())
        if (endpoint.mixKey == driverMixKey(owner, id))
            result.emplace(uid, endpoint.role);
    return result;
}
template <class Predicate>
BrokerCoordinatorState await(BrokerCoordinator& coordinator, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    for (;;) {
        const auto state = coordinator.state();
        if (predicate(state))
            return state;
        if (!state.error.empty())
            throw std::runtime_error(state.error);
        require(std::chrono::steady_clock::now() < deadline, "Live coordinator timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
} // namespace
void runLiveCoordinatorAcceptance(const std::string& mixId) {
    Handle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(cancel.value != nullptr, "Cannot query coordinator fixture baseline");
    const auto owner = currentOwnerSid();
    require(readDeviceBrokerSnapshot(cancel.value).records.empty() && ids(owner, mixId).empty(),
            "Live coordinator requires its parent's empty test store and absent mix");
    Handle primary, restricted;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &primary.value),
            "Cannot read fixture token");
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD size = static_cast<DWORD>(administrators.size());
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size),
            "Cannot identify administrator group");
    SID_AND_ATTRIBUTES disabled{administrators.data(), 0};
    require(CreateRestrictedToken(primary.value, DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0,
                                  nullptr, &restricted.value),
            "Cannot restrict coordinator worker transport");
    BrokerCoordinator coordinator({[&](HANDLE stop) {
                                       Impersonation identity(restricted.value);
                                       return readDeviceBrokerSnapshot(stop);
                                   },
                                   [&](std::string_view bytes, HANDLE stop) {
                                       Impersonation identity(restricted.value);
                                       return callDeviceBroker(bytes, stop);
                                   }});
    RouteDocument saved;
    VirtualDeviceDefinition device;
    device.id = mixId;
    device.name = "Coordinator live acceptance";
    saved.devices.push_back(device);
    coordinator.submit(saved, true, 100);
    await(coordinator, [&](const auto& state) {
        return state.generation == 100 && state.accepted && state.devices.contains(mixId) &&
               state.devices.at(mixId).phase == "ready";
    });
    const auto initial = ids(owner, mixId);
    require(initial.size() == 2, "Coordinator did not create both public endpoints");
    saved.devices.front().name = "Coordinator intermediate";
    saved.devices.front().sampleRate = 96000;
    saved.devices.front().routing.outputChannels = 8;
    coordinator.submit(saved, true, 101);
    saved.devices.front().name = "Coordinator latest";
    coordinator.submit(saved, true, 102);
    await(coordinator, [&](const auto& state) {
        return state.generation == 102 && state.accepted && state.devices.contains(mixId) &&
               state.devices.at(mixId).phase == "ready";
    });
    const auto stored = readDeviceBrokerSnapshot(cancel.value);
    require(stored.records.size() == 1 && stored.records.front().mixId == mixId &&
                stored.records.front().name == "Coordinator latest" &&
                stored.records.front().sampleRate == 96000 && stored.records.front().channels == 8,
            "Latest worker edit not durable");
    require(ids(owner, mixId) == initial, "Background format edit changed endpoint IDs");
    coordinator.submit({}, true, 103);
    await(coordinator, [](const auto& state) {
        return state.generation == 103 && state.accepted && state.devices.empty();
    });
    require(readDeviceBrokerSnapshot(cancel.value).records.empty() && ids(owner, mixId).empty(),
            "Coordinator did not complete explicit removal");
    std::cout << "PASS: restricted background worker creates, coalesces format/name edits and "
                 "removes through actual service\n";
}
} // namespace loopz::windows
