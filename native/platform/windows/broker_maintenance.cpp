#include "broker_maintenance.hpp"
#include "broker_installation.hpp"
#include "broker_deploy.hpp"
#include "device_intent_store.hpp"
#include "device_operations.hpp"
#include "driver_identity.hpp"
#include <windows.h>
#include <devguid.h>
#include <sddl.h>
#include <array>
#include <vector>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <utility>

namespace loopz::windows {
namespace {
struct Service {
    SC_HANDLE value{};
    explicit Service(SC_HANDLE handle) : value(handle) {}
    ~Service() {
        if (value)
            CloseServiceHandle(value);
    }
    Service(Service&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
};
void check(bool result, const char* message) {
    if (!result)
        throw std::runtime_error(std::string(message) + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
}
class MaintenanceLock {
    HANDLE value_{};

  public:
    MaintenanceLock() {
        PSECURITY_DESCRIPTOR descriptor{};
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                  L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr),
              "Cannot prepare service maintenance lock");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        value_ = CreateMutexW(&attributes, FALSE, L"Global\\LoopZBrokerMaintenance");
        LocalFree(descriptor);
        check(value_ != nullptr, "Cannot open service maintenance lock");
        const auto waited = WaitForSingleObject(value_, 0);
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
            CloseHandle(value_);
            value_ = nullptr;
            throw std::runtime_error("Another service maintenance operation is running");
        }
    }
    ~MaintenanceLock() {
        if (value_) {
            ReleaseMutex(value_);
            CloseHandle(value_);
        }
    }
    MaintenanceLock(const MaintenanceLock&) = delete;
    MaintenanceLock& operator=(const MaintenanceLock&) = delete;
};
std::wstring command() {
    return L"\"" + (brokerServiceDirectory() / L"LoopZDeviceBroker.exe").wstring() + L"\"";
}
void validateRegistration(SC_HANDLE service) {
    DWORD bytes{};
    QueryServiceConfigW(service, nullptr, 0, &bytes);
    check(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes >= sizeof(QUERY_SERVICE_CONFIGW) &&
              bytes <= 65536,
          "Cannot inspect setup service registration");
    std::vector<BYTE> storage(bytes);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
    check(QueryServiceConfigW(service, config, bytes, &bytes),
          "Cannot read setup service registration");
    if (config->dwServiceType != SERVICE_WIN32_OWN_PROCESS || !config->lpBinaryPathName ||
        _wcsicmp(config->lpBinaryPathName, command().c_str()) != 0 || !config->lpServiceStartName ||
        _wcsicmp(config->lpServiceStartName, L"LocalSystem") != 0)
        throw std::runtime_error("Existing setup service has an unexpected executable or identity");
}
SERVICE_STATUS_PROCESS status(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS result{};
    DWORD bytes{};
    check(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&result),
                               sizeof(result), &bytes),
          "Cannot query setup service");
    return result;
}
void pauseUntil(std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Microphone setup service transition timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
void stop(SC_HANDLE service) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto current = status(service);
    while (current.dwCurrentState == SERVICE_START_PENDING) {
        pauseUntil(deadline);
        current = status(service);
    }
    if (current.dwCurrentState == SERVICE_STOPPED)
        return;
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, current.dwProcessId);
    struct Process {
        HANDLE value;
        ~Process() {
            if (value)
                CloseHandle(value);
        }
    } held{process};
    check(process != nullptr, "Cannot retain setup service process during stop");
    if (current.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS ignored{};
        check(ControlService(service, SERVICE_CONTROL_STOP, &ignored), "Cannot stop setup service");
    }
    while (status(service).dwCurrentState != SERVICE_STOPPED)
        pauseUntil(deadline);
    check(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0,
          "Stopped setup service has not exited");
}
void start(SC_HANDLE service) {
    check(StartServiceW(service, 0, nullptr), "Cannot start setup service");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        const auto current = status(service);
        if (current.dwCurrentState == SERVICE_RUNNING)
            return;
        if (current.dwCurrentState == SERVICE_STOPPED)
            throw std::runtime_error(
                "Microphone setup service stopped during startup (service error " +
                std::to_string(current.dwServiceSpecificExitCode) + ")");
        pauseUntil(deadline);
    }
}
Service manager(DWORD access) {
    const auto result = OpenSCManagerW(nullptr, nullptr, access);
    check(result != nullptr, "Cannot open setup service manager");
    return Service(result);
}
constexpr DWORD maintenanceAccess = SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START |
                                    SERVICE_STOP | SERVICE_CHANGE_CONFIG | WRITE_DAC | DELETE;
