#include "runtime.hpp"
#include "app_routing_preferences.hpp"
#include "app_routing_store.hpp"
#include "automatic_render_probe.hpp"
#include "endpoint_roles.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl.h>
#include <loopz/app_routing_lease.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace loopz;
using namespace loopz::windows;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct Image {
    std::filesystem::path directory, executable;
    bool keep{};
    Image() {
        GUID guid{};
        wchar_t name[40]{}, module[32768]{};
        require(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, name, 40),
                "Fixture identity");
        require(GetModuleFileNameW(nullptr, module, 32768) != 0, "Fixture image");
        directory = std::filesystem::temp_directory_path() / (std::wstring(L"LoopZ-route-") + name);
        require(std::filesystem::create_directory(directory), "Fixture directory");
        executable = directory / L"marker.exe";
        std::filesystem::copy_file(module, executable);
    }
    ~Image() {
        if (!keep) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    }
};
struct Child {
    PROCESS_INFORMATION process{};
    Handle job;
    explicit Child(const std::filesystem::path& image) {
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                                     &limits, sizeof(limits)),
                "Contain silent child");
        auto command = L"\"" + image.wstring() + L"\" --silent";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        require(CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                               &process) != FALSE,
                "Start silent child");
        if (!AssignProcessToJobObject(job.value, process.hProcess) ||
            ResumeThread(process.hThread) == DWORD(-1)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            process = {};
            throw std::runtime_error("Prepare silent child");
        }
    }
    ~Child() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
                TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
        }
        if (process.hThread)
            CloseHandle(process.hThread);
    }
};
std::string activeEndpoint(DWORD pid) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> endpoints;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))) &&
                SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)),
            "Inspect silent endpoints");
    UINT count{};
    require(SUCCEEDED(endpoints->GetCount(&count)) && count <= 4096, "Endpoint count");
    std::string result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(endpoints->Item(i, &device)) ||
            FAILED(
                device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
            FAILED(manager->GetSessionEnumerator(&sessions)))
            continue;
        int n{};
        require(SUCCEEDED(sessions->GetCount(&n)) && n >= 0 && n <= 4096, "Session count");
        for (int j = 0; j < n; ++j) {
            ComPtr<IAudioSessionControl> session;
            ComPtr<IAudioSessionControl2> detail;
            DWORD owner{};
            AudioSessionState state{};
            if (FAILED(sessions->GetSession(j, &session)) || FAILED(session.As(&detail)) ||
                detail->GetProcessId(&owner) != S_OK || owner != pid ||
                FAILED(session->GetState(&state)) || state != AudioSessionStateActive)
                continue;
            LPWSTR text{};
            require(SUCCEEDED(device->GetId(&text)) && text, "Silent endpoint identity");
            const auto uid = utf8(text);
            CoTaskMemFree(text);
            require(result.empty() || result == uid, "Silent marker has multiple active endpoints");
            result = uid;
        }
    }
    return result;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
} // namespace
int runOverrideFixture(int argc, wchar_t** argv) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    require(SUCCEEDED(initialized), "Initialize override fixture MTA");
    struct Apartment {
        ~Apartment() {
            CoUninitialize();
        }
    } apartment;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--silent")
        return probe::automaticSilence(60);
    wchar_t flag[2]{};
    require(argc == 3 && GetEnvironmentVariableW(L"LOOPZ_APP_OVERRIDE_ACCEPTANCE", flag, 2) == 1 &&
                flag[0] == L'1',
            "Requires explicit override acceptance opt-in and two virtual Feeds");
    const auto first = utf8(argv[1]), second = utf8(argv[2]);
    const auto endpoints = loopzEndpoints();
    require(endpoints.contains(first) && endpoints.contains(second) &&
                endpoints.at(first).role == EndpointRole::Feed &&
                endpoints.at(second).role == EndpointRole::Feed &&
                endpoints.at(first).mixKey != endpoints.at(second).mixKey,
            "Requires distinct authenticated virtual Feeds");
    const auto defaults = audioDefaultEndpoints();
    Image image;
    auto child = std::make_unique<Child>(image.executable);
    const auto wait = [&](const std::string* expected) {
        const auto deadline = GetTickCount64() + 5000;
        do {
            require(WaitForSingleObject(child->process.hProcess, 0) == WAIT_TIMEOUT,
                    "Silent child exited early");
            const auto current = activeEndpoint(child->process.dwProcessId);
            if (!current.empty() && (!expected || current == *expected))
                return current;
            Sleep(100);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error("Silent output did not settle");
    };
    const auto initialEndpoint = wait(nullptr);
    AppAudioPolicy policy;
    require(policy.available(), "Private app policy unavailable");
    auto access = std::make_unique<ProcessRoutingPreferences>(policy, child->process.dwProcessId,
                                                              image.executable);
    const auto original = access->read();
    const auto restoreInitial = [&] {
        require(access != nullptr, "Fixture app access is unavailable for restoration");
        for (unsigned role = 0; role < 3; ++role)
            access->write(role, original[role]);
        require(access->read() == original, "Fixture original preferences not restored");
        wait(&initialEndpoint);
    };
    try {
        const auto storePath = image.directory / L"recovery";
        auto store = std::make_unique<AppRoutingStore>(storePath);
        auto lease = std::make_unique<AppRoutingLease>(store->journal());
        RouteDocument document;
        VirtualDeviceDefinition device;
        device.id = "override-local-" + utf8(image.directory.filename().native());
        device.name = "Override fixture";
        device.routing.sources = {{"selected", 2}, {"alias", 2}, {"unrelated", 2}};
        device.routing.sources[1].enabled = false;
        device.routing.sources[2].enabled = false;
        device.bindings = {
            {"selected", SourceKind::Application, utf8(image.executable.native()), true},
            {"alias", SourceKind::Application, access->application(), true},
            {"unrelated", SourceKind::Application, "C:\\LoopZ-unrelated-not-installed.exe", true}};
        document.devices.push_back(device);
        const auto storage = image.directory / L"local-routing.json";
        const auto initialBytes = encodeDocument(document).dump();
        {
            std::ofstream file(storage, std::ios::binary);
            file << initialBytes;
            require(bool(file), "Save fixture document");
        }
        auto runtime = std::make_unique<Runtime>(storage, false); // No broker/device management.
        const auto target = utf8(AppAudioPolicy::renderInterface(first));
        require(lease->apply(*access, target) == AppRoutingApplyResult::Applied,
                "Initial owned route");
        wait(&first);
        // Simulated external output choice, restricted to this owned silent app.
        const auto external = utf8(AppAudioPolicy::renderInterface(second));
        for (unsigned role = 0; role < 3; ++role)
            access->write(role, external);
        wait(&second);
        {
            Handle blocker;
            blocker.value = CreateFileW(storage.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(blocker.value != INVALID_HANDLE_VALUE,
                    "Pin fixture document for failed-save test");
            const auto recovery = runtime->recoverApplicationOutputs(store->journal(), policy);
            const bool failed =
                recovery.at("pending") == 1 &&
                recovery.at("applications")[0].at("phase") == "recovery-pending" &&
                !recovery.at("applications")[0].at("error").get<std::string>().empty();
            require(failed && bytes(storage) == initialBytes &&
                        store->journal().snapshot().size() == 1,
                    "Failed desired-state save acknowledged override or changed original document");
        }
        // Saving succeeds while an inactive stream can still retain the old
        // target. Its receipt must remain until that stream actually closes.
        const auto savedRecovery = runtime->recoverApplicationOutputs(store->journal(), policy);
        require(savedRecovery.at("pending") == 1 &&
                    savedRecovery.at("applications")[0].at("phase") ==
                        "waiting-for-stream-restoration" &&
                    store->journal().snapshot()[0].phase == AppRoutingReceiptPhase::UserOverride,
                "Inactive owned stream did not retain recovery ownership");
        runtime.reset();
        lease.reset();
        store.reset();
        access.reset();
        child.reset(); // Close all old streams; no unrelated process is touched.
        child = std::make_unique<Child>(image.executable);
        wait(nullptr);
        access = std::make_unique<ProcessRoutingPreferences>(policy, child->process.dwProcessId,
                                                             image.executable);
        store = std::make_unique<AppRoutingStore>(storePath, false);
        lease = std::make_unique<AppRoutingLease>(store->journal());
        runtime = std::make_unique<Runtime>(storage, false);
        const auto finishRecovery = [&] {
            JSON status;
            const auto deadline = GetTickCount64() + 5000;
            do {
                status = runtime->recoverApplicationOutputs(store->journal(), policy);
                if (status.at("pending") == 0)
                    return status;
                Sleep(100);
            } while (GetTickCount64() < deadline);
            throw std::runtime_error("Automatic output recovery did not finish: " + status.dump());
        };
        const auto recovery = finishRecovery();
        require(recovery.at("applications")[0].at("phase") == "user-override-saved" &&
                    access->read() != std::array<std::string, 3>{target, target, target},
                "Automatic restart recovery retook external output");
        const auto saved = decodeDocument(parseControlJSON(bytes(storage)));
        require(!saved.devices[0].bindings[0].muteOriginal &&
                    !saved.devices[0].bindings[1].muteOriginal &&
                    saved.devices[0].bindings[2].muteOriginal &&
                    saved.devices[0].routing.sources[0].gain == device.routing.sources[0].gain,
                "Runtime override changed unrelated controls or missed an alias binding");
        require(store->journal().snapshot().empty(),
                "Automatic acknowledgment did not release verified override");
        runtime.reset();
        runtime = std::make_unique<Runtime>(storage, false);
        const auto state = runtime->request(JSON{{"version", 1}, {"operation", "state"}});
        require(state.at("snapshot")
                        .at("document")
                        .at("devices")[0]
                        .at("bindings")[0]
                        .at("muteOriginal") == false,
                "Runtime restart lost saved override");
        // Desired state is now false. An outstanding owned route must restore
        // automatically without rewriting the saved output choice.
        const auto savedBytes = bytes(storage);
        restoreInitial();
        require(lease->apply(*access, target) == AppRoutingApplyResult::Applied,
                "Prepare deselected-route recovery");
        wait(&first);
        const auto restoring = runtime->recoverApplicationOutputs(store->journal(), policy);
        require(restoring.at("pending") == 1 &&
                    restoring.at("applications")[0].at("phase") ==
                        "waiting-for-stream-restoration" &&
                    access->read() == original && bytes(storage) == savedBytes,
                "Inactive deselected stream lost its receipt or original preferences");
        access.reset();
        child.reset();
        child = std::make_unique<Child>(image.executable);
        wait(&initialEndpoint);
        access = std::make_unique<ProcessRoutingPreferences>(policy, child->process.dwProcessId,
                                                             image.executable);
        const auto deselected = finishRecovery();
        require(deselected.at("applications")[0].at("phase") == "released" &&
                    store->journal().snapshot().empty() && bytes(storage) == savedBytes &&
                    access->read() == original,
                "Deselected output did not recover without changing desired state");
        wait(&initialEndpoint);
        runtime.reset();
        restoreInitial();
        const bool defaultsPreserved = defaults == audioDefaultEndpoints();
        require(defaultsPreserved, "Global default changed");
        std::cout << "{\"ok\":true,\"probe\":\"runtime-durable-app-output-override\","
                     "\"submittedOnlySilence\":true,\"failedSaveRetainedReceipt\":true,"
                     "\"controllerRestartPreservedChoice\":true,\"runtimeSavedMatchingBindings\":"
                     "true,\"unrelatedBindingPreserved\":true,\"runtimeRestartPreservedChoice\":"
                     "true,\"receiptReleasedAfterSave\":true,"
                     "\"automaticRecovery\":true,\"automaticDeselectionRestored\":true,"
                     "\"inactiveStreamRetainedReceipt\":true,"
                     "\"fixtureOriginalPreferencesRestored\":true,\"defaultsPreserved\":true}\n";
        return 0;
    } catch (...) {
        try {
            restoreInitial();
        } catch (...) {
            image.keep = true;
        }
        throw;
    }
}

int wmain(int argc, wchar_t** argv) {
    try {
        return runOverrideFixture(argc, argv);
    } catch (const std::exception& error) {
        std::cout << JSON{{"ok", false},
                          {"probe", "runtime-durable-app-output-override"},
                          {"error", error.what()}}
                         .dump()
                  << '\n';
        return 1;
    }
}
