// Opt-in administrative acceptance. Requires absent production service/store/path.
// Starts with an empty store, exercises one unique device, then removes its work.
#include "broker_deploy.hpp"
#include "broker_payload.hpp"
#include "broker_maintenance.hpp"
#include "broker_client.hpp"
#include "broker_pipe.hpp"
#include <loopz/device_intent.hpp>
#include <array>
#include "broker_installation.hpp"
#include "device_intent_store.hpp"
#include "device_operations.hpp"
#include "driver_identity.hpp"
#include "endpoint_roles.hpp"
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl.h>
#include <devguid.h>
#include <functional>
#include <map>
#include <algorithm>
#include <windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace loopz::windows {
void runLiveCoordinatorAcceptance(const std::string& mixId);
#ifdef LOOPZ_RUNTIME_BROKER_ACCEPTANCE
void runRuntimeBrokerAcceptance(const std::string& mixId);
#endif
} // namespace loopz::windows
using namespace loopz::windows;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(std::string(message) + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
}
struct Service {
    SC_HANDLE value{};
    ~Service() {
        if (value)
            CloseServiceHandle(value);
    }
};
SERVICE_STATUS_PROCESS status(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS result{};
    DWORD bytes{};
    require(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&result),
                                 sizeof(result), &bytes),
            "Cannot query service state");
    return result;
}
void wait(SC_HANDLE service, DWORD desired) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        const auto current = status(service);
        if (current.dwCurrentState == desired) {
            require(current.dwWin32ExitCode == 0 && current.dwServiceSpecificExitCode == 0,
                    "Service reported failure");
            return;
        }
        require(!(desired == SERVICE_RUNNING && current.dwCurrentState == SERVICE_STOPPED),
                "Service stopped during startup");
        require(std::chrono::steady_clock::now() < deadline, "Service transition timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
void stop(SC_HANDLE service) {
    if (status(service).dwCurrentState == SERVICE_STOPPED)
        return;
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, status(service).dwProcessId);
    struct Process {
        HANDLE value;
        ~Process() {
            if (value)
                CloseHandle(value);
        }
    } held{process};
    SERVICE_STATUS ignored{};
    require(ControlService(service, SERVICE_CONTROL_STOP, &ignored), "Cannot stop service");
    wait(service, SERVICE_STOPPED);
    if (held.value)
        require(WaitForSingleObject(held.value, 10000) == WAIT_OBJECT_0,
                "Stopped service process has not exited");
}
struct Cleanup {
    SC_HANDLE manager;
    Service service;
    std::filesystem::path binaryRoot, storeRoot;
    bool ownsBinary{}, ownsStore{};
    std::wstring deviceOwner;
    std::string deviceMix;
    bool clean() noexcept {
        try {
            if (!service.value && (ownsBinary || ownsStore)) {
                service.value = OpenServiceW(manager, kDeviceBrokerServiceName, SERVICE_ALL_ACCESS);
                require(service.value || GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST,
                        "Cannot inspect fixture service during cleanup");
            }
            if (service.value) {
                stop(service.value);
                if (!deviceMix.empty()) {
                    DeviceSetupLock lock;
                    require(!removeMixDevice(deviceOwner, deviceMix).rebootRequired,
                            "Test microphone cleanup requires restart");
                    deviceMix.clear();
                }
                require(DeleteService(service.value), "Cannot delete fixture service");
                CloseServiceHandle(service.value);
                service.value = nullptr;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                for (;;) {
                    Service check{
                        OpenServiceW(manager, kDeviceBrokerServiceName, SERVICE_QUERY_STATUS)};
                    if (!check.value && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
                        break;
                    require(std::chrono::steady_clock::now() < deadline,
                            "Service deletion timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            if (ownsBinary) {
                std::filesystem::remove_all(binaryRoot);
                ownsBinary = false;
            }
            if (ownsStore) {
                std::filesystem::remove_all(storeRoot);
                ownsStore = false;
            }
            return true;
        } catch (const std::exception& error) {
            std::cerr << "Service fixture cleanup failed; retaining installation: " << error.what()
                      << '\n';
            return false;
        }
    }
    ~Cleanup() {
        if (service.value || ownsBinary || ownsStore)
            clean();
    }
};
std::string ascii(const wchar_t* value) {
    std::wstring input(value);
    require(input.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos,
            "Digest must be hexadecimal");
    std::string result;
    for (const auto digit : input) {
        auto c = static_cast<char>(digit);
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
        result += c;
    }
    return result;
}
void restrictedRequests(DWORD serverPid, uint64_t revision, size_t desiredCount,
                        const std::function<void(HANDLE)>& exercise) {
    struct Handle {
        HANDLE value{};
        ~Handle() {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    } token, restricted, cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(cancel.value != nullptr, "Cannot create cancellation event");
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token.value),
            "Cannot read test token");
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD size = static_cast<DWORD>(administrators.size());
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size),
            "Cannot create administrator SID");
    SID_AND_ATTRIBUTES disabled{administrators.data(), 0};
    require(CreateRestrictedToken(token.value, DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0,
                                  nullptr, &restricted.value),
            "Cannot restrict test token");
    require(ImpersonateLoggedOnUser(restricted.value), "Cannot enter restricted test context");
    struct Revert {
        ~Revert() {
            if (!RevertToSelf())
                std::terminate();
        }
    } revert;
    for (const DWORD access :
         {DWORD(PROCESS_TERMINATE), DWORD(PROCESS_VM_WRITE), DWORD(PROCESS_DUP_HANDLE)}) {
        const auto process = OpenProcess(access, FALSE, serverPid);
        const auto error = GetLastError();
        if (process)
            CloseHandle(process);
        require(!process && error == ERROR_ACCESS_DENIED,
                "Restricted client gained process modification rights");
    }
    Service manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    require(manager.value != nullptr, "Restricted client cannot query service manager");
    for (const DWORD access :
         {DWORD(SERVICE_CHANGE_CONFIG), DWORD(SERVICE_STOP), DWORD(WRITE_DAC), DWORD(DELETE)}) {
        Service service{OpenServiceW(manager.value, kDeviceBrokerServiceName, access)};
        const auto error = GetLastError();
        require(!service.value && error == ERROR_ACCESS_DENIED,
                "Restricted client gained service modification rights");
    }
    const auto result =
        loopz::JSON::parse(callDeviceBroker(R"({"version":1,"action":"status"})", cancel.value));
    require(result.at("ok") == true && result.at("revision") == revision &&
                result.at("desiredRecordCount") == desiredCount,
            "Restricted client status failed");
    const auto forged = loopz::JSON::parse(
        callDeviceBroker(R"({"version":1,"action":"status","owner":"S-1-5-18"})", cancel.value));
    require(forged.at("ok") == false, "Forged owner was accepted");
    exercise(cancel.value);
    SetEvent(cancel.value);
    bool cancelled{};
    try {
        callDeviceBroker(R"({"version":1,"action":"status"})", cancel.value);
    } catch (const std::exception&) {
        cancelled = true;
    }
    require(cancelled, "Pre-cancelled request was sent");
    std::cout
        << "PASS: restricted-token verified client, status, forged-owner rejection, cancellation\n";
}
std::string uuid() {
    GUID value{};
    require(SUCCEEDED(CoCreateGuid(&value)), "Cannot create test identity");
    std::array<wchar_t, 40> text{};
    require(StringFromGUID2(value, text.data(), static_cast<int>(text.size())) != 0,
            "Cannot encode test identity");
    std::string result;
    for (size_t i = 1; i < 37; ++i) {
        auto c = static_cast<char>(text[i]);
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
        result += c;
    }
    return result;
}
void awaitDevice(HANDLE cancel, const std::string& id, const char* phase, uint64_t revision) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        const auto query = loopz::JSON{{"version", 1}, {"action", "status"}, {"mixId", id}}.dump();
        const auto reply = loopz::JSON::parse(callDeviceBroker(query, cancel));
        require(reply.at("ok") == true && reply.at("revision") == revision,
                "Unexpected device status revision");
        const auto& device = reply.at("device");
        if (device.at("phase") == phase) {
            if (std::string_view(phase) == "ready")
                require(device.at("revision") == revision, "Stale Ready was exposed");
            return;
        }
        if (device.at("phase") == "error" || device.at("phase") == "reboot_required")
            throw std::runtime_error("Device setup failed: " + device.dump());
        require(std::chrono::steady_clock::now() < deadline, "Device readiness timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
std::map<std::string, EndpointRole> endpointIds(std::wstring_view owner, const std::string& id) {
    std::map<std::string, EndpointRole> result;
    for (const auto& [uid, endpoint] : loopzEndpoints())
        if (endpoint.mixKey == driverMixKey(owner, id))
            result.emplace(uid, endpoint.role);
    return result;
}
void checkFormats(const std::map<std::string, EndpointRole>& ids, unsigned rate,
                  unsigned channels) {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Apartment {
        HRESULT result;
        ~Apartment() {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
    } initialized{apartment};
    require(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE,
            "Cannot initialize audio inspection");
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))),
            "Cannot enumerate audio endpoints");
    for (const auto& [id, role] : ids) {
        (void)role;
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, id.data(),
                                               static_cast<int>(id.size()), nullptr, 0);
        require(count > 0, "Invalid endpoint identity");
        std::wstring wide(static_cast<size_t>(count), L'\0');
        require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, id.data(),
                                    static_cast<int>(id.size()), wide.data(), count) == count,
                "Cannot decode endpoint identity");
        Microsoft::WRL::ComPtr<IMMDevice> device;
        Microsoft::WRL::ComPtr<IAudioClient> client;
        require(
            SUCCEEDED(enumerator->GetDevice(wide.c_str(), &device)) &&
                SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)),
            "Cannot inspect endpoint audio format");
        WAVEFORMATEX* format{};
        require(SUCCEEDED(client->GetMixFormat(&format)), "Cannot read public endpoint format");
        const auto matches = format->nSamplesPerSec == rate && format->nChannels == channels;
        CoTaskMemFree(format);
        require(matches, "Public endpoint format differs from desired format");
    }
}
void checkName(std::wstring_view owner, const std::string& id) {
    const auto devices = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr);
    require(devices != INVALID_HANDLE_VALUE, "Cannot inspect adapter name");
    struct Devices {
        HDEVINFO value;
        ~Devices() {
            SetupDiDestroyDeviceInfoList(value);
        }
    } close{devices};
    SP_DEVINFO_DATA device{sizeof(device)};
    require(SetupDiOpenDeviceInfoW(devices, driverInstance(driverMixKey(owner, id)).c_str(),
                                   nullptr, 0, &device),
            "Cannot inspect renamed adapter");
    std::array<wchar_t, 256> name{};
    DWORD type{}, bytes{};
    require(SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_FRIENDLYNAME, &type,
                                              reinterpret_cast<BYTE*>(name.data()),
                                              static_cast<DWORD>(sizeof(name)), &bytes) &&
                type == REG_SZ && bytes >= sizeof(wchar_t) && bytes <= sizeof(name) &&
                name[bytes / sizeof(wchar_t) - 1] == 0 &&
                std::wstring_view(name.data()) == L"LoopZ — LoopZ service renamed acceptance",
            "Adapter rename did not retain the latest accepted intent");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 4, "Expected host binary, trusted test digest, and installed INF");
        const auto binaryRoot = brokerServiceDirectory();
        const auto storeRoot = deviceIntentStoreRoot();
        require(!std::filesystem::exists(binaryRoot) && !std::filesystem::exists(storeRoot),
                "Production paths already exist; refusing acceptance test");
        Service manager{
            OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
        require(manager.value != nullptr, "Cannot open service manager");
        {
            Service existing{
                OpenServiceW(manager.value, kDeviceBrokerServiceName, SERVICE_QUERY_STATUS)};
            require(!existing.value && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST,
                    "Service already exists or cannot be inspected");
        }
        const auto defaultsBefore = audioDefaultEndpoints();
        const auto endpointsBefore = loopzEndpointRoles();
        Cleanup cleanup{manager.value, {}, binaryRoot, storeRoot, false, false, {}, {}};
        const auto owner = currentOwnerSid();
        const auto mixId = "service-probe-" + uuid();
        cleanup.deviceOwner = owner;
        // Unique test identity is the only device ever eligible for cleanup.
        {
            const auto devices = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr);
            require(devices != INVALID_HANDLE_VALUE, "Cannot inspect test device identity");
            SP_DEVINFO_DATA device{sizeof(device)};
            const auto exists = SetupDiOpenDeviceInfoW(
                devices, driverInstance(driverMixKey(owner, mixId)).c_str(), nullptr, 0, &device);
            const auto error = GetLastError();
            SetupDiDestroyDeviceInfoList(devices);
            require(!exists && error == ERROR_NO_SUCH_DEVINST, "Test device already exists");
        }
        cleanup.deviceMix = mixId;
        std::map<std::string, EndpointRole> initialIds;
        require(endpointIds(owner, mixId).empty(), "Test endpoint identity already exists");
        loopz::DeviceIntentRequest desired;
        desired.operationId = uuid();
        desired.mixId = mixId;
        desired.name = "LoopZ service acceptance";
        // Mark ownership after absent-path preflight, including partial failures.
        cleanup.ownsBinary = true;
        cleanup.ownsStore = true;
        require(embeddedBrokerPayload().sha256 == ascii(argv[2]),
                "Embedded host differs from independent test digest");
        bool invalidRejected{};
        try {
            installOrRepairBrokerService(L"../untrusted.inf");
        } catch (const std::invalid_argument&) {
            invalidRejected = true;
        }
        require(invalidRejected && !std::filesystem::exists(binaryRoot) &&
                    !std::filesystem::exists(storeRoot),
                "Invalid receipt mutated installation");
        cleanup.service.value = CreateServiceW(
            manager.value, kDeviceBrokerServiceName, L"LoopZ registration rejection fixture",
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL, L"C:\\LoopZForeignFixtureNeverExecuted.exe", nullptr, nullptr,
            nullptr, L"LocalSystem", nullptr);
        require(cleanup.service.value != nullptr, "Cannot create foreign-registration fixture");
        bool foreignRejected{};
        try {
            installOrRepairBrokerService(argv[3]);
        } catch (const std::runtime_error&) {
            foreignRejected = true;
        }
        require(foreignRejected &&
                    status(cleanup.service.value).dwCurrentState == SERVICE_STOPPED &&
                    !std::filesystem::exists(binaryRoot) && !std::filesystem::exists(storeRoot),
                "Unexpected service registration was modified or installation was staged");
        require(DeleteService(cleanup.service.value), "Cannot delete foreign-registration fixture");
        CloseServiceHandle(cleanup.service.value);
        cleanup.service.value = nullptr;
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;) {
                Service pending{
                    OpenServiceW(manager.value, kDeviceBrokerServiceName, SERVICE_QUERY_STATUS)};
                if (!pending.value && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
                    break;
                require(std::chrono::steady_clock::now() < deadline,
                        "Fixture registration deletion timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        std::cout << "PASS: invalid receipt and foreign service rejected before installation\n";
        installOrRepairBrokerService(argv[3]);
        cleanup.service.value =
            OpenServiceW(manager.value, kDeviceBrokerServiceName, SERVICE_ALL_ACCESS);
        require(cleanup.service.value != nullptr, "Cannot inspect maintenance-created service");
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            if (cycle > 0)
                installOrRepairBrokerService(argv[3]);
            wait(cleanup.service.value, SERVICE_RUNNING);
            const auto running = status(cleanup.service.value);
            require(running.dwProcessId != 0, "Running service has no process");
            DWORD session{};
            require(ProcessIdToSessionId(running.dwProcessId, &session) && session == 0,
                    "Service is outside Session 0");
            std::cout << "RUNNING cycle=" << cycle << " pid=" << running.dwProcessId
                      << " session=0\n";
            restrictedRequests(running.dwProcessId, cycle, cycle == 0 ? 0 : 1, [&](HANDLE cancel) {
                if (cycle == 0) {
                    const auto wire = loopz::encodeDeviceIntent(desired).dump();
                    const auto accepted = loopz::JSON::parse(callDeviceBroker(wire, cancel));
                    require(accepted.at("ok") == true && accepted.at("acceptedRevision") == 1,
                            "Create intent was not committed");
                    const auto retry = loopz::JSON::parse(callDeviceBroker(wire, cancel));
                    require(retry.at("ok") == true && retry.at("acceptedRevision") == 1,
                            "Create retry was not idempotent");
                    awaitDevice(cancel, mixId, "ready", 1);
                    const auto consistent = readDeviceBrokerSnapshot(cancel);
                    require(consistent.revision == 1 && consistent.records.size() == 1 &&
                                consistent.records.front().mixId == mixId,
                            "Verified client snapshot reader failed");
                    const auto snapshot = loopz::JSON::parse(
                        callDeviceBroker(R"({"version":1,"action":"snapshot"})", cancel));
                    require(snapshot.at("ok") && snapshot.at("revision") == 1 &&
                                snapshot.at("records").size() == 1 &&
                                snapshot.at("nextOffset").is_null(),
                            "Restricted production snapshot failed");
                    const auto stored =
                        loopz::decodeDeviceIntent(snapshot.at("records").front().dump());
                    require(stored.mixId == mixId && stored.name == desired.name &&
                                stored.sampleRate == 48000 && stored.channels == 2,
                            "Restricted production snapshot returned wrong desired metadata");
                } else if (cycle == 1) {
                    awaitDevice(cancel, mixId, "ready", 1);
                    require(endpointIds(owner, mixId) == initialIds,
                            "Service restart changed endpoint identities");
                    desired.operationId = uuid();
                    desired.expectedRevision = 1;
                    desired.name = "LoopZ service renamed acceptance";
                    desired.sampleRate = 96000;
                    desired.channels = 8;
                    const auto accepted = loopz::JSON::parse(
                        callDeviceBroker(loopz::encodeDeviceIntent(desired).dump(), cancel));
                    require(accepted.at("ok") == true && accepted.at("acceptedRevision") == 2,
                            "Format edit was not committed");
                    auto stale = desired;
                    stale.operationId = uuid();
                    stale.name = "Stale edit must never apply";
                    stale.sampleRate = 44100;
                    stale.channels = 2;
                    require(loopz::JSON::parse(
                                callDeviceBroker(loopz::encodeDeviceIntent(stale).dump(), cancel))
                                    .at("ok") == false,
                            "Stale edit was accepted");
                    awaitDevice(cancel, mixId, "ready", 2);
                    require(endpointIds(owner, mixId) == initialIds,
                            "Format edit changed endpoint identities");
                    checkFormats(initialIds, 96000, 8);
                    checkName(owner, mixId);
                } else {
                    awaitDevice(cancel, mixId, "ready", 2);
                    require(endpointIds(owner, mixId) == initialIds,
                            "Updated service recovery changed endpoint identities");
                    checkFormats(initialIds, 96000, 8);
                    checkName(owner, mixId);
                    desired.operationId = uuid();
                    desired.expectedRevision = 2;
                    desired.action = loopz::DeviceIntentAction::Remove;
                    const auto accepted = loopz::JSON::parse(
                        callDeviceBroker(loopz::encodeDeviceIntent(desired).dump(), cancel));
                    require(accepted.at("ok") == true && accepted.at("acceptedRevision") == 3,
                            "Removal intent was not committed");
                    awaitDevice(cancel, mixId, "unrequested", 3);
                }
            });
            if (cycle == 0) {
                initialIds = endpointIds(owner, mixId);
                require(initialIds.size() == 2, "Service did not publish both endpoints");
                require(std::count_if(initialIds.begin(), initialIds.end(),
                                      [](const auto& entry) {
                                          return entry.second == EndpointRole::Microphone;
                                      }) == 1,
                        "Service did not publish one microphone");
                require(std::count_if(initialIds.begin(), initialIds.end(),
                                      [](const auto& entry) {
                                          return entry.second == EndpointRole::Feed;
                                      }) == 1,
                        "Service did not publish one Feed");
                std::cout << "PASS: restricted create, durable owner scope and two endpoints\n";
                checkFormats(initialIds, 48000, 2);
                DeviceIntentStore persisted;
                require(persisted.snapshot(owner).records.size() == 1 &&
                            persisted.snapshot(L"S-1-5-18").records.empty(),
                        "Pipe token owner did not scope durable intent");
            } else if (cycle == 1) {
                DeviceIntentStore persisted;
                const auto snapshot = persisted.snapshot(owner);
                require(snapshot.revision == 2 && snapshot.records.size() == 1 &&
                            snapshot.records.front().name == desired.name &&
                            snapshot.records.front().sampleRate == 96000 &&
                            snapshot.records.front().channels == 8,
                        "Latest format/name not durable");
                std::cout << "PASS: rename and 96 kHz eight-channel edit, stale rejection, IDs "
                             "retained\n";
            } else {
                require(endpointIds(owner, mixId).empty(), "Removed endpoints remain");
                DeviceIntentStore persisted;
                require(persisted.snapshot(owner).records.empty(), "Removal tombstone remains");
                std::cout
                    << "PASS: recovery preserves endpoint IDs, explicit removal prunes tombstone\n";
            }
            if (cycle == 1) {
                const auto priorPid = status(cleanup.service.value).dwProcessId;
                installOrRepairBrokerService(argv[3]);
                require(status(cleanup.service.value).dwCurrentState == SERVICE_RUNNING &&
                            status(cleanup.service.value).dwProcessId != priorPid,
                        "Live repair did not replace the service process");
                const auto cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                require(cancel != nullptr, "Cannot prepare crash recovery query");
                struct Event {
                    HANDLE value;
                    ~Event() {
                        CloseHandle(value);
                    }
                } event{cancel};
                awaitDevice(cancel, mixId, "ready", 2);
                const auto crashPid = status(cleanup.service.value).dwProcessId;
                const auto process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, crashPid);
                require(process != nullptr,
                        "Cannot open the exact test service for crash recovery");
                struct Process {
                    HANDLE value;
                    ~Process() {
                        CloseHandle(value);
                    }
                } crashed{process};
                require(TerminateProcess(process, 0xDEAD) &&
                            WaitForSingleObject(process, 10000) == WAIT_OBJECT_0,
                        "Test service did not terminate");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                for (;;) {
                    const auto recovered = status(cleanup.service.value);
                    if (recovered.dwCurrentState == SERVICE_RUNNING &&
                        recovered.dwProcessId != crashPid)
                        break;
                    require(std::chrono::steady_clock::now() < deadline,
                            "Automatic crash restart timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                awaitDevice(cancel, mixId, "ready", 2);
                require(endpointIds(owner, mixId) == initialIds,
                        "Crash recovery changed endpoint IDs");
                checkFormats(initialIds, 96000, 8);
                std::cout
                    << "PASS: live repair and automatic crash restart preserve state and IDs\n";
            }
            stopInstalledBrokerService();
            std::cout << "STOPPED cycle=" << cycle << '\n';
        }
        installOrRepairBrokerService(argv[3]);
        runLiveCoordinatorAcceptance(mixId);
#ifdef LOOPZ_RUNTIME_BROKER_ACCEPTANCE
        runRuntimeBrokerAcceptance(mixId);
#endif
        CloseServiceHandle(cleanup.service.value);
        cleanup.service.value = nullptr;
        unregisterBrokerService();
        unregisterBrokerService(); // Already absent is idempotent.
        stopInstalledBrokerService();
        require(cleanup.clean(), "Acceptance cleanup failed");
        require(audioDefaultEndpoints() == defaultsBefore, "Default audio roles changed");
        require(loopzEndpointRoles() == endpointsBefore, "Existing LoopZ endpoints changed");
        require(!std::filesystem::exists(binaryRoot) && !std::filesystem::exists(storeRoot),
                "Acceptance paths remain");
        {
            const auto fake = CreateNamedPipeW(
                kDeviceBrokerPipe,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                65536, 0, nullptr);
            require(fake != INVALID_HANDLE_VALUE, "Cannot bind spoof-server fixture");
            const auto cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            bool rejected{};
            try {
                callDeviceBroker(R"({"version":1,"action":"status"})", cancel);
            } catch (const std::exception&) {
                rejected = true;
            }
            if (cancel)
                CloseHandle(cancel);
            CloseHandle(fake);
            require(rejected, "Client accepted a spoof server");
            std::cout << "PASS: spoof server rejected\n";
        }
        std::cout << "PASS: protected host, real SCM LocalSystem Session 0 start/stop/restart and "
                     "cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
