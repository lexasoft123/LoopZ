#include "driver_identity.hpp"
#include "device_operations.hpp"
#include "broker_maintenance.hpp"
#include "broker_cleanup.hpp"
#include "endpoint_format.hpp"
#include "driver/audio_format.hpp"
#include <loopz/sample_rates.hpp>
#include <loopz/document_json.hpp>
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace {
using namespace loopz::windows;
struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() {
        if (value && value != INVALID_HANDLE_VALUE)
            RegCloseKey(value);
    }
};
struct Devices {
    HDEVINFO value{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr)};
    Devices() = default;
    explicit Devices(HDEVINFO inventory) : value(inventory) {}
    ~Devices() {
        if (value != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(value);
    }
};
[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
struct RoutingReadLock {
    HANDLE value{};
    RoutingReadLock(const std::filesystem::path& path, const std::string& expected) {
        std::string current(expected.size() + 1, '\0');
        // Prevent a concurrent edit from turning a desired root into an orphan
        // between checking the document and removing its obsolete microphones.
        value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (value == INVALID_HANDLE_VALUE) {
            value = nullptr;
            fail("Lock routing document for microphone reconciliation");
        }
        LARGE_INTEGER size{};
        DWORD read{};
        const bool matches =
            GetFileSizeEx(value, &size) &&
            size.QuadPart == static_cast<LONGLONG>(expected.size()) &&
            ReadFile(value, current.data(), static_cast<DWORD>(current.size()), &read, nullptr) &&
            read == expected.size() && current.compare(0, read, expected) == 0;
        if (!matches) {
            CloseHandle(value);
            value = nullptr;
            throw std::runtime_error("Mixes changed during setup. Run microphone repair again.");
        }
    }
    ~RoutingReadLock() {
        if (value)
            CloseHandle(value);
    }
    RoutingReadLock(const RoutingReadLock&) = delete;
    RoutingReadLock& operator=(const RoutingReadLock&) = delete;
};
std::filesystem::path package() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size())
        fail("Locate driver package");
    return std::filesystem::path(path.data()).parent_path() / L"driver" / L"LoopZAudio.inf";
}
unsigned pruneOwnedMicrophones(const std::vector<std::wstring>& desired,
                               const std::vector<BYTE>& ownerSid, BOOL& reboot) {
    struct Inventory {
        HDEVINFO value = SetupDiGetClassDevsW(&GUID_DEVCLASS_MEDIA, nullptr, nullptr, 0);
        ~Inventory() {
            if (value != INVALID_HANDLE_VALUE)
                SetupDiDestroyDeviceInfoList(value);
        }
    } inventory;
    if (inventory.value == INVALID_HANDLE_VALUE)
        fail("Enumerate microphones for reconciliation");
    std::vector<std::wstring> obsolete;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiEnumDeviceInfo(inventory.value, index, &device)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            fail("Enumerate microphones for reconciliation");
        }
        if (!isLoopZDevice(inventory.value, device))
            continue;
        std::array<wchar_t, 256> id{};
        if (!SetupDiGetDeviceInstanceIdW(inventory.value, &device, id.data(),
                                         static_cast<DWORD>(id.size()), nullptr))
            fail("Read microphone identity for reconciliation");
        const std::wstring instance(id.data());
        constexpr std::wstring_view prefix = L"ROOT\\LOOPZAUDIO\\LZ_";
        if (instance.size() != prefix.size() + 64 ||
            _wcsnicmp(instance.c_str(), prefix.data(), prefix.size()) != 0 ||
            instance.find_first_not_of(L"0123456789abcdefABCDEF", prefix.size()) !=
                std::wstring::npos)
            continue;
        if (std::any_of(desired.begin(), desired.end(), [&](const auto& wanted) {
                return _wcsicmp(instance.c_str(), wanted.c_str()) == 0;
            }))
            continue;
        RegistryKey key{SetupDiOpenDevRegKey(inventory.value, &device, DICS_FLAG_GLOBAL, 0,
                                             DIREG_DEV, KEY_QUERY_VALUE)};
        if (key.value == INVALID_HANDLE_VALUE)
            continue;
        std::vector<BYTE> recorded(ownerSid.size());
        DWORD bytes = static_cast<DWORD>(recorded.size()), type{};
        if (RegQueryValueExW(key.value, L"LoopZOwnerSid", nullptr, &type, recorded.data(),
                             &bytes) != ERROR_SUCCESS ||
            type != REG_BINARY || bytes != ownerSid.size() || recorded != ownerSid)
            continue;
        // Only canonical LoopZ roots with this exact account's protected owner
        // record qualify. Other accounts and unclaimed nodes are untouched.
        obsolete.push_back(instance);
    }
    Devices removal;
    if (removal.value == INVALID_HANDLE_VALUE)
        fail("Open microphone reconciliation device set");
    for (const auto& instance : obsolete) {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiOpenDeviceInfoW(removal.value, instance.c_str(), nullptr, 0, &device))
            fail("Open obsolete microphone");
        const bool restart = removeLoopZDevice(removal.value, device);
        reboot = reboot || restart;
    }
    return static_cast<unsigned>(obsolete.size());
}

