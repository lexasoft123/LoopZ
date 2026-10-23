#include "broker_cleanup.hpp"
#include "broker_binary.hpp"
#include "broker_installation.hpp"
#include "broker_security.hpp"
#include "device_intent_store.hpp"
#include "device_operations.hpp"
#include "driver_identity.hpp"
#include <devguid.h>
#include <sddl.h>
#include <objbase.h>
#include <vector>
#include <memory>
#include <utility>
#include <algorithm>

namespace loopz::windows {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    std::filesystem::path path;
    explicit Handle(HANDLE handle, std::filesystem::path location = {})
        : value(handle), path(std::move(location)) {}
    Handle(Handle&& other) noexcept
        : value(std::exchange(other.value, INVALID_HANDLE_VALUE)), path(std::move(other.path)) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Plan {
    Handle directory;
    std::vector<Handle> files;
    explicit Plan(HANDLE handle, const std::filesystem::path& path) : directory(handle, path) {}
};
bool payloadName(std::wstring_view name) {
    if (_wcsicmp(std::wstring(name).c_str(), L"LoopZDeviceBroker.exe") == 0)
        return true;
    constexpr std::wstring_view prefix = L"LoopZDeviceBroker.pending-";
    if (!name.starts_with(prefix))
        return false;
    const std::wstring suffix(name.substr(prefix.size()));
    if (suffix.size() != 38 || suffix.front() != L'{' || suffix.back() != L'}')
        return false;
    GUID guid{};
    if (FAILED(CLSIDFromString(suffix.c_str(), &guid)))
        return false;
    wchar_t canonical[40]{};
    return StringFromGUID2(guid, canonical, 40) && _wcsicmp(canonical, suffix.c_str()) == 0;
}
std::unique_ptr<Plan> inspect(const std::filesystem::path& path, bool store) {
    const auto directory = CreateFileW(
        path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        brokerProtectedObjectFailure("Open service cleanup directory");
    }
    auto plan = std::make_unique<Plan>(directory, path);
    validateBrokerProtectedObject(directory, true);
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        const auto name = entry.path().filename().wstring();
        if (store ? name != L"setup.sqlite3" : !payloadName(name))
            throw std::runtime_error(
                "Unexpected file in service cleanup directory; no files removed");
        Handle file(CreateFileW(entry.path().c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr),
                    entry.path());
        if (file.value == INVALID_HANDLE_VALUE)
            brokerProtectedObjectFailure("Inspect service cleanup file");
        validateBrokerProtectedObject(file.value, false);
        plan->files.push_back(std::move(file));
    }
    return plan;
}
void prepareDeletion(Handle& file, bool directory) {
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!GetFileInformationByHandle(file.value, &before))
        brokerProtectedObjectFailure("Retain service cleanup file identity");
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;
    file.value = CreateFileW(
        file.path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
    if (file.value == INVALID_HANDLE_VALUE)
        brokerProtectedObjectFailure("Lock validated cleanup object for deletion");
    validateBrokerProtectedObject(file.value, directory);
    if (!GetFileInformationByHandle(file.value, &after) ||
        before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
        before.nFileIndexHigh != after.nFileIndexHigh ||
        before.nFileIndexLow != after.nFileIndexLow)
        throw std::runtime_error("Service cleanup object changed during preflight");
}
void prepareDeletion(Plan& plan) {
    for (auto& file : plan.files)
        prepareDeletion(file, false);
    prepareDeletion(plan.directory, true);
}
void erase(Plan& plan) {
    for (auto& file : plan.files) {
        validateBrokerProtectedObject(file.value, false);
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition,
                                        sizeof(disposition)))
            brokerProtectedObjectFailure("Delete validated service file");
    }
    plan.files.clear();
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(plan.directory.value, FileDispositionInfo, &disposition,
                                    sizeof(disposition)))
        brokerProtectedObjectFailure("Delete validated service directory");
}
void purgeBrokerInstallationImpl(std::span<const std::wstring> removedInstances) {
    // Same maintenance serialization used by install/stop/unregister.
    // Taking its name here prevents a fresh installation during cleanup.
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr))
        brokerProtectedObjectFailure("Prepare cleanup serialization");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    Handle mutex(CreateMutexW(&attributes, FALSE, L"Global\\LoopZBrokerMaintenance"));
    LocalFree(descriptor);
    if (!mutex.value)
        brokerProtectedObjectFailure("Open cleanup serialization");
    const auto waited = WaitForSingleObject(mutex.value, 0);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
        throw std::runtime_error("Another service maintenance operation is running");
    struct Unlock {
        HANDLE value;
        ~Unlock() {
            ReleaseMutex(value);
        }
    } unlock{mutex.value};
    DeviceSetupLock devicesLock;
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        brokerProtectedObjectFailure("Inspect service before cleanup");
    struct Service {
        SC_HANDLE value;
        ~Service() {
            if (value)
                CloseServiceHandle(value);
        }
    } scm{manager};
    Service service{OpenServiceW(manager, kDeviceBrokerServiceName, SERVICE_QUERY_STATUS)};
    if (service.value || GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        throw std::runtime_error("Unregister the setup service before removing its files");
    const auto binaryRoot = brokerServiceDirectory(), storeRoot = deviceIntentStoreRoot();
    auto binary = inspect(binaryRoot, false);
    auto store = inspect(storeRoot, true);
    if (store && !store->files.empty()) {
        DeviceIntentStore database;
        const auto devices = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr);
        if (devices == INVALID_HANDLE_VALUE)
            brokerProtectedObjectFailure("Inspect desired devices before cleanup");
        struct Devices {
            HDEVINFO value;
            ~Devices() {
                SetupDiDestroyDeviceInfoList(value);
            }
        } close{devices};
        for (const auto& owner : database.ownersWithDesiredState())
            for (const auto& desired : database.snapshot(owner).records) {
                SP_DEVINFO_DATA device{sizeof(device)};
                const auto instance =
                    driverInstance(desired.kind == DeviceIntentKind::AppRoutingSink
                                       ? driverAppRoutingSinkKey(owner)
                                       : driverMixKey(owner, desired.mixId));
                if (SetupDiOpenDeviceInfoW(devices, instance.c_str(), nullptr, 0, &device)) {
                    const auto removed = std::any_of(
                        removedInstances.begin(), removedInstances.end(), [&](const auto& id) {
                            return _wcsicmp(id.c_str(), instance.c_str()) == 0;
                        });
                    if (!removed || !isLoopZDevice(devices, device))
                        throw std::runtime_error(
                            "Remove desired microphones before purging service state");
                } else if (GetLastError() != ERROR_NO_SUCH_DEVINST)
                    brokerProtectedObjectFailure("Confirm desired microphone absence");
            }
    }
    if (store)
        prepareDeletion(*store);
    if (binary)
        prepareDeletion(*binary);
    if (store)
        erase(*store);
    if (binary)
        erase(*binary);
}
} // namespace
void purgeBrokerInstallation() {
    purgeBrokerInstallationImpl({});
}
void purgeBrokerInstallationAfterDeviceRemoval(const DeviceSetupLock&,
                                               std::span<const std::wstring> removedInstances) {
    purgeBrokerInstallationImpl(removedInstances);
}
} // namespace loopz::windows