Service existing(SC_HANDLE managerHandle) {
    Service result(OpenServiceW(managerHandle, kDeviceBrokerServiceName, maintenanceAccess));
    if (!result.value && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        check(false, "Cannot open installed setup service");
    if (result.value)
        validateRegistration(result.value);
    return result;
}
constexpr wchar_t dependencies[] = L"AudioEndpointBuilder\0Audiosrv\0";
// Called only by elevated maintenance, with the broker stopped and setup lock
// held. The protected store supplies owners and desired mixes; no user path or
// IPC payload can authorize a package change. Routine reconciliation stays strict.
BrokerMaintenanceResult migrateExistingDevices(std::wstring_view publishedInf) {
    DeviceIntentStore store;
    struct Devices {
        HDEVINFO value{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr)};
        ~Devices() {
            if (value != INVALID_HANDLE_VALUE)
                SetupDiDestroyDeviceInfoList(value);
        }
    } devices;
    check(devices.value != INVALID_HANDLE_VALUE, "Cannot open maintenance device set");
    BrokerMaintenanceResult result;
    for (const auto& owner : store.ownersWithDesiredState()) {
        for (const auto& desired : store.snapshot(owner).records) {
            if (desired.action != DeviceIntentAction::Upsert)
                continue;
            SP_DEVINFO_DATA device{sizeof(device)};
            const auto instance = driverInstance(desired.kind == DeviceIntentKind::AppRoutingSink
                                                     ? driverAppRoutingSinkKey(owner)
                                                     : driverMixKey(owner, desired.mixId));
            if (!SetupDiOpenDeviceInfoW(devices.value, instance.c_str(), nullptr, 0, &device)) {
                if (GetLastError() == ERROR_NO_SUCH_DEVINST)
                    continue; // Startup reconciliation creates missing desired devices.
                check(false, "Cannot open microphone for maintenance");
            }
            const auto installed = installedDevicePackage(devices.value, device);
            if (_wcsicmp(installed.c_str(), std::wstring(publishedInf).c_str()) == 0)
                continue;
            if (desired.kind == DeviceIntentKind::AppRoutingSink) {
                const auto migrated =
                    upsertAppRoutingSink(devices.value, owner, publishedInf, true);
                ++result.migratedDevices;
                result.rebootRequired = result.rebootRequired || migrated.rebootRequired;
                if (!migrated.rebootRequired)
                    finalizeAppRoutingSink(devices.value, owner);
                continue;
            }
            VirtualDeviceDefinition mix;
            mix.id = desired.mixId;
            mix.name = desired.name;
            mix.sampleRate = desired.sampleRate;
            mix.routing.outputChannels = desired.channels;
            // upsert verifies LoopZ hardware and the protected matching owner
            // before any mutation. Never remove/recreate a root during migration.
            const auto migrated = upsertMixDevice(devices.value, owner, mix, publishedInf, true);
            ++result.migratedDevices;
            result.rebootRequired = result.rebootRequired || migrated.rebootRequired;
            if (!migrated.rebootRequired)
                finalizeMixDevice(devices.value, owner, mix);
        }
    }
    return result;
}
} // namespace
BrokerMaintenanceResult installOrRepairBrokerService(std::wstring_view publishedInf) {
    validateBrokerPublishedInf(publishedInf);
    MaintenanceLock maintenance;
    auto scm = manager(SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    auto service = existing(scm.value);
    if (service.value)
        stop(service.value);
    BrokerMaintenanceResult result;
    {
        DeviceSetupLock lock;
        // Publication preserves an existing binary on copy/hash/replace failure.
        stageEmbeddedBroker();
        provisionDeviceIntentStore(); // Existing unsafe or replaced directories fail closed.
        result = migrateExistingDevices(publishedInf);
        if (!service.value) {
            service.value = CreateServiceW(
                scm.value, kDeviceBrokerServiceName, L"LoopZ microphone setup", maintenanceAccess,
                SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                command().c_str(), nullptr, nullptr, dependencies, L"LocalSystem", nullptr);
            check(service.value != nullptr, "Cannot register setup service");
        } else {
            check(ChangeServiceConfigW(service.value, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                       SERVICE_ERROR_NORMAL, command().c_str(), nullptr, nullptr,
                                       dependencies, L"LocalSystem", nullptr,
                                       L"LoopZ microphone setup"),
                  "Cannot repair setup service configuration");
        }
        provisionBrokerServiceAccess(service.value);
        provisionBrokerInstallation(publishedInf);
        std::array<SC_ACTION, 3> actions{
            {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 15000}, {SC_ACTION_RESTART, 60000}}};
        SERVICE_FAILURE_ACTIONSW recovery{};
        recovery.dwResetPeriod = 86400;
        recovery.cActions = static_cast<DWORD>(actions.size());
        recovery.lpsaActions = actions.data();
        check(ChangeServiceConfig2W(service.value, SERVICE_CONFIG_FAILURE_ACTIONS, &recovery),
              "Cannot configure setup service recovery");
        SERVICE_FAILURE_ACTIONS_FLAG failures{TRUE};
        check(ChangeServiceConfig2W(service.value, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failures),
              "Cannot configure setup service error recovery");
    } // Release the PnP lock before startup recovery can apply desired state.
    start(service.value);
    return result;
}
void stopInstalledBrokerService() {
    MaintenanceLock maintenance;
    auto scm = manager(SC_MANAGER_CONNECT);
    auto service = existing(scm.value);
    if (service.value)
        stop(service.value);
}
void unregisterBrokerService() {
    MaintenanceLock maintenance;
    auto scm = manager(SC_MANAGER_CONNECT);
    {
        auto service = existing(scm.value);
        if (!service.value)
            return;
        stop(service.value);
        check(DeleteService(service.value), "Cannot unregister setup service");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        Service pending(OpenServiceW(scm.value, kDeviceBrokerServiceName, SERVICE_QUERY_STATUS));
        if (!pending.value && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            return;
        if (!pending.value && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
            check(false, "Cannot confirm setup service removal");
        pauseUntil(deadline);
    }
}
} // namespace loopz::windows