std::wstring stageDriverPackage(const std::filesystem::path& inf) {
    std::array<wchar_t, 32768> published{};
    if (!SetupCopyOEMInfW(inf.c_str(), nullptr, SPOST_PATH, 0, published.data(),
                          static_cast<DWORD>(published.size()), nullptr, nullptr))
        fail("Stage LoopZ driver package");
    const auto publishedName = std::filesystem::path(published.data()).filename().wstring();
    HKEY packages{};
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\LoopZ\\DriverPackages", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &packages, nullptr) != ERROR_SUCCESS)
        fail("Record staged driver package");
    const DWORD present = 1;
    const auto recorded = RegSetValueExW(packages, publishedName.c_str(), 0, REG_DWORD,
                                         reinterpret_cast<const BYTE*>(&present), sizeof(present));
    RegCloseKey(packages);
    if (recorded != ERROR_SUCCESS)
        throw std::runtime_error("Cannot record staged driver package");
    return publishedName;
}

} // namespace
int wmain(int argc, wchar_t** argv) {
    using namespace loopz;
    using namespace loopz::windows;
    try {
        const std::wstring_view command = argc > 1 ? argv[1] : L"";
        if (!(((command == L"--uninstall" || command == L"--install-service" ||
                command == L"--stop-service" || command == L"--unregister-service" ||
                command == L"--purge-service") &&
               argc == 2) ||
              ((command == L"--sync" || command == L"--reconcile") && argc == 4)))
            throw std::invalid_argument(
                "Use --sync or --reconcile routing.json owner-SID, or --install-service, "
                "--stop-service, --unregister-service, or --uninstall");
        if (command == L"--purge-service") {
            purgeBrokerInstallation();
            std::cout << JSON{{"ok", true}}.dump() << '\n';
            return 0;
        }
        if (command == L"--stop-service" || command == L"--unregister-service") {
            if (command == L"--stop-service")
                stopInstalledBrokerService();
            else
                unregisterBrokerService();
            std::cout << JSON{{"ok", true}}.dump() << '\n';
            return 0;
        }
        if (command == L"--install-service") {
            std::wstring receipt;
            {
                DeviceSetupLock setupLock;
                receipt = stageDriverPackage(package());
            }
            // Startup recovery must run after the package staging lock is released.
            const auto result = installOrRepairBrokerService(receipt);
            std::cout << JSON{{"ok", true},
                              {"serviceInstalled", true},
                              {"migratedDevices", result.migratedDevices},
                              {"rebootRequired", result.rebootRequired}}
                             .dump()
                      << '\n';
            return result.rebootRequired ? 3010 : 0;
        }
        if (command == L"--uninstall")
            stopInstalledBrokerService();
        DeviceSetupLock setupLock;
        const auto inf = package();
        if (command == L"--uninstall" && argc == 2) {
            // Enumerate present and non-present nodes so upgrades leave no orphaned devices.
            Devices inventory{SetupDiGetClassDevsW(&GUID_DEVCLASS_MEDIA, nullptr, nullptr, 0)};
            const auto set = inventory.value;
            if (set == INVALID_HANDLE_VALUE)
                fail("Enumerate LoopZ devices");
            std::vector<std::wstring> instances;
            for (DWORD index = 0;; ++index) {
                SP_DEVINFO_DATA device{sizeof(device)};
                if (!SetupDiEnumDeviceInfo(set, index, &device)) {
                    if (GetLastError() != ERROR_NO_MORE_ITEMS)
                        fail("Enumerate devices");
                    break;
                }
                std::array<wchar_t, 2048> id{};
                if (!isLoopZDevice(set, device))
                    continue;
                if (!SetupDiGetDeviceInstanceIdW(set, &device, id.data(),
                                                 static_cast<DWORD>(id.size()), nullptr))
                    fail("Read LoopZ microphone identity before uninstall");
                instances.emplace_back(id.data());
            }
            Devices devices;
            BOOL reboot{};
            for (const auto& id : instances) {
                SP_DEVINFO_DATA device{sizeof(device)};
                if (!SetupDiOpenDeviceInfoW(devices.value, id.c_str(), nullptr, 0, &device))
                    fail("Open LoopZ microphone");
                const bool restart = removeLoopZDevice(devices.value, device);
                reboot = reboot || restart;
            }
            HKEY packages{};
            const auto opened =
                RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\LoopZ\\DriverPackages", 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE, &packages);
            if (opened != ERROR_SUCCESS && opened != ERROR_FILE_NOT_FOUND)
                throw std::runtime_error("Cannot read installed driver package receipts");
            if (opened == ERROR_SUCCESS) {
                std::vector<std::wstring> names;
                for (DWORD index = 0;; ++index) {
                    std::array<wchar_t, 260> name{};
                    DWORD length = static_cast<DWORD>(name.size());
                    const auto result = RegEnumValueW(packages, index, name.data(), &length,
                                                      nullptr, nullptr, nullptr, nullptr);
                    if (result == ERROR_NO_MORE_ITEMS)
                        break;
                    if (result != ERROR_SUCCESS) {
                        RegCloseKey(packages);
                        throw std::runtime_error("Cannot enumerate driver receipts");
                    }
                    std::wstring receipt(name.data(), length);
                    const auto digits =
                        receipt.size() >= 8 ? receipt.substr(3, receipt.size() - 7) : L"";
                    if (!receipt.starts_with(L"oem") || !receipt.ends_with(L".inf") ||
                        digits.empty() ||
                        digits.find_first_not_of(L"0123456789") != std::wstring::npos) {
                        RegCloseKey(packages);
                        throw std::runtime_error("Invalid driver receipt");
                    }
                    names.push_back(receipt);
                }
                for (const auto& name : names) {
                    BOOL packageReboot{};
                    std::array<wchar_t, 32768> windowsDirectory{};
                    if (!GetWindowsDirectoryW(windowsDirectory.data(),
                                              static_cast<UINT>(windowsDirectory.size()))) {
                        RegCloseKey(packages);
                        fail("Locate Windows INF directory");
                    }
                    const auto installedInf =
                        std::filesystem::path(windowsDirectory.data()) / L"INF" / name;
                    if (!DiUninstallDriverW(nullptr, installedInf.c_str(), 0, &packageReboot)) {
                        const auto error = GetLastError();
                        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NOT_FOUND) {
                            RegCloseKey(packages);
                            SetLastError(error);
                            fail("Remove driver package");
                        }
                    }
                    reboot = reboot || packageReboot;
                    const auto removedReceipt = RegDeleteValueW(packages, name.c_str());
                    if (removedReceipt != ERROR_SUCCESS && removedReceipt != ERROR_FILE_NOT_FOUND) {
                        RegCloseKey(packages);
                        throw std::runtime_error(
                            "Cannot remove installed driver package receipt (Windows error " +
                            std::to_string(removedReceipt) + ")");
                    }
                }
                RegCloseKey(packages);
                const auto removedReceipts =
                    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"Software\\LoopZ\\DriverPackages");
                if (removedReceipts != ERROR_SUCCESS && removedReceipts != ERROR_FILE_NOT_FOUND)
                    throw std::runtime_error(
                        "Cannot remove installed driver package receipts (Windows error " +
                        std::to_string(removedReceipts) + ")");
            }
            unregisterBrokerService();
            // Every enumerated removal succeeded, under this still-held lock.
            // Pending kernel cleanup needs a reboot, not an orphaned broker/store.
            purgeBrokerInstallationAfterDeviceRemoval(setupLock, instances);
            std::cout << JSON{{"ok", true}, {"rebootRequired", reboot != FALSE}}.dump() << '\n';
            return reboot ? 3010 : 0;
        }
        PSID parsedSid{};
        if (!ConvertStringSidToSidW(argv[3], &parsedSid))
            fail("Validate microphone owner");
        std::vector<BYTE> ownerSid(GetLengthSid(parsedSid));
        CopySid(static_cast<DWORD>(ownerSid.size()), ownerSid.data(), parsedSid);
        LocalFree(parsedSid);
        const std::filesystem::path routing(argv[2]);
        if (std::filesystem::file_size(routing) > 1024 * 1024)
            throw std::runtime_error("Routing file is too large");
        std::ifstream file(routing, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot read routing document");
        std::string routingBytes(1024 * 1024 + 1, '\0');
        file.read(routingBytes.data(), static_cast<std::streamsize>(routingBytes.size()));
        routingBytes.resize(static_cast<size_t>(file.gcount()));
        if (file.bad() || routingBytes.size() > 1024 * 1024)
            throw std::runtime_error("Cannot read bounded routing document");
        file.close();
        const auto document = decodeDocument(parseControlJSON(routingBytes));
        for (const auto& device : document.devices)
            if (!supportedVirtualSampleRate(device.sampleRate) ||
                !loopz_driver::AudioFormat{static_cast<unsigned>(device.sampleRate),
                                           device.routing.outputChannels}
                     .valid())
                throw std::runtime_error("Unsupported Windows microphone format");
        // Stage first. Windows verifies catalog integrity and signature policy; no force flag,
        // certificate import, boot configuration change or test-signing bypass exists here.
        BOOL reboot{};
        const auto publishedName = stageDriverPackage(inf);
        Devices devices;
        if (devices.value == INVALID_HANDLE_VALUE)
            fail("Open audio device set");
        std::vector<SP_DEVINFO_DATA> created;
        created.reserve(document.devices.size());
        unsigned removed{};
        try {
            for (const auto& mix : document.devices) {
                const auto applied =
                    upsertMixDevice(devices.value, argv[3], mix, publishedName, true);
                if (applied.created)
                    created.push_back(applied.device);
                reboot = reboot || applied.rebootRequired;
            }
            if (!document.devices.empty()) {
                if (!reboot) {
                    for (const auto& mix : document.devices)
                        finalizeMixDevice(devices.value, argv[3], mix);
                }
            }
            if (command == L"--reconcile" && !reboot) {
                RoutingReadLock routingLock(routing, routingBytes);
                std::vector<std::wstring> desired;
                for (const auto& mix : document.devices)
                    desired.push_back(driverInstance(driverMixKey(argv[3], mix.id)));
                removed = pruneOwnedMicrophones(desired, ownerSid, reboot);
            }
        } catch (...) {
            for (auto& device : created) {
                try {
                    removeLoopZDevice(devices.value, device);
                } catch (...) {
                }
            }
            throw;
        }
        std::cout << JSON{{"ok", true},
                          {"microphones", document.devices.size()},
                          {"removed", removed},
                          {"rebootRequired", reboot != FALSE}}
                         .dump()
                  << '\n';
        return reboot ? 3010 : 0;
    } catch (const std::exception& error) {
        std::cerr << JSON{{"ok", false}, {"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
