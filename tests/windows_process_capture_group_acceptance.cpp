#include "process_capture_group.hpp"
#include "automatic_render_probe.hpp"
#include <functional>
#include "runtime.hpp"
#include "ipc.hpp"
#include "endpoint_roles.hpp"
#include "driver_identity.hpp"
#include "app_routing_preferences.hpp"
#include "app_routing_store.hpp"
#include "app_routing_startup.hpp"
#include "app_routing_sessions.hpp"
#include "app_routing_sink_lifetime.hpp"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl.h>
#include <array>
#include <cmath>
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
uint64_t nowNs() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const auto ticks = static_cast<uint64_t>(counter.QuadPart),
               rate = static_cast<uint64_t>(frequency.QuadPart);
    return ticks / rate * 1000000000ULL + ticks % rate * 1000000000ULL / rate;
}
struct Image {
    std::filesystem::path directory, executable;
    bool preserve{};
    Image() {
        GUID guid{};
        wchar_t id[40]{}, module[32768]{};
        require(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, id, 40) &&
                    GetModuleFileNameW(nullptr, module, 32768),
                "Create group fixture image");
        directory = std::filesystem::temp_directory_path() / (std::wstring(L"LoopZ-route-") + id);
        require(std::filesystem::create_directory(directory), "Create fixture directory");
        executable = directory / L"marker.exe";
        std::filesystem::copy_file(module, executable);
    }
    ~Image() {
        if (preserve)
            return;
        const auto until = GetTickCount64() + 5000;
        do {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
            if (!error)
                break;
            Sleep(20);
        } while (GetTickCount64() < until);
    }
};
struct Child {
    PROCESS_INFORMATION process{};
    HANDLE job{};
    Child(const std::filesystem::path& image, const std::wstring& arguments, bool ownJob = true) {
        if (ownJob) {
            job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            require(job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                                   sizeof(limits)),
                    "Contain group source");
        } else {
            BOOL contained = FALSE;
            require(IsProcessInJob(GetCurrentProcess(), nullptr, &contained) && contained,
                    "Surviving source requires its controller's cleanup job");
        }
        auto command = L"\"" + image.wstring() + L"\" " + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                            &process)) {
            if (job)
                CloseHandle(job);
            job = nullptr;
            throw std::runtime_error("Start group source");
        }
        if ((job && !AssignProcessToJobObject(job, process.hProcess)) ||
            ResumeThread(process.hThread) == DWORD(-1)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            if (job)
                CloseHandle(job);
            process = {};
            job = nullptr;
            throw std::runtime_error("Prepare group source");
        }
    }
    ~Child() {
        if (job) {
            TerminateJobObject(job, 0);
            const auto until = GetTickCount64() + 5000;
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            while (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                             sizeof(accounting), nullptr) &&
                   accounting.ActiveProcesses && GetTickCount64() < until)
                Sleep(10);
            CloseHandle(job);
        }
        if (process.hProcess) {
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
        }
        if (process.hThread)
            CloseHandle(process.hThread);
    }
};
int tone(std::wstring_view endpoint, double hz) {
    using Microsoft::WRL::ComPtr;
    const auto known = loopzEndpoints();
    const auto uid = utf8(endpoint);
    require(known.contains(uid) && known.at(uid).role == EndpointRole::Feed,
            "Tone requires a verified virtual Feed");
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))) &&
                SUCCEEDED(enumerator->GetDevice(std::wstring(endpoint).c_str(), &device)) &&
                SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)),
            "Open exact virtual tone endpoint");
    WAVEFORMATEX* format{};
    require(SUCCEEDED(client->GetMixFormat(&format)) && format, "Read tone format");
    struct Format {
        WAVEFORMATEX* value;
        ~Format() {
            CoTaskMemFree(value);
        }
    } cleanup{format};
    require(format->nChannels == 2 && format->nSamplesPerSec == 48000 &&
                format->wBitsPerSample == 32 && format->nBlockAlign == 8 &&
                (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                 (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                  format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
                  reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat ==
                      KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)),
            "Tone format must be float32 stereo 48 kHz");
    require(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr)),
            "Initialize virtual tone");
    UINT32 capacity{};
    require(SUCCEEDED(client->GetBufferSize(&capacity)), "Tone buffer size");
    ComPtr<IAudioRenderClient> render;
    require(SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))), "Tone render service");
    wchar_t ownModule[32768]{};
    require(GetModuleFileNameW(nullptr, ownModule, 32768) != 0, "Locate tone control directory");
    const auto silenceFile = std::filesystem::path(ownModule).parent_path() / L"silence";
    uint64_t position = 0;
    bool started = false, silenceLatched = false;
    const auto deadline = GetTickCount64() + 60000;
    while (GetTickCount64() < deadline) {
        const bool silent = std::filesystem::exists(silenceFile);
        if (silent && !silenceLatched) {
            if (started)
                require(SUCCEEDED(client->Stop()) && SUCCEEDED(client->Reset()),
                        "Clear tone before output policy restoration");
            started = false;
            silenceLatched = true;
        }
        UINT32 padding{};
        require(SUCCEEDED(client->GetCurrentPadding(&padding)) && padding <= capacity,
                "Tone padding");
        if (const auto frames = capacity - padding) {
            BYTE* bytes{};
            require(SUCCEEDED(render->GetBuffer(frames, &bytes)) && bytes, "Tone buffer");
            auto* samples = reinterpret_cast<float*>(bytes);
            for (UINT32 frame = 0; frame < frames; ++frame) {
                const auto value = static_cast<float>(
                    (silent ? 0.0 : .03) * std::sin(6.283185307179586 * hz *
                                                    static_cast<double>(position + frame) / 48000));
                samples[frame * 2] = samples[frame * 2 + 1] = value;
            }
            position += frames;
            require(SUCCEEDED(render->ReleaseBuffer(frames, 0)), "Publish virtual tone");
        }
        if (!started) {
            require(SUCCEEDED(client->Start()), "Start virtual tone");
            started = true;
            wchar_t module[32768]{};
            require(GetModuleFileNameW(nullptr, module, 32768) != 0,
                    "Locate tone readiness directory");
            const auto ready = std::filesystem::path(module).parent_path() /
                               (L"tone-" + std::to_wstring(GetCurrentProcessId()) + L".ready");
            std::ofstream file(ready);
            file << "started";
            require(bool(file), "Publish tone stream readiness");
            if (silenceLatched) {
                std::ofstream acknowledgment(
                    std::filesystem::path(ownModule).parent_path() /
                    (L"silent-" + std::to_wstring(GetCurrentProcessId()) + L".ready"));
                acknowledgment << "silent";
                require(bool(acknowledgment), "Acknowledge drained silent marker stream");
            }
        }
        Sleep(2);
    }
    require(SUCCEEDED(client->Stop()), "Stop virtual tone");
    return 0;
}
struct PublicRecording {
    std::vector<float> left, right;
    uint64_t discontinuities{}, timestampErrors{}, silentFrames{};
};
PublicRecording recordPublicMicrophone(const wchar_t* uid) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator))) &&
                SUCCEEDED(enumerator->GetDevice(uid, &device)) &&
                SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)),
            "Open exact public virtual microphone");
    WAVEFORMATEX* format{};
    require(SUCCEEDED(client->GetMixFormat(&format)) && format, "Read public microphone format");
    struct Format {
        WAVEFORMATEX* value;
        ~Format() {
            CoTaskMemFree(value);
        }
    } cleanup{format};
    require(format->nChannels == 2 && format->nSamplesPerSec == 48000 &&
                format->wBitsPerSample == 32 && format->nBlockAlign == 8 &&
                (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                 (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                  format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
                  reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat ==
                      KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)),
            "Public microphone must be float32 stereo 48 kHz");
    require(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr)),
            "Initialize public microphone recording");
    ComPtr<IAudioCaptureClient> capture;
    require(SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start()),
            "Start public microphone recording");
    PublicRecording recording;
    uint64_t skipped = 0;
    const auto until = GetTickCount64() + 5000;
    while (recording.left.size() < 24000 && GetTickCount64() < until) {
        UINT32 packet{};
        require(SUCCEEDED(capture->GetNextPacketSize(&packet)), "Read public packet size");
        if (!packet) {
            Sleep(2);
            continue;
        }
        BYTE* bytes{};
        UINT32 frames{};
        DWORD flags{};
        require(SUCCEEDED(capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr)),
                "Read public microphone packet");
        const auto skip = static_cast<UINT32>(std::min<uint64_t>(frames, 9600 - skipped));
        skipped += skip;
        if (skip < frames && recording.left.size() < 24000) {
            recording.discontinuities += (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            recording.timestampErrors += (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
            const auto* values = reinterpret_cast<const float*>(bytes);
            for (UINT32 i = skip; i < frames && recording.left.size() < 24000; ++i) {
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                recording.silentFrames += silent;
                recording.left.push_back(silent ? 0 : values[i * 2]);
                recording.right.push_back(silent ? 0 : values[i * 2 + 1]);
            }
        }
        require(SUCCEEDED(capture->ReleaseBuffer(frames)), "Release public microphone packet");
    }
    require(SUCCEEDED(client->Stop()), "Stop public microphone recording");
    require(recording.left.size() == 24000, "Public microphone recording timed out");
    return recording;
}
double amplitude(const std::vector<float>& samples, double hz) {
    double sine = 0, cosine = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto phase = 6.283185307179586 * hz * static_cast<double>(i) / 48000;
        sine += samples[i] * std::sin(phase);
        cosine += samples[i] * std::cos(phase);
    }
    return 2 * std::hypot(sine, cosine) / static_cast<double>(samples.size());
}
int positiveRouting(const wchar_t* microphone, const std::string& mixId, const wchar_t* otherFeed,
                    const wchar_t* recoveryFeed) {
    const auto defaults = audioDefaultEndpoints();
    const auto sink = observeAppRoutingSink(currentOwnerSid());
    require(!sink.endpoint.empty() && sink.endpointsExamined && sink.defaultsExamined &&
                !sink.defaultEndpoint && sink.error.empty(),
            "Positive routing requires a safe held account sink");
    Image image, otherImage;
    require(appRoutingSinkRootPresent(currentOwnerSid()), "Held internal PnP root is missing");
    const auto missingHistory = image.directory / L"must-not-rebase";
    bool refusedNewHistory = false;
    try {
        auto ignored =
            openAppRoutingStore(missingHistory, AppRoutingStorageIntent::InitializeForRequest);
    } catch (const std::exception&) {
        refusedNewHistory = true;
    }
    require(refusedNewHistory && !std::filesystem::exists(missingHistory),
            "A live internal endpoint was accepted without existing recovery history");
    auto first = std::make_unique<Child>(image.executable,
                                         L"--routed-source \"" + wide(sink.endpoint) + L"\" 440");
    auto second = std::make_unique<Child>(image.executable,
                                          L"--routed-source \"" + wide(sink.endpoint) + L"\" 660");
    Child outside(otherImage.executable, L"--tone \"" + std::wstring(otherFeed) + L"\" 1100");
    const auto initialDeadline = GetTickCount64() + 10000;
    const auto hasActive = [&](uint32_t pid, bool onSink) {
        bool found = false;
        for (const auto& session : appRoutingSessions())
            if (session.pid == pid && session.active) {
                if (onSink && session.endpoint != sink.endpoint)
                    return false;
                if (!onSink && session.endpoint == sink.endpoint)
                    return false;
                found = true;
            }
        return found;
    };
    while ((!hasActive(first->process.dwProcessId, false) ||
            !hasActive(second->process.dwProcessId, false)) &&
           GetTickCount64() < initialDeadline)
        Sleep(50);
    require(hasActive(first->process.dwProcessId, false) &&
                hasActive(second->process.dwProcessId, false),
            "Automatic silent sources did not start away from the internal sink");
    AppAudioPolicy policy;
    std::vector<std::unique_ptr<ProcessRoutingPreferences>> access;
    access.push_back(std::make_unique<ProcessRoutingPreferences>(policy, first->process.dwProcessId,
                                                                 image.executable));
    access.push_back(std::make_unique<ProcessRoutingPreferences>(
        policy, second->process.dwProcessId, image.executable, access.front()->application()));
    const auto original = access.front()->read();
    require(access.back()->read() == original, "Positive routing originals differ");
    RouteDocument document;
    VirtualDeviceDefinition device;
    device.id = mixId;
    device.name = "Positive routing fixture";
    device.routing.sources = {{"selected", 2}, {"alias", 2}};
    device.routing.sources.front().connections = {{0, 0, 1}, {1, 1, 1}};
    device.bindings = {{"selected", SourceKind::Application, access.front()->application(), true},
                       {"alias", SourceKind::Application, utf8(image.executable.native()), false}};
    document.devices.push_back(device);
    const auto storage = image.directory / L"routing.json";
    {
        std::ofstream file(storage);
        file << encodeDocument(document).dump();
        require(bool(file), "Save positive routing fixture");
    }
    JSON acquisition, wave, recovery, muteControl;
    {
        Runtime runtime(storage, false);
        AppRoutingStore store(image.directory / L"recovery");
        AppRoutingLease lease(store.journal());
        image.preserve = true;
        const auto silenceSources = [&] {
            {
                std::ofstream file(image.directory / L"silence");
                file << "silent";
                require(bool(file), "Silence routed sources");
            }
            const auto silent = [&](const auto& source) {
                return !source ||
                       std::filesystem::exists(
                           image.directory /
                           (L"silent-" + std::to_wstring(source->process.dwProcessId) + L".ready"));
            };
            const auto deadline = GetTickCount64() + 5000;
            while ((!silent(first) || !silent(second)) && GetTickCount64() < deadline)
                Sleep(20);
            require(silent(first) && silent(second),
                    "Routed sources did not clear nonzero buffers");
        };
        struct Guard {
            std::function<void()> action;
            ~Guard() {
                try {
                    action();
                } catch (...) {
                }
            }
        } guard{[&] {
            if (store.journal().snapshot().empty())
                return;
            try {
                silenceSources();
            } catch (...) {
                first.reset();
                second.reset();
                throw;
            }
            for (auto& member : access)
                lease.release(*member, AppRoutingReleaseMode::RetainVerified);
        }};
        const auto preparedDeadline = GetTickCount64() + 10000;
        bool ready = false;
        do {
            const auto state = runtime.request({{"version", 1}, {"operation", "state"}});
            unsigned prepared = 0;
            for (const auto& capture : state.at("snapshot").at("runtime").at("applicationCaptures"))
                if (capture.at("roots").size() == 2 && capture.at("ready") == true)
                    ++prepared;
            bool connected = false;
            for (const auto& row : state.at("snapshot").at("runtime").at("devices"))
                if (row.at("id") == mixId)
                    connected = row.at("microphone").value("connected", false);
            ready = prepared == 2 && connected;
            if (!ready)
                Sleep(50);
        } while (!ready && GetTickCount64() < preparedDeadline);
        require(ready, "Positive routing capture or publication was not ready");
        const auto routingDeadline = GetTickCount64() + 8000;
        bool routed = false;
        do {
            runtime.recoverApplicationOutputs(store.journal(), policy);
            acquisition = runtime.acquireApplicationOutputs(store.journal(), policy);
            for (const auto& row : acquisition.at("applications"))
                routed |= row.at("phase") == "routed";
            if (!routed)
                Sleep(100);
        } while (!routed && GetTickCount64() < routingDeadline);
        require(routed && hasActive(first->process.dwProcessId, true) &&
                    hasActive(second->process.dwProcessId, true),
                ("Automatic streams did not route: " + acquisition.dump()).c_str());
        const auto target = utf8(AppAudioPolicy::renderInterface(sink.endpoint));
        for (auto& member : access)
            require(member->read() == std::array<std::string, 3>{target, target, target},
                    "Routed roles differ");
        {
            std::ofstream permission(image.directory / L"begin-virtual-tone");
            permission << "explicit virtual only";
            require(bool(permission), "Authorize explicit virtual tones");
        }
        const auto toneDeadline = GetTickCount64() + 5000;
        const auto toneReady = [&](const auto& source) {
            return std::filesystem::exists(
                image.directory /
                (L"tone-" + std::to_wstring(source->process.dwProcessId) + L".ready"));
        };
        while ((!toneReady(first) || !toneReady(second)) && GetTickCount64() < toneDeadline)
            Sleep(20);
        require(toneReady(first) && toneReady(second), "Routed virtual tone sources did not start");
        const auto recorded = recordPublicMicrophone(microphone);
        wave = {{"frames", recorded.left.size()},
                {"discontinuities", recorded.discontinuities},
                {"timestampErrors", recorded.timestampErrors},
                {"silentFrames", recorded.silentFrames},
                {"left", JSON::object()},
                {"right", JSON::object()}};
        bool valid =
            !recorded.discontinuities && !recorded.timestampErrors && !recorded.silentFrames;
        for (const auto hz : {440, 660, 880, 1100}) {
            const auto l = amplitude(recorded.left, hz), r = amplitude(recorded.right, hz);
            wave["left"][std::to_string(hz)] = l;
            wave["right"][std::to_string(hz)] = r;
            valid &= hz == 440 || hz == 660 ? (l > .02 && l < .04 && r > .02 && r < .04)
                                            : (l < .005 && r < .005);
        }
        require(valid, ("Routed public waveform failed: " + wave.dump()).c_str());
        const auto heldRevision = store.journal().snapshot().at(0).revision;
        for (const bool muted : {true, false}) {
            const auto current = runtime.request({{"version", 1}, {"operation", "state"}});
            const auto answer = runtime.request(
                {{"version", 1},
                 {"operation", "edit"},
                 {"request",
                  {{"version", 1},
                   {"expected", current.at("snapshot").at("revision")},
                   {"commands", JSON::array({{{"type", "setSourceControls"},
                                              {"device", mixId},
                                              {"source", "selected"},
                                              {"controls", {{"muted", muted}}}}})}}}});
            require(answer.value("ok", false),
                    ("Ordinary source mute edit failed: " + answer.dump()).c_str());
            runtime.recoverApplicationOutputs(store.journal(), policy);
            const auto route = runtime.acquireApplicationOutputs(store.journal(), policy);
            require(route.at("applications").size() == 1 &&
                        route.at("applications")[0].at("phase") == "routed" &&
                        store.journal().snapshot().at(0).revision == heldRevision,
                    ("Mute edit released or reapplied original playback ownership: " + route.dump())
                        .c_str());
            for (auto& member : access)
                require(member->read() == std::array<std::string, 3>{target, target, target},
                        "Mute edit restored original output prematurely");
            const auto samples = recordPublicMicrophone(microphone);
            double peak = 0;
            for (size_t i = 0; i < samples.left.size(); ++i)
                peak = std::max({peak, std::abs(static_cast<double>(samples.left[i])),
                                 std::abs(static_cast<double>(samples.right[i]))});
            const auto a440 = amplitude(samples.left, 440), a660 = amplitude(samples.left, 660);
            require(samples.discontinuities == 0 && samples.timestampErrors == 0 &&
                        (muted ? peak < .000001
                               : (a440 > .02 && a440 < .04 && a660 > .02 && a660 < .04)),
                    ("Mute/unmute waveform failed: " +
                     JSON{{"muted", muted}, {"peak", peak}, {"440", a440}, {"660", a660}}.dump())
                        .c_str());
            muteControl[muted ? "muted" : "unmuted"] = {
                {"frames", samples.left.size()},
                {"peak", peak},
                {"440", a440},
                {"660", a660},
                {"discontinuities", samples.discontinuities},
                {"timestampErrors", samples.timestampErrors},
                {"receiptRevisionUnchanged", true}};
        }
        silenceSources();
        const auto state = runtime.request({{"version", 1}, {"operation", "state"}});
        const auto edited =
            runtime.request({{"version", 1},
                             {"operation", "edit"},
                             {"request",
                              {{"version", 1},
                               {"expected", state.at("snapshot").at("revision")},
                               {"commands", JSON::array({{{"type", "setSourceControls"},
                                                          {"device", mixId},
                                                          {"source", "selected"},
                                                          {"controls", JSON::object()},
                                                          {"muteOriginal", false}}})}}}});
        require(edited.value("ok", false), "Save positive routing deselection");
        recovery = runtime.recoverApplicationOutputs(store.journal(), policy);
        for (auto& member : access)
            require(member->read() == original, "Routed original output not restored");
        first.reset();
        second.reset();
        Child relaunched(image.executable, L"--tone \"" + std::wstring(recoveryFeed) + L"\" 880");
        const auto recoveryDeadline = GetTickCount64() + 8000;
        do {
            recovery = runtime.recoverApplicationOutputs(store.journal(), policy);
            if (store.journal().snapshot().empty())
                break;
            Sleep(100);
        } while (GetTickCount64() < recoveryDeadline);
        require(store.journal().snapshot().empty(),
                ("Positive routing recovery pending: " + recovery.dump()).c_str());
        image.preserve = false;
    }
    require(audioDefaultEndpoints() == defaults, "Positive routing changed defaults");
    std::cout << JSON{{"ok", true},
                      {"probe", "automatic-routing-positive"},
                      {"selectedProcesses", 2},
                      {"automaticSilentStreamsMoved", true},
                      {"missingHistoryWithExistingRootRefused", true},
                      {"explicitVirtualToneAfterMovement", true},
                      {"acquisition", acquisition},
                      {"publicMicrophone", wave},
                      {"muteControl", muteControl},
                      {"originalRolesRestored", true},
                      {"receiptReleased", true},
                      {"recovery", recovery},
                      {"defaultsPreserved", true}}
                     .dump()
              << '\n';
    return 0;
}
int installedRouting(const std::string& mixId, const wchar_t* otherFeed,
                     const wchar_t* recoveryFeed, const wchar_t* microphone) {
    wchar_t optIn[2]{};
    require(GetEnvironmentVariableW(L"LOOPZ_INSTALLED_ROUTING_ACCEPTANCE", optIn, 2) == 1 &&
                optIn[0] == L'1',
            "Installed routing requires explicit opt-in");
    const auto defaults = audioDefaultEndpoints();
    require(!appRoutingSinkRootPresent(currentOwnerSid()),
            "Installed routing fixture requires no pre-existing account sink");
    const bool historyExisted = std::filesystem::exists(appRoutingStoreRoot());
    const auto state = [] {
        return engineRequest({{"version", 1}, {"operation", "state"}});
    };
    JSON last;
    const auto await = [&](const auto& predicate, unsigned milliseconds, const char* message) {
        const auto until = GetTickCount64() + milliseconds;
        do {
            last = state();
            if (predicate(last))
                return;
            Sleep(100);
        } while (GetTickCount64() < until);
        throw std::runtime_error(std::string(message) + ": " + last.dump());
    };
    const auto edit = [&](const JSON& commands) {
        const auto current = state();
        const auto reply = engineRequest({{"version", 1},
                                          {"operation", "edit"},
                                          {"request",
                                           {{"version", 1},
                                            {"expected", current.at("snapshot").at("revision")},
                                            {"commands", commands}}}});
        require(reply.value("ok", false), ("Installed edit failed: " + reply.dump()).c_str());
    };
    const auto before = state();
    require(
        before.at("snapshot").at("runtime").at("editCapabilities").value("appOutputRouting", false),
        "Installed app lacks playback capability");
    const auto baseline = before.at("snapshot").at("document");
    for (const auto& device : baseline.at("devices")) {
        require(device.at("monitors").empty(), "Installed fixture refuses hardware monitors");
        for (const auto& binding : device.at("bindings"))
            require(!binding.value("muteOriginal", false),
                    "Installed fixture refuses an existing playback-routing request");
    }
    Image image, excludedImage;
    image.preserve = true;
    auto first = std::make_unique<Child>(image.executable, L"--await-routed-source 440");
    auto second = std::make_unique<Child>(image.executable, L"--await-routed-source 660");
    Child excluded(excludedImage.executable, L"--tone \"" + std::wstring(otherFeed) + L"\" 1100");
    const auto activeAt = [&](uint32_t pid, const std::string& endpoint, bool exact) {
        bool active = false;
        for (const auto& session : appRoutingSessions())
            if (session.pid == pid && session.active) {
                if (exact && session.endpoint != endpoint)
                    return false;
                active = true;
            }
        return active;
    };
    await(
        [&](const JSON&) {
            return activeAt(first->process.dwProcessId, {}, false) &&
                   activeAt(second->process.dwProcessId, {}, false);
        },
        10000, "Installed sources did not start silently");
    AppAudioPolicy policy;
    ProcessRoutingPreferences firstAccess(policy, first->process.dwProcessId, image.executable);
    const auto application = firstAccess.application();
    ProcessRoutingPreferences secondAccess(policy, second->process.dwProcessId, image.executable,
                                           application);
    const auto original = firstAccess.read();
    require(secondAccess.read() == original, "Installed source originals differ");
    bool sourceAdded = false, tonesStarted = false;
    JSON acquisition, wave, mutedWave, unmutedWave, recovery;
    std::string sinkEndpoint;
    const auto cleanup = [&] {
        if (!sourceAdded) {
            first.reset();
            second.reset();
            image.preserve = false;
            return;
        }
        {
            std::ofstream silent(image.directory / L"silence");
            silent << "silent";
            require(bool(silent), "Silence installed sources");
        }
        if (tonesStarted) {
            const auto acknowledged = [&](const auto& child) {
                return !child ||
                       std::filesystem::exists(
                           image.directory /
                           (L"silent-" + std::to_wstring(child->process.dwProcessId) + L".ready"));
            };
            await(
                [&](const JSON&) {
                    return acknowledged(first) && acknowledged(second);
                },
                5000, "Installed sources did not drain nonzero buffers");
        }
        edit(JSON::array({{{"type", "setSourceControls"},
                           {"device", mixId},
                           {"source", "installed-selected"},
                           {"controls", JSON::object()},
                           {"muteOriginal", false}}}));
        await(
            [&](const JSON&) {
                const bool firstRestored =
                    !first || WaitForSingleObject(first->process.hProcess, 0) != WAIT_TIMEOUT ||
                    firstAccess.read() == original;
                const bool secondRestored =
                    !second || WaitForSingleObject(second->process.hProcess, 0) != WAIT_TIMEOUT ||
                    secondAccess.read() == original;
                return firstRestored && secondRestored;
            },
            15000, "Installed engine did not restore original output roles");
        first.reset();
        second.reset();
        auto relaunched = std::make_unique<Child>(
            image.executable, L"--tone \"" + std::wstring(recoveryFeed) + L"\" 880");
        ProcessRoutingPreferences restored(policy, relaunched->process.dwProcessId,
                                           image.executable, application);
        await(
            [&](const JSON& current) {
                recovery = current.at("applicationRoutingRecovery");
                return recovery.value("examined", false) &&
                       recovery.value("pending", size_t{1}) == 0 && restored.read() == original;
            },
            20000, "Installed engine left a pending playback receipt");
        edit(JSON::array(
            {{{"type", "removeSource"}, {"device", mixId}, {"source", "installed-selected"}}}));
        sourceAdded = false;
        relaunched.reset();
        await(
            [&](const JSON&) {
                return !appRoutingSinkRootPresent(currentOwnerSid());
            },
            30000, "Installed broker did not remove the released internal sink");
        require(state().at("snapshot").at("document") == baseline,
                "Installed native fixture changed its baseline document");
        image.preserve = false;
    };
    try {
        edit(JSON::array({{{"type", "addSource"},
                           {"device", mixId},
                           {"id", "installed-selected"},
                           {"kind", "application"},
                           {"selector", utf8(image.executable.native())},
                           {"channels", 2},
                           {"muteOriginal", false}}}));
        sourceAdded = true;
        edit(JSON::array(
            {{{"type", "setSourceWiring"},
              {"device", mixId},
              {"source", "installed-selected"},
              {"connections",
               JSON::array({{{"sourceChannel", 0}, {"outputChannel", 0}, {"gain", 1.0}},
                            {{"sourceChannel", 1}, {"outputChannel", 1}, {"gain", 1.0}}})}}}));
        edit(JSON::array({{{"type", "setSourceControls"},
                           {"device", mixId},
                           {"source", "installed-selected"},
                           {"controls", JSON::object()},
                           {"muteOriginal", true}}}));
        await(
            [&](const JSON& current) {
                acquisition = current.at("applicationRoutingRecovery");
                const auto sink = observeAppRoutingSink(currentOwnerSid());
                if (sink.endpoint.empty())
                    return false;
                for (const auto& source : current.at("snapshot").at("runtime").at("sources"))
                    if (source.at("selector") == utf8(image.executable.native()) &&
                        source.value("playbackRouting", JSON::object()).value("verified", false) &&
                        activeAt(first->process.dwProcessId, sink.endpoint, true) &&
                        activeAt(second->process.dwProcessId, sink.endpoint, true)) {
                        sinkEndpoint = sink.endpoint;
                        return true;
                    }
                return false;
            },
            45000, "Installed routing did not confirm both automatic streams");
        const auto target = utf8(AppAudioPolicy::renderInterface(sinkEndpoint));
        require(firstAccess.read() == std::array<std::string, 3>{target, target, target} &&
                    secondAccess.read() == firstAccess.read(),
                "Installed routed roles differ");
        require(std::filesystem::exists(appRoutingStoreRoot() / L"routing.sqlite3"),
                "Installed routing did not create durable recovery history");
        {
            std::ofstream endpoint(image.directory / L"virtual-endpoint");
            endpoint << sinkEndpoint;
            require(bool(endpoint), "Publish exact virtual endpoint");
        }
        {
            std::ofstream permission(image.directory / L"begin-virtual-tone");
            permission << "virtual only";
            require(bool(permission), "Authorize installed virtual-only markers");
        }
        tonesStarted = true;
        await(
            [&](const JSON&) {
                return std::filesystem::exists(
                           image.directory /
                           (L"tone-" + std::to_wstring(first->process.dwProcessId) + L".ready")) &&
                       std::filesystem::exists(
                           image.directory /
                           (L"tone-" + std::to_wstring(second->process.dwProcessId) + L".ready"));
            },
            5000, "Installed virtual tones did not start");
        const auto record = [&](bool muted) {
            const auto samples = recordPublicMicrophone(microphone);
            JSON result = {{"frames", samples.left.size()},
                           {"discontinuities", samples.discontinuities},
                           {"timestampErrors", samples.timestampErrors},
                           {"silentFrames", samples.silentFrames}};
            double peak = 0;
            for (size_t i = 0; i < samples.left.size(); ++i)
                peak = std::max(
                    {peak, std::abs(double(samples.left[i])), std::abs(double(samples.right[i]))});
            result["peak"] = peak;
            bool valid = !samples.discontinuities && !samples.timestampErrors;
            for (const auto hz : {440, 660, 880, 1100}) {
                const auto left = amplitude(samples.left, hz), right = amplitude(samples.right, hz);
                result["left"][std::to_string(hz)] = left;
                result["right"][std::to_string(hz)] = right;
                valid &= muted ? peak < .000001
                               : ((hz == 440 || hz == 660)
                                      ? left > .02 && left < .04 && right > .02 && right < .04
                                      : left < .005 && right < .005);
            }
            require(valid, ("Installed public waveform failed: " + result.dump()).c_str());
            return result;
        };
        wave = record(false);
        for (bool muted : {true, false}) {
            edit(JSON::array({{{"type", "setSourceControls"},
                               {"device", mixId},
                               {"source", "installed-selected"},
                               {"controls", {{"muted", muted}}}}}));
            auto samples = record(muted);
            require(firstAccess.read() == std::array<std::string, 3>{target, target, target} &&
                        secondAccess.read() == firstAccess.read(),
                    "Installed mute released output routing");
            (muted ? mutedWave : unmutedWave) = std::move(samples);
        }
        cleanup();
    } catch (const std::exception& error) {
        const std::string cause = error.what();
        {
            JSON failure = {{"error", cause},
                            {"state", last},
                            {"publicMicrophone", wave},
                            {"muteControl", {{"muted", mutedWave}, {"unmuted", unmutedWave}}}};
            for (const auto* source : {first.get(), second.get()})
                if (source) {
                    DWORD code{};
                    GetExitCodeProcess(source->process.hProcess, &code);
                    failure["processes"].push_back(
                        {{"pid", source->process.dwProcessId}, {"exitCode", code}});
                }
            std::ofstream file(image.directory / L"installed-failure.json");
            file << failure.dump(2);
        }
        try {
            cleanup();
        } catch (const std::exception& cleanupError) {
            throw std::runtime_error(cause + "; cleanup pending at " +
                                     utf8(image.directory.native()) + ": " + cleanupError.what());
        }
        throw std::runtime_error(cause);
    }
    require(audioDefaultEndpoints() == defaults, "Installed fixture changed system defaults");
    std::cout << JSON{{"ok", true},
                      {"probe", "installed-routing"},
                      {"selectedProcesses", 2},
                      {"historyExistedBefore", historyExisted},
                      {"durableHistoryCreated", true},
                      {"automaticSilentStreamsMoved", true},
                      {"explicitVirtualToneAfterMovement", true},
                      {"acquisition", acquisition},
                      {"publicMicrophone", wave},
                      {"muteControl", {{"muted", mutedWave}, {"unmuted", unmutedWave}}},
                      {"originalRolesRestored", true},
                      {"receiptReleased", true},
                      {"recovery", recovery},
                      {"internalSinkRemoved", true},
                      {"defaultsPreserved", true}}
                     .dump()
              << '\n';
    return 0;
}

} // namespace
int run(int argc, wchar_t** argv) {
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
            "Initialize group fixture MTA");
    struct Apartment {
        ~Apartment() {
            CoUninitialize();
        }
    } apartment;
    if (argc == 6 && std::wstring_view(argv[1]) == L"--installed-route")
        return installedRouting(utf8(argv[2]), argv[3], argv[4], argv[5]);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--await-routed-source") {
        wchar_t module[32768]{};
        require(GetModuleFileNameW(nullptr, module, 32768) != 0,
                "Locate installed source controls");
        const auto directory = std::filesystem::path(module).parent_path();
        const auto permission = directory / L"begin-virtual-tone";
        probe::automaticSilenceUntil(
            60,
            [](const void* value) {
                return std::filesystem::exists(*static_cast<const std::filesystem::path*>(value));
            },
            &permission);
        require(std::filesystem::exists(permission), "Installed source timed out while silent");
        std::ifstream file(directory / L"virtual-endpoint");
        std::string endpoint;
        std::getline(file, endpoint);
        const auto known = loopzEndpoints();
        require(known.contains(endpoint) && known.at(endpoint).role == EndpointRole::Feed &&
                    known.at(endpoint).mixKey == driverAppRoutingSinkKey(currentOwnerSid()),
                "Installed tone requires the exact current-account virtual sink");
        return tone(wide(endpoint), std::stod(argv[2]));
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--recover-fixture") {
        wchar_t optIn[2]{};
        require(GetEnvironmentVariableW(L"LOOPZ_GROUP_ROUTING_ACCEPTANCE", optIn, 2) == 1 &&
                    optIn[0] == L'1',
                "Recovery fixture requires explicit opt-in");
        const auto directory = std::filesystem::canonical(argv[2]);
        require(directory.parent_path() ==
                        std::filesystem::canonical(std::filesystem::temp_directory_path()) &&
                    directory.filename().native().starts_with(L"LoopZ-route-") &&
                    std::filesystem::exists(directory / L"silence") &&
                    std::filesystem::exists(directory / L"recovery" / L"routing.sqlite3"),
                "Recovery requires a retained, silenced fixture under this user's temp directory");
        const auto defaults = audioDefaultEndpoints();
        const auto endpoint = utf8(argv[3]);
        const auto verifiedEndpoints = loopzEndpoints();
        require(verifiedEndpoints.contains(endpoint) &&
                    verifiedEndpoints.at(endpoint).role == EndpointRole::Feed,
                "Recovery marker requires an explicit hardware-identified virtual Feed");
        JSON recovery;
        {
            const auto executable = directory / L"marker.exe";
            Child marker(executable, L"--tone \"" + std::wstring(argv[3]) + L"\" 880");
            const auto deadline = GetTickCount64() + 8000;
            bool active = false;
            while (!active && GetTickCount64() < deadline) {
                for (const auto& session : appRoutingSessions())
                    active |= session.pid == marker.process.dwProcessId && session.active &&
                              session.endpoint == endpoint;
                if (!active)
                    Sleep(50);
            }
            require(active, "Retained recovery marker did not start silently");
            AppRoutingStore store(directory / L"recovery", false);
            const auto receipt = store.journal().snapshot();
            require(receipt.size() == 1, "Recovery fixture requires exactly one retained receipt");
            AppAudioPolicy policy;
            ProcessRoutingPreferences preferences(policy, marker.process.dwProcessId, executable,
                                                  receipt[0].application);
            Runtime runtime(directory / L"routing.json", false);
            const auto until = GetTickCount64() + 8000;
            do {
                recovery = runtime.recoverApplicationOutputs(store.journal(), policy);
                if (store.journal().snapshot().empty())
                    break;
                Sleep(100);
            } while (GetTickCount64() < until);
            require(store.journal().snapshot().empty() && preferences.read() == receipt[0].original,
                    ("Retained fixture recovery remains pending: " + recovery.dump()).c_str());
        }
        require(audioDefaultEndpoints() == defaults, "Retained fixture recovery changed defaults");
        std::filesystem::remove_all(directory);
        require(!std::filesystem::exists(directory), "Retained fixture directory remains");
        std::cout << JSON{{"ok", true},
                          {"probe", "retained-fixture-recovery"},
                          {"receiptReleased", true},
                          {"directoryRemoved", true},
                          {"recovery", recovery}}
                         .dump()
                  << '\n';
        return 0;
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--routed-source") {
        const auto uid = utf8(argv[2]);
        const auto known = loopzEndpoints();
        require(known.contains(uid) && known.at(uid).role == EndpointRole::Feed &&
                    known.at(uid).mixKey == driverAppRoutingSinkKey(currentOwnerSid()),
                "Routed source requires this account's internal virtual endpoint");
        wchar_t module[32768]{};
        require(GetModuleFileNameW(nullptr, module, 32768) != 0, "Locate routed source controls");
        const auto permission = std::filesystem::path(module).parent_path() / L"begin-virtual-tone";
        probe::automaticSilenceUntil(
            60,
            [](const void* value) {
                return std::filesystem::exists(*static_cast<const std::filesystem::path*>(value));
            },
            &permission);
        require(std::filesystem::exists(permission), "Routed source timed out while silent");
        return tone(argv[2], std::stod(argv[3])); // Explicit verified virtual endpoint only.
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--tone")
        return tone(argv[2], std::stod(argv[3]));
    if (argc == 4 && (std::wstring_view(argv[1]) == L"--tree" ||
                      std::wstring_view(argv[1]) == L"--surviving-tree")) {
        wchar_t module[32768]{};
        require(GetModuleFileNameW(nullptr, module, 32768) != 0, "Locate tree source");
        Child descendant(module, L"--tone \"" + std::wstring(argv[2]) + L"\" 660",
                         std::wstring_view(argv[1]) == L"--tree");
        {
            std::ofstream file{std::filesystem::path(argv[3])};
            file << descendant.process.dwProcessId;
            require(bool(file), "Publish descendant PID");
        }
        return tone(argv[2], 440);
    }
    wchar_t flag[2]{};
    require(argc == 8 && GetEnvironmentVariableW(L"LOOPZ_GROUP_CAPTURE_ACCEPTANCE", flag, 2) == 1 &&
                flag[0] == L'1',
            "Requires explicit group acceptance opt-in and three owned virtual Feeds plus a public "
            "microphone");
    wchar_t routingFlag[2]{};
    const bool testRouting =
        GetEnvironmentVariableW(L"LOOPZ_GROUP_ROUTING_ACCEPTANCE", routingFlag, 2) == 1 &&
        routingFlag[0] == L'1';
    wchar_t parentExitFlag[2]{};
    const bool testParentExit =
        GetEnvironmentVariableW(L"LOOPZ_GROUP_PARENT_EXIT_ACCEPTANCE", parentExitFlag, 2) == 1 &&
        parentExitFlag[0] == L'1';
    require(!testParentExit || !testRouting,
            "Surviving-child fixture requires capture-only group mode");
    const auto first = utf8(argv[1]), second = utf8(argv[3]);
    const auto endpoints = loopzEndpoints();
    require(endpoints.contains(first) && endpoints.contains(second) &&
                endpoints.at(first).role == EndpointRole::Feed &&
                endpoints.at(second).role == EndpointRole::Feed &&
                endpoints.at(first).mixKey == driverMixKey(currentOwnerSid(), utf8(argv[2])) &&
                endpoints.at(second).mixKey == driverMixKey(currentOwnerSid(), utf8(argv[4])) &&
                first != second,
            "Requires distinct account-owned virtual Feeds");
    const auto third = utf8(argv[5]), microphone = utf8(argv[7]);
    const auto runtimeKey = driverMixKey(currentOwnerSid(), utf8(argv[6]));
    require(endpoints.contains(third) && endpoints.contains(microphone) &&
                endpoints.at(third).role == EndpointRole::Feed &&
                endpoints.at(microphone).role == EndpointRole::Microphone &&
                endpoints.at(third).mixKey == runtimeKey &&
                endpoints.at(microphone).mixKey == runtimeKey && third != first && third != second,
            "Requires a separate account-owned Runtime publication device");
    const auto defaults = audioDefaultEndpoints();
    wchar_t positiveFlag[2]{};
    if (GetEnvironmentVariableW(L"LOOPZ_GROUP_ROUTING_POSITIVE", positiveFlag, 2) == 1 &&
        positiveFlag[0] == L'1')
        return positiveRouting(argv[7], utf8(argv[6]), argv[1], argv[3]);
    Image image, otherImage;
    const auto pidFile = image.directory / L"descendant.pid";
    Child branch(image.executable, std::wstring(testParentExit ? L"--surviving-tree" : L"--tree") +
                                       L" \"" + std::wstring(argv[1]) + L"\" \"" +
                                       pidFile.wstring() + L"\"");
    auto independent =
        std::make_unique<Child>(image.executable, L"--tone \"" + std::wstring(argv[3]) + L"\" 880");
    Child outside(otherImage.executable, L"--tone \"" + std::wstring(argv[1]) + L"\" 1100");
    uint32_t descendant{};
    const auto deadline = GetTickCount64() + 5000;
    do {
        {
            std::ifstream file(pidFile);
            file >> descendant;
        }
        if (descendant)
            break;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    require(descendant != 0, "Missing source descendant");
    const std::array<std::filesystem::path, 4> readyFiles{
        image.directory / (L"tone-" + std::to_wstring(branch.process.dwProcessId) + L".ready"),
        image.directory / (L"tone-" + std::to_wstring(descendant) + L".ready"),
        image.directory /
            (L"tone-" + std::to_wstring(independent->process.dwProcessId) + L".ready"),
        otherImage.directory /
            (L"tone-" + std::to_wstring(outside.process.dwProcessId) + L".ready")};
    const auto producersDeadline = GetTickCount64() + 10000;
    while (std::any_of(readyFiles.begin(), readyFiles.end(),
                       [](const auto& path) {
                           return !std::filesystem::exists(path);
                       }) &&
           GetTickCount64() < producersDeadline)
        Sleep(20);
    require(std::all_of(readyFiles.begin(), readyFiles.end(),
                        [](const auto& path) {
                            return std::filesystem::exists(path);
                        }),
            "Not all virtual source streams started");

    std::vector<std::shared_ptr<AppRoutingProcess>> members{
        std::make_shared<AppRoutingProcess>(branch.process.dwProcessId, image.executable),
        std::make_shared<AppRoutingProcess>(descendant, image.executable),
        std::make_shared<AppRoutingProcess>(independent->process.dwProcessId, image.executable)};
    auto group = std::make_unique<ProcessCaptureGroup>(members);
    require(group->roots().size() == 2, "Nested child captured twice or independent instance lost");
    const auto readyDeadline = GetTickCount64() + 5000;
    while (!group->readyToRoute() && GetTickCount64() < readyDeadline)
        Sleep(20);
    require(group->readyToRoute(), "Capture group never published valid history");
    Sleep(500);
    auto reader = group->reader();
    AudioReadCursor cursor;
    std::array<float, 256> left{}, right{};
    const std::array<float*, 2> planes{left.data(), right.data()};
    std::vector<float> samples;
    const auto start = nowNs() - 200000000ULL;
    uint64_t missing = 0;
    for (uint32_t block = 0; block < 24; ++block) {
        const auto result =
            reader->read(start + static_cast<uint64_t>(block) * 256 * 1000000000ULL / 48000, 48000,
                         {planes, 256}, 256, cursor);
        require(result.online, "Capture group offline");
        missing += result.missingFrames;
        samples.insert(samples.end(), left.begin(), left.end());
    }
    const auto a = amplitude(samples, 440), b = amplitude(samples, 660),
               c = amplitude(samples, 880), excluded = amplitude(samples, 1100);
    require(
        missing == 0 && a > .02 && a < .04 && b > .02 && b < .04 && c > .02 && c < .04 &&
            excluded < .005,
        ("Group waveform lost, duplicated or included another application's signal: " +
         JSON{{"missing", missing}, {"440", a}, {"660", b}, {"880", c}, {"excluded1100", excluded}}
             .dump())
            .c_str());
    // One serialized capture owner: close the probe before Runtime acquires
    // these same targets. Histories are not a second native capture owner.
    reader.reset();
    group.reset();
    RouteDocument document;
    VirtualDeviceDefinition device;
    device.id = utf8(argv[6]);
    device.name = "Group fixture";
    device.routing.sources = {{"selected", 2}, {"legacy-alias", 2}};
    device.routing.sources.front().connections = {{0, 0, 1}, {1, 1, 1}};
    device.bindings = {
        {"selected", SourceKind::Application, members.front()->identity().application, false},
        {"legacy-alias", SourceKind::Application, utf8(image.executable.native()), false}};
    device.bindings.front().muteOriginal = testRouting;
    document.devices.push_back(device);
    const auto storage = image.directory / L"routing.json";
    {
        std::ofstream file(storage);
        file << encodeDocument(document).dump();
        require(bool(file), "Save group document");
    }
    JSON publicResult, routingResult, membershipResults = JSON::array();
    {
        Runtime runtime(storage, false);
        const auto until = GetTickCount64() + 5000;
        bool matched = false;
        JSON lastState;
        do {
            const auto state = runtime.request(JSON{{"version", 1}, {"operation", "state"}});
            lastState = state;
            unsigned readyAliases = 0;
            for (const auto& capture : state.at("snapshot").at("runtime").at("applicationCaptures"))
                if ((capture.at("selector") == members.front()->identity().application ||
                     capture.at("selector") == utf8(image.executable.native())) &&
                    capture.at("roots").size() == 2 && capture.at("ready") == true)
                    ++readyAliases;
            bool publicationReady = false;
            for (const auto& row : state.at("snapshot").at("runtime").at("devices"))
                if (row.at("id") == device.id)
                    publicationReady = row.at("microphone").value("connected", false);
            matched = readyAliases == 2 && publicationReady;
            if (matched)
                break;
            Sleep(50);
        } while (GetTickCount64() < until);
        if (!matched)
            throw std::runtime_error(
                "Runtime did not prepare the complete canonical application group: " +
                JSON{{"waveform",
                      {{"missing", missing},
                       {"440", a},
                       {"660", b},
                       {"880", c},
                       {"excluded1100", excluded}}},
                     {"runtime", lastState.at("snapshot").at("runtime")},
                     {"diagnostics", lastState.value("diagnostics", JSON{})}}
                    .dump());
        const auto recording = recordPublicMicrophone(argv[7]);
        publicResult = {{"frames", recording.left.size()},
                        {"discontinuities", recording.discontinuities},
                        {"timestampErrors", recording.timestampErrors},
                        {"silentFrames", recording.silentFrames},
                        {"left", JSON::object()},
                        {"right", JSON::object()}};
        bool valid = recording.discontinuities == 0 && recording.timestampErrors == 0 &&
                     recording.silentFrames == 0;
        for (const auto hz : {440, 660, 880, 1100}) {
            const auto l = amplitude(recording.left, hz), r = amplitude(recording.right, hz);
            publicResult["left"][std::to_string(hz)] = l;
            publicResult["right"][std::to_string(hz)] = r;
            valid &=
                hz == 1100 ? (l < .005 && r < .005) : (l > .02 && l < .04 && r > .02 && r < .04);
        }
        require(valid,
                ("Runtime public microphone waveform failed: " + publicResult.dump()).c_str());
        if (testRouting) {
            // Every selected marker acknowledges silence through its directory
            // before an OS preference can restore a hardware/default output.
            {
                std::ofstream silence(image.directory / L"silence");
                silence << "silent";
                require(bool(silence), "Silence routing markers");
            }
            const auto silenceDeadline = GetTickCount64() + 5000;
            const auto allSilent = [&] {
                return std::all_of(members.begin(), members.end(), [&](const auto& member) {
                    return std::filesystem::exists(
                        image.directory /
                        (L"silent-" + std::to_wstring(member->identity().pid) + L".ready"));
                });
            };
            while (!allSilent() && GetTickCount64() < silenceDeadline)
                Sleep(20);
            require(allSilent(), "Selected marker streams did not acknowledge silence");
            AppAudioPolicy policy;
            AppRoutingStore store(image.directory / L"recovery");
            AppRoutingLease lease(store.journal());
            std::vector<std::unique_ptr<ProcessRoutingPreferences>> access;
            std::vector<std::array<std::string, 3>> originals;
            for (const auto& member : members) {
                access.push_back(std::make_unique<ProcessRoutingPreferences>(
                    policy, member->identity().pid, image.executable,
                    member->identity().application));
                originals.push_back(access.back()->read());
            }
            image.preserve = true; // A failed policy test must retain its durable receipt.
            struct Restore {
                AppRoutingLease& lease;
                std::vector<std::unique_ptr<ProcessRoutingPreferences>>& access;
                ~Restore() {
                    for (auto& member : access)
                        try {
                            lease.release(*member, AppRoutingReleaseMode::RetainVerified);
                        } catch (...) {
                        }
                }
            } restore{lease, access};
            const auto sink = observeAppRoutingSink(currentOwnerSid());
            require(!sink.endpoint.empty() && sink.error.empty(),
                    "Held account sink is unavailable");
            const auto target = utf8(AppAudioPolicy::renderInterface(sink.endpoint));
            routingResult["first"] = runtime.acquireApplicationOutputs(store.journal(), policy);
            require(store.journal().snapshot().size() == 1 &&
                        store.journal().snapshot()[0].phase == AppRoutingReceiptPhase::Applied,
                    ("Controller did not durably acquire selected app: " + routingResult.dump())
                        .c_str());
            for (auto& member : access)
                require(member->read() == std::array<std::string, 3>{target, target, target},
                        "Controller missed selected process roles");
            routingResult["allSelectedRolesVerified"] = true;
            const auto routingDeadline = GetTickCount64() + 18000;
            std::string phase;
            do {
                runtime.recoverApplicationOutputs(store.journal(), policy);
                routingResult["last"] = runtime.acquireApplicationOutputs(store.journal(), policy);
                const auto& rows = routingResult["last"].at("applications");
                require(!rows.empty(), "Missing controller application state");
                phase = rows[0].at("phase");
                if (phase == "routed" || phase == "restoring-after-timeout")
                    break;
                Sleep(100);
            } while (GetTickCount64() < routingDeadline);
            require(phase == "routed" || phase == "restoring-after-timeout",
                    ("No terminal routing decision: " + routingResult.dump()).c_str());
            routingResult["decision"] = phase;
            const auto state = runtime.request({{"version", 1}, {"operation", "state"}});
            const auto edited =
                runtime.request({{"version", 1},
                                 {"operation", "edit"},
                                 {"request",
                                  {{"version", 1},
                                   {"expected", state.at("snapshot").at("revision")},
                                   {"commands", JSON::array({{{"type", "setSourceControls"},
                                                              {"device", device.id},
                                                              {"source", "selected"},
                                                              {"controls", JSON::object()},
                                                              {"muteOriginal", false}}})}}}});
            require(edited.value("ok", false), "Deselect owned application output");
            for (auto& member : access)
                lease.release(*member, AppRoutingReleaseMode::RetainVerified);
            for (size_t i = 0; i < access.size(); ++i)
                require(access[i]->read() == originals[i],
                        "Controller failed to restore original roles");
            routingResult["originalRolesRestored"] = true;
            require(TerminateJobObject(branch.job, 0) &&
                        WaitForSingleObject(branch.process.hProcess, 5000) == WAIT_OBJECT_0,
                    "Close owned branch after restoration");
            independent.reset();
            Child relaunched(image.executable, L"--tone \"" + std::wstring(argv[3]) + L"\" 880");
            const auto recoveryDeadline = GetTickCount64() + 8000;
            do {
                routingResult["recovery"] =
                    runtime.recoverApplicationOutputs(store.journal(), policy);
                if (store.journal().snapshot().empty())
                    break;
                Sleep(100);
            } while (GetTickCount64() < recoveryDeadline);
            require(store.journal().snapshot().empty(),
                    ("Controller receipt remains: " + routingResult.dump()).c_str());
            routingResult["receiptReleased"] = true;
            image.preserve = false;
        } else {
            const auto verifyMembership = [&](std::vector<uint32_t> expectedRoots,
                                              std::vector<int> expectedTones,
                                              const char* transition) {
                std::sort(expectedRoots.begin(), expectedRoots.end());
                const auto deadline = GetTickCount64() + 10000;
                bool ready = false;
                JSON last;
                do {
                    runtime.recoverAudio();
                    last = runtime.request({{"version", 1}, {"operation", "state"}});
                    unsigned aliases = 0;
                    for (const auto& capture :
                         last.at("snapshot").at("runtime").at("applicationCaptures")) {
                        if (capture.at("selector") != members.front()->identity().application &&
                            capture.at("selector") != utf8(image.executable.native()))
                            continue;
                        std::vector<uint32_t> actual;
                        for (const auto& root : capture.at("roots"))
                            actual.push_back(root.at("pid"));
                        std::sort(actual.begin(), actual.end());
                        if (actual == expectedRoots && capture.at("ready") == true)
                            ++aliases;
                    }
                    ready = aliases == 2;
                    if (ready)
                        break;
                    Sleep(50);
                } while (GetTickCount64() < deadline);
                require(ready, (std::string("Membership refresh failed: ") + transition + " " +
                                last.dump())
                                   .c_str());
                const auto wave = recordPublicMicrophone(argv[7]);
                JSON result{{"transition", transition},
                            {"roots", expectedRoots},
                            {"frames", wave.left.size()},
                            {"discontinuities", wave.discontinuities},
                            {"timestampErrors", wave.timestampErrors},
                            {"silentFrames", wave.silentFrames},
                            {"left", JSON::object()},
                            {"right", JSON::object()}};
                bool valid = wave.discontinuities == 0 && wave.timestampErrors == 0 &&
                             wave.silentFrames == 0;
                for (const auto hz : {440, 660, 880, 1100, 1320}) {
                    const auto l = amplitude(wave.left, hz), r = amplitude(wave.right, hz);
                    result["left"][std::to_string(hz)] = l;
                    result["right"][std::to_string(hz)] = r;
                    const bool expected = std::find(expectedTones.begin(), expectedTones.end(),
                                                    hz) != expectedTones.end();
                    valid &= expected ? (l > .02 && l < .04 && r > .02 && r < .04)
                                      : (l < .005 && r < .005);
                }
                require(valid, ("Membership public waveform failed: " + result.dump()).c_str());
                membershipResults.push_back(std::move(result));
            };
            Child added(image.executable, L"--tone \"" + std::wstring(argv[3]) + L"\" 1320");
            verifyMembership({branch.process.dwProcessId, independent->process.dwProcessId,
                              added.process.dwProcessId},
                             {440, 660, 880, 1320}, "independent-instance-started");
            independent.reset();
            verifyMembership({branch.process.dwProcessId, added.process.dwProcessId},
                             {440, 660, 1320}, "independent-instance-exited");
            if (testParentExit) {
                require(TerminateProcess(branch.process.hProcess, 0) &&
                            WaitForSingleObject(branch.process.hProcess, 5000) == WAIT_OBJECT_0,
                        "Stop only the owned parent, retaining its controller job");
                // The group still pins this authenticated child. A process
                // handle also prevents confusing a reused PID with survival.
                members[1]->validate();
                verifyMembership({descendant, added.process.dwProcessId}, {660, 1320},
                                 "parent-exited-descendant-survived");
            }
        }
    }
    require(audioDefaultEndpoints() == defaults, "Group fixture changed global defaults");
    std::cout << JSON{{"ok", true},
                      {"probe", "multi-process-capture-group"},
                      {"virtualOnlyTones", true},
                      {"selectedProcesses", 3},
                      {"disjointRoots", 2},
                      {"missingFrames", missing},
                      {"amplitudes",
                       {{"440", a}, {"660", b}, {"880", c}, {"excluded1100", excluded}}},
                      {"runtimeGroupVerified", true},
                      {"runtimePublicMicrophone", publicResult},
                      {"automaticRouting", routingResult},
                      {"membershipTransitions", membershipResults},
                      {"parentExitExercised", testParentExit},
                      {"runtimeAliasesShareCapture", true},
                      {"defaultsPreserved", true}}
                     .dump()
              << '\n';
    return 0;
}
int wmain(int argc, wchar_t** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        if (argc > 1 && (std::wstring_view(argv[1]) == L"--await-routed-source" ||
                         std::wstring_view(argv[1]) == L"--tone")) {
            wchar_t module[32768]{};
            if (GetModuleFileNameW(nullptr, module, 32768)) {
                std::ofstream file(
                    std::filesystem::path(module).parent_path() /
                    (L"source-error-" + std::to_wstring(GetCurrentProcessId()) + L".json"));
                file << JSON{{"error", error.what()},
                             {"pid", GetCurrentProcessId()},
                             {"timeMs", GetTickCount64()}}
                            .dump(2);
            }
        }
        std::cout << JSON{{"ok", false},
                          {"probe", "multi-process-capture-group"},
                          {"error", error.what()}}
                         .dump()
                  << '\n';
        return 1;
    }
}
