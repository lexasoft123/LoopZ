// Opt-in administrative fixture. Never registered with CTest.
#include "device_operations.hpp"
#include "broker_reconcile.hpp"
#include "endpoint_roles.hpp"
#include "device_intent_store.hpp"
#include "driver_identity.hpp"
#include <devguid.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <algorithm>

using namespace loopz;
using namespace loopz::windows;
namespace {
void require(bool value, const char* detail) {
    if (!value)
        throw std::runtime_error(detail);
}
struct Devices {
    HDEVINFO value{SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MEDIA, nullptr)};
    ~Devices() {
        if (value != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(value);
    }
};
struct Lock {
    HANDLE value{};
    Lock() {
        PSECURITY_DESCRIPTOR descriptor{};
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr),
                "Cannot prepare fixture setup lock");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        value = CreateMutexW(&attributes, FALSE, L"Global\\LoopZDriverSetup");
        LocalFree(descriptor);
        require(value != nullptr, "Cannot open fixture setup lock");
        const auto waited = WaitForSingleObject(value, 0);
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
            CloseHandle(value);
            value = nullptr;
            throw std::runtime_error("Another microphone setup is running");
        }
    }
    ~Lock() {
        if (value) {
            ReleaseMutex(value);
            CloseHandle(value);
        }
    }
};
void validateRoot(const std::filesystem::path& root) {
    require(root.is_absolute() && root == root.lexically_normal() &&
                root.parent_path() == deviceIntentStoreRoot().parent_path() &&
                root.filename().wstring().starts_with(L"LoopZSystemDeviceTest-"),
            "System fixture requires an isolated ProgramData test directory");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 3,
                "Use --prepare directory or --run directory owner-SID test-ID installed-INF");
        const std::filesystem::path root(argv[2]);
        validateRoot(root);
        if (std::wstring_view(argv[1]) == L"--prepare" && argc == 3) {
            require(!std::filesystem::exists(root), "Fixture directory already exists");
            provisionDeviceIntentStore(root);
            return 0;
        }
        const bool workerMode = std::wstring_view(argv[1]) == L"--worker";
        require((std::wstring_view(argv[1]) == L"--run" || workerMode) && argc == 6,
                "Invalid fixture invocation");
        require(currentOwnerSid() == L"S-1-5-18", "Fixture must run as LocalSystem");
        DWORD session{};
        require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session == 0,
                "Fixture must run in Session 0");
        DeviceIntentStore protectedRoot(
            root); // Verify security before writing results or operating devices.
        const std::wstring owner(argv[3]);
        const std::wstring id(argv[4]);
        require(id.starts_with(L"system-probe-") && id.size() == 49 &&
                    id.find_first_not_of(L"0123456789abcdef-", 13) == std::wstring::npos,
                "Fixture only accepts a unique system-probe test mix ID");
        VirtualDeviceDefinition mix;
        for (const auto character : id)
            mix.id.push_back(static_cast<char>(character));
        mix.name = "System setup acceptance";
        mix.sampleRate = 48000;
        mix.routing.outputChannels = 2;
        JSON report{{"ok", false},        {"cleanup", false},
                    {"session", session}, {"processOwner", "S-1-5-18"},
                    {"mixId", mix.id},    {"checks", JSON::array()}};
        bool initiallyAbsent = false;
        std::unique_ptr<Lock> fixtureLock;
        if (!workerMode)
            fixtureLock = std::make_unique<Lock>();
        Devices devices;
        require(devices.value != INVALID_HANDLE_VALUE, "Cannot open fixture device set");
        try {
            const auto instance = driverInstance(driverMixKey(owner, mix.id));
            SP_DEVINFO_DATA device{sizeof(device)};
            require(!SetupDiOpenDeviceInfoW(devices.value, instance.c_str(), nullptr, 0, &device) &&
                        GetLastError() == ERROR_NO_SUCH_DEVINST,
                    "Fixture root already exists or cannot be inspected");
            initiallyAbsent = true;
            if (workerMode) {
                const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                require(stop != nullptr, "Cannot create worker stop event");
                struct Event {
                    HANDLE value;
                    ~Event() {
                        CloseHandle(value);
                    }
                } close{stop};
                WindowsBrokerDeviceAdapter adapter(argv[5]);
                std::vector<BrokerDeviceProgress> progress;
                const auto observe = [&](const BrokerDeviceProgress& update) {
                    progress.push_back(update);
                };
                BrokerReconciler worker(protectedRoot, adapter, stop, observe);
                const auto checkPass = [&](BrokerReconciler& candidate, uint64_t revision,
                                           BrokerDevicePhase phase) {
                    progress.clear();
                    const auto retry = candidate.pass();
                    for (const auto& update : progress)
                        if (update.phase == BrokerDevicePhase::Error)
                            throw std::runtime_error(update.error);
                    require(!retry && std::any_of(progress.begin(), progress.end(),
                                                  [&](const auto& update) {
                                                      return update.owner == owner &&
                                                             update.mixId == mix.id &&
                                                             update.revision == revision &&
                                                             update.phase == phase;
                                                  }),
                            "Worker did not complete the requested revision");
                };
                const auto endpointIds = [&] {
                    std::map<std::string, EndpointRole> ids;
                    const auto key = driverMixKey(owner, mix.id);
                    for (const auto& [uid, endpoint] : loopzEndpoints())
                        if (endpoint.mixKey == key)
                            ids.emplace(uid, endpoint.role);
                    require(ids.size() == 2, "Worker endpoints are unavailable");
                    require(std::count_if(ids.begin(), ids.end(),
                                          [](const auto& endpoint) {
                                              return endpoint.second == EndpointRole::Feed;
                                          }) == 1 &&
                                std::count_if(ids.begin(), ids.end(),
                                              [](const auto& endpoint) {
                                                  return endpoint.second ==
                                                         EndpointRole::Microphone;
                                              }) == 1,
                            "Worker endpoint roles are not one Feed and one microphone");
                    return ids;
                };
                DeviceIntentRequest desired;
                desired.operationId = "12345678-1234-1234-1234-123456789abc";
                desired.mixId = mix.id;
                desired.name = "System worker acceptance";
                require(protectedRoot.accept(owner, desired) == 1,
                        "Cannot persist worker create intent");
                checkPass(worker, 1, BrokerDevicePhase::Ready);
                const auto initialIds = endpointIds();
                report["checks"].push_back(
                    "Worker created and finalized persisted 48 kHz stereo intent");
                progress.clear();
                require(!worker.pass() && progress.empty(),
                        "Unchanged ready work was applied again");
                BrokerReconciler restarted(protectedRoot, adapter, stop, observe);
                checkPass(restarted, 1, BrokerDevicePhase::Ready);
                require(endpointIds() == initialIds, "Worker restart changed endpoint IDs");
                report["checks"].push_back(
                    "Fresh worker recovered desired state without changing endpoint IDs");
                desired.operationId.back() = 'd';
                desired.expectedRevision = 1;
                desired.name = "System worker renamed";
                desired.sampleRate = 96000;
                desired.channels = 8;
                require(protectedRoot.accept(owner, desired) == 2,
                        "Cannot persist worker format intent");
                checkPass(restarted, 2, BrokerDevicePhase::Ready);
                report["formatEndpointIdsPreserved"] = endpointIds() == initialIds;
                require(report["formatEndpointIdsPreserved"].get<bool>(),
                        "Format change regenerated endpoint IDs");
                report["checks"].push_back("Worker applied persisted 96 kHz eight-channel format "
                                           "without changing endpoint IDs");
                desired.operationId.back() = 'e';
                desired.expectedRevision = 2;
                desired.action = DeviceIntentAction::Remove;
                require(protectedRoot.accept(owner, desired) == 3,
                        "Cannot persist worker removal intent");
                checkPass(restarted, 3, BrokerDevicePhase::Removed);
                require(protectedRoot.snapshot(owner).records.empty() &&
                            protectedRoot.snapshot(owner).revision == 3,
                        "Worker did not acknowledge current removal tombstone");
                report["checks"].push_back(
                    "Worker removed adapter and acknowledged exact removal revision");
            } else {
                auto applied = upsertMixDevice(devices.value, owner, mix, argv[5]);
                require(applied.created && !applied.rebootRequired,
                        "Initial setup requires repair or reboot");
                finalizeMixDevice(devices.value, owner, mix);
                report["checks"].push_back("LocalSystem created and finalized 48 kHz stereo");
                mix.sampleRate = 96000;
                mix.routing.outputChannels = 8;
                mix.name = "System setup renamed";
                applied = upsertMixDevice(devices.value, owner, mix, argv[5]);
                require(!applied.created && !applied.rebootRequired,
                        "Format edit requires repair or reboot");
                finalizeMixDevice(devices.value, owner, mix);
                report["checks"].push_back(
                    "LocalSystem renamed and finalized 96 kHz eight-channel format");
                applied = upsertMixDevice(devices.value, owner, mix, argv[5]);
                require(!applied.created && !applied.rebootRequired,
                        "Repeated setup is not a no-op");
                finalizeMixDevice(devices.value, owner, mix);
                report["checks"].push_back("Repeated setup succeeded without package maintenance");
            }
        } catch (const std::exception& error) {
            report["error"] = error.what();
        }
        if (initiallyAbsent) {
            try {
                const auto removed = removeMixDevice(owner, mix.id);
                require(!removed.rebootRequired, "Fixture removal requires reboot");
                const auto repeated = removeMixDevice(owner, mix.id);
                require(!repeated.existed && !repeated.rebootRequired,
                        "Repeated removal is not idempotent");
                report["cleanup"] = true;
                report["checks"].push_back("Scoped removal and repeated removal succeeded");
            } catch (const std::exception& error) {
                report["cleanupError"] = error.what();
            }
        }
        report["ok"] = !report.contains("error") && report["cleanup"].get<bool>();
        std::ofstream output(root / L"report.json", std::ios::binary);
        output << report.dump(2) << '\n';
        output.close();
        require(!output.fail(), "Cannot save protected system fixture report");
        return report["ok"].get<bool>() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
