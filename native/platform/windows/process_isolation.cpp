#include "process_capture.hpp"
#include "app_audio_policy.hpp"
#include "app_routing_store.hpp"
#include "app_routing_preferences.hpp"
#include <loopz/app_routing_lease.hpp>
#include "endpoint_roles.hpp"
#include "automatic_render_probe.hpp"
#include <loopz/document_json.hpp>
#include <shlobj.h>
#include <fstream>
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <exception>
#include <iostream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
struct Child {
    PROCESS_INFORMATION process{};
    HANDLE job{};
    Child(const std::filesystem::path& path, const wchar_t* mode, unsigned seconds = 8,
          std::wstring_view endpoint = {}, bool suspended = false,
          std::wstring_view secondEndpoint = {}, std::wstring_view completionEvent = {}) {
        auto command = L"\"" + path.wstring() + L"\" " + mode + L" " + std::to_wstring(seconds);
        for (const auto value : {endpoint, secondEndpoint, completionEvent}) {
            if (value.empty())
                continue;
            if (value.find(L'"') != std::wstring_view::npos)
                throw std::invalid_argument("Invalid marker endpoint");
            command += L" \"" + std::wstring(value) + L"\"";
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                             sizeof(limits))) {
            if (job)
                CloseHandle(job);
            throw std::runtime_error("Cannot contain owned audio marker process");
        }
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                            &process)) {
            CloseHandle(job);
            throw std::runtime_error("Cannot start audio marker process");
        }
        if (!AssignProcessToJobObject(job, process.hProcess) ||
            (!suspended && ResumeThread(process.hThread) == DWORD(-1))) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job);
            throw std::runtime_error("Cannot prepare contained audio marker process");
        }
    }
    ~Child() {
        // These are exclusively owned, finite-lived test processes.
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 0);
                WaitForSingleObject(process.hProcess, 1000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread)
            CloseHandle(process.hThread);
        if (job)
            CloseHandle(job);
    }
};

using Microsoft::WRL::ComPtr;
uint64_t clockNs() {
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    const auto t = static_cast<uint64_t>(ticks.QuadPart),
               f = static_cast<uint64_t>(frequency.QuadPart);
    return t / f * 1000000000ULL + t % f * 1000000000ULL / f;
}
struct Signal {
    double target, other;
    uint64_t missing;
    loopz::AudioHistoryStatus status;
    loopz::windows::ProcessCapture::PacketStatus packet;
    bool ok;
    void write(bool reverse = false) const {
        std::cout << (reverse ? "{\"target880\":" : "{\"target440\":") << target
                  << (reverse ? ",\"excluded440\":" : ",\"excluded880\":") << other
                  << ",\"callbacks\":" << status.callbacks
                  << ",\"discontinuities\":" << status.discontinuities
                  << ",\"packetFrames\":" << packet.frames << ",\"packetFlags\":" << packet.flags
                  << ",\"positionDelta\":" << packet.positionDelta
                  << ",\"qpcDelta100ns\":" << packet.qpcDelta100ns
                  << ",\"missingFrames\":" << missing << ",\"ok\":" << (ok ? "true" : "false")
                  << "}";
    }
};
Signal measure(loopz::windows::ProcessCapture& capture, double targetFrequency = 440,
               double excludedFrequency = 880) {
    const auto history = capture.history();
    const auto status = history->status();
    const auto packet = capture.packetStatus();
    const auto now = clockNs();
    const bool fresh = status.lastHostNs >= 300000000ULL && status.callbacks &&
                       status.lastHostNs <= now && now - status.lastHostNs < 400000000ULL;
    if (!fresh)
        return {0, 0, 0, status, packet, false};
    const uint64_t start = status.lastHostNs - 250000000ULL;
    std::array<float, 256> left{}, right{};
    const std::array<float*, 2> planes{left.data(), right.data()};
    loopz::AudioReadCursor cursor;
    uint64_t missing = 0;
    double sinA = 0, cosA = 0, sinB = 0, cosB = 0;
    constexpr uint32_t blocks = 32, frames = blocks * 256;
    for (uint32_t block = 0; block < blocks; ++block) {
        const auto result =
            history->read(start + static_cast<uint64_t>(block) * 256 * 1000000000ULL / 48000, 48000,
                          {planes, 256}, 256, cursor);
        missing += result.missingFrames;
        for (uint32_t i = 0; i < 256; ++i) {
            const double time = static_cast<double>(block * 256 + i) / 48000;
            const double a = 6.283185307179586 * targetFrequency * time,
                         b = 6.283185307179586 * excludedFrequency * time;
            sinA += left[i] * std::sin(a);
            cosA += left[i] * std::cos(a);
            sinB += left[i] * std::sin(b);
            cosB += left[i] * std::cos(b);
        }
    }
    const double target = 2 * std::hypot(sinA, cosA) / frames,
                 other = 2 * std::hypot(sinB, cosB) / frames;
    const bool ok =
        fresh && !missing && status.discontinuities < 4 && target > 0.00001 && other < target * 0.1;
    return {target, other, missing, status, packet, ok};
}
// Each routing experiment uses a distinct executable identity so persisted
// per-application settings cannot affect the regular probe or a user app.
struct UniqueMarker {
    std::filesystem::path directory, executable;
    bool preserveForRecovery{};
    explicit UniqueMarker(const std::filesystem::path& source) {
        GUID id{};
        wchar_t text[40]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, 40))
            throw std::runtime_error("Cannot create isolated marker identity");
        directory = std::filesystem::temp_directory_path() / (std::wstring(L"LoopZ-route-") + text);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Cannot create isolated marker directory");
        executable = directory / L"marker.exe";
        try {
            std::filesystem::copy_file(source, executable);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
            throw;
        }
    }
    ~UniqueMarker() {
        if (preserveForRecovery)
            return;
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};
std::wstring activeMarkerEndpoint(DWORD pid) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> endpoints;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)))
        throw std::runtime_error("Cannot inspect active marker endpoint");
    UINT count{};
    if (FAILED(endpoints->GetCount(&count)) || count > 4096)
        throw std::runtime_error("Invalid endpoint count");
    std::wstring result;
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
        if (FAILED(sessions->GetCount(&n)) || n < 0 || n > 4096)
            throw std::runtime_error("Invalid session count");
        for (int j = 0; j < n; ++j) {
            ComPtr<IAudioSessionControl> session;
            ComPtr<IAudioSessionControl2> detail;
            DWORD owner{};
            AudioSessionState state{};
            if (FAILED(sessions->GetSession(j, &session)) || FAILED(session.As(&detail)) ||
                detail->GetProcessId(&owner) != S_OK || owner != pid ||
                FAILED(session->GetState(&state)) || state != AudioSessionStateActive)
                continue;
            LPWSTR uid{};
            if (FAILED(device->GetId(&uid)) || !uid)
                throw std::runtime_error("Cannot identify marker endpoint");
            std::wstring current(uid);
            CoTaskMemFree(uid);
            if (!result.empty() && result != current)
                throw std::runtime_error("Marker is active on multiple endpoints");
            result = std::move(current);
        }
    }
    return result;
}
class MarkerRouting {
    loopz::windows::AppAudioPolicy& policy_;
    DWORD pid_;
    std::array<std::wstring, 3> original_;
    std::array<bool, 3> changed_{};

  public:
    MarkerRouting(loopz::windows::AppAudioPolicy& policy, DWORD pid) : policy_(policy), pid_(pid) {
        std::string error;
        for (unsigned role = 0; role < 3; ++role)
            if (!policy_.get(pid_, role, original_[role], error))
                throw std::runtime_error(error);
    }
    // Test-only: caller has relaunched the same uniquely copied owned image.
    // Keep the original baseline and pending restoration across PID replacement.
    void rebindOwnedRelaunch(DWORD pid) {
        pid_ = pid;
    }
    void apply(const std::wstring& target) {
        std::string error;
        for (unsigned role = 0; role < 3; ++role) {
            // Mark before the call: even an ambiguous failure requires restoration.
            changed_[role] = true;
            if (!policy_.set(pid_, role, target, error))
                throw std::runtime_error(error);
            std::wstring actual;
            if (!policy_.get(pid_, role, actual, error) || actual != target)
                throw std::runtime_error("Marker routing preference did not apply: " + error);
        }
    }
    void restore() {
        std::string failures;
        for (unsigned role = 0; role < 3; ++role) {
            if (!changed_[role])
                continue;
            std::string error;
            std::wstring actual;
            if (policy_.set(pid_, role, original_[role], error) &&
                policy_.get(pid_, role, actual, error) && actual == original_[role])
                changed_[role] = false;
            else
                failures += " Role " + std::to_string(role) + ": " + error;
        }
        if (!failures.empty())
            throw std::runtime_error("Marker routing restore failed." + failures);
    }
    ~MarkerRouting() {
        try {
            restore();
        } catch (...) { /* Explicit restore reports failures to the caller. */
        }
    }
};
// This probe can only mute the exact single-process session of its own child.
// Restoration precedes child cleanup and runs on every ordinary exception path.
std::wstring wideArgument(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length <= 1)
        throw std::invalid_argument("Invalid virtual marker argument");
    std::wstring value(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, value.data(), length))
        throw std::runtime_error("Cannot decode virtual marker argument");
    value.pop_back();
    return value;
}
// Fixture controller wrapper: production process/policy adapters authenticate
// the uniquely copied owned child; this wrapper adds durable-before-write assertions.
class JournalMarkerRouting final : public loopz::AppRoutingPreferences {
    loopz::windows::AppAudioPolicy& policy_;
    std::filesystem::path image_, root_;
    std::string identity_;
    std::unique_ptr<loopz::windows::ProcessRoutingPreferences> access_;
    std::unique_ptr<loopz::windows::AppRoutingStore> store_;
    std::unique_ptr<loopz::AppRoutingLease> lease_;
    static std::string encode(std::wstring_view value) {
        if (value.empty())
            return {};
        const auto count =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
            throw std::runtime_error("Invalid marker preference encoding");
        std::string result(static_cast<size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), result.data(), count, nullptr,
                                nullptr) != count)
            throw std::runtime_error("Cannot encode marker preference");
        return result;
    }

  public:
    JournalMarkerRouting(loopz::windows::AppAudioPolicy& policy, DWORD pid,
                         const UniqueMarker& image)
        : policy_(policy), image_(image.executable), root_(image.directory / L"recovery") {
        rebindOwnedRelaunch(pid);
        restartController();
    }
    void rebindOwnedRelaunch(DWORD pid) {
        auto access = std::make_unique<loopz::windows::ProcessRoutingPreferences>(
            policy_, pid, image_, identity_);
        if (identity_.empty())
            identity_ = access->application();
        access_ = std::move(access);
    }
    const loopz::windows::RoutingProcessIdentity& processIdentity() const noexcept {
        return access_->processIdentity();
    }
    void restartController() {
        lease_.reset();
        store_.reset();
        store_ = std::make_unique<loopz::windows::AppRoutingStore>(root_);
        lease_ = std::make_unique<loopz::AppRoutingLease>(store_->journal());
    }
    bool pending() const {
        return !store_->journal().snapshot().empty();
    }
    std::string application() const override {
        return access_->application();
    }
    std::array<std::string, 3> read() override {
        return access_->read();
    }
    void write(unsigned role, std::string_view value) override {
        const auto records = store_->journal().snapshot();
        if (role >= 3 || records.size() != 1 || records[0].application != identity_ ||
            (records[0].original[role] != value &&
             std::find(records[0].attemptedTargets.begin(), records[0].attemptedTargets.end(),
                       value) == records[0].attemptedTargets.end()))
            throw std::runtime_error("Marker preference mutation has no durable ownership receipt");
        access_->write(role, value);
    }
    void apply(const std::wstring& target) {
        if (lease_->apply(*this, encode(target)) != loopz::AppRoutingApplyResult::Applied)
            throw std::runtime_error("Owned marker routing was externally changed");
    }
    void restore() {
        if (!lease_ || !store_)
            restartController();
        lease_->release(*this);
        if (pending())
            throw std::runtime_error("Marker routing receipt remains after restoration");
    }
};
int automaticRoutingLifecycle(const std::filesystem::path& executable, const char* feed) {
    const auto endpoints = loopz::windows::loopzEndpoints();
    if (!endpoints.contains(feed) || endpoints.at(feed).role != loopz::windows::EndpointRole::Feed)
        throw std::invalid_argument("Lifecycle routing requires a LoopZ Feed endpoint");
    const auto defaults = loopz::windows::audioDefaultEndpoints();
    const auto target = loopz::windows::AppAudioPolicy::renderInterface(feed);
    const auto targetUid = wideArgument(feed);
    UniqueMarker image(executable);
    auto marker = std::make_unique<Child>(image.executable, L"--automatic-silence", 30);
    const auto waitEndpoint = [&](const std::wstring* expected) {
        const auto deadline = GetTickCount64() + 5000;
        std::wstring current;
        do {
            if (WaitForSingleObject(marker->process.hProcess, 0) != WAIT_TIMEOUT)
                throw std::runtime_error("Lifecycle silent marker exited early");
            current = activeMarkerEndpoint(marker->process.dwProcessId);
            if (!current.empty() && (!expected || current == *expected))
                return current;
            Sleep(100);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error("Lifecycle marker endpoint did not settle");
    };
    const auto before = waitEndpoint(nullptr);
    if (before == targetUid)
        throw std::runtime_error("Lifecycle marker needs a distinct initial endpoint");
    loopz::windows::AppAudioPolicy policy;
    if (!policy.available())
        throw std::runtime_error(policy.error());
    const DWORD firstPid = marker->process.dwProcessId;
    JournalMarkerRouting routing(policy, firstPid, image);
    bool receiptRecovered = false;
    const auto firstIdentity = routing.application();
    const auto firstCreated = routing.processIdentity().created100ns;
    bool exited = false, deadPidReadable = false, persisted = false, streamOnTarget = false,
         reappliedStreamMoved = false;
    std::array<bool, 3> rolesOnTarget{}, rolesEmpty{};
    DWORD secondPid{};
    const auto relaunch = [&] {
        marker = std::make_unique<Child>(image.executable, L"--automatic-silence", 30);
        const auto endpoint = waitEndpoint(nullptr);
        routing.rebindOwnedRelaunch(marker->process.dwProcessId);
        return endpoint;
    };
    try {
        routing.apply(target);
        waitEndpoint(&targetUid);
        // Simulate an app crash, stopping only this job-contained owned child.
        if (!TerminateProcess(marker->process.hProcess, 0) ||
            WaitForSingleObject(marker->process.hProcess, 5000) != WAIT_OBJECT_0)
            throw std::runtime_error("Cannot stop owned lifecycle marker");
        exited = true;
        if (!routing.pending())
            throw std::runtime_error("Applied routing receipt disappeared");
        routing.restartController();
        receiptRecovered = routing.pending();
        if (!receiptRecovered)
            throw std::runtime_error("Routing receipt did not survive reopen");
        std::wstring ignored;
        std::string error;
        deadPidReadable = policy.get(firstPid, 0, ignored, error);
        relaunch();
        Sleep(1000); // Observe settled routing separately from initial session activation.
        const auto relaunchedEndpoint = activeMarkerEndpoint(marker->process.dwProcessId);
        secondPid = marker->process.dwProcessId;
        streamOnTarget = relaunchedEndpoint == targetUid;
        persisted = true;
        for (unsigned role = 0; role < 3; ++role) {
            std::wstring actual;
            if (!policy.get(secondPid, role, actual, error))
                throw std::runtime_error(error);
            rolesOnTarget[role] = actual == target;
            rolesEmpty[role] = actual.empty();
            persisted = persisted && rolesOnTarget[role];
        }
        routing.apply(target);
        waitEndpoint(&targetUid);
        reappliedStreamMoved = true;
        routing.restore();
        waitEndpoint(&before);
    } catch (...) {
        const auto failure = std::current_exception();
        // A dead PID cannot be assumed to restore persisted application policy.
        // Recover through an active instance of exactly this owned image.
        try {
            if (exited &&
                (!secondPid || WaitForSingleObject(marker->process.hProcess, 0) != WAIT_TIMEOUT))
                relaunch();
            routing.restore();
        } catch (...) {
            image.preserveForRecovery = true;
            std::cerr << "Routing recovery evidence retained at " << image.directory << '\n';
            throw;
        }
        std::rethrow_exception(failure);
    }
    const bool defaultsPreserved = defaults == loopz::windows::audioDefaultEndpoints();
    const bool distinctPid = firstPid != secondPid;
    const bool identityMatched = firstIdentity == routing.application() &&
                                 firstCreated != routing.processIdentity().created100ns;
    const bool ok = identityMatched && persisted && receiptRecovered && reappliedStreamMoved &&
                    distinctPid && defaultsPreserved;
    const auto writeRoles = [](const std::array<bool, 3>& values) {
        std::cout << '[';
        for (unsigned role = 0; role < 3; ++role)
            std::cout << (role ? "," : "") << (values[role] ? "true" : "false");
        std::cout << ']';
    };
    std::cout << "{\"rolesOnTargetAfterRelaunch\":";
    writeRoles(rolesOnTarget);
    std::cout << ",\"rolesEmptyAfterRelaunch\":";
    writeRoles(rolesEmpty);
    std::cout << ",\"streamOnTargetAfterRelaunch\":" << (streamOnTarget ? "true" : "false")
              << ",\"probe\":\"automatic-silent-routing-lifecycle\",\"firstPid\":" << firstPid
              << ",\"secondPid\":" << secondPid
              << ",\"sameApplicationNewProcessVerified\":" << (identityMatched ? "true" : "false")
              << ",\"firstCreated100ns\":" << firstCreated
              << ",\"secondCreated100ns\":" << routing.processIdentity().created100ns
              << ",\"durableReceiptRecovered\":" << (receiptRecovered ? "true" : "false")
              << ",\"receiptReleased\":" << (!routing.pending() ? "true" : "false")
              << ",\"deadPidReadable\":" << (deadPidReadable ? "true" : "false")
              << ",\"preferenceSurvivedExit\":" << (persisted ? "true" : "false")
              << ",\"reapplyingMovedStream\":" << (reappliedStreamMoved ? "true" : "false")
              << ",\"preferencesRestoredThroughNewPid\":true,\"activeStreamRestored\":true"
                 ",\"submittedOnlySilence\":true,\"defaultsPreserved\":"
              << (defaultsPreserved ? "true" : "false") << ",\"ok\":" << (ok ? "true" : "false")
              << "}\n";
    return ok ? 0 : 1;
}
void requireVirtualPair(const char* first, const char* second) {
    const auto endpoints = loopz::windows::loopzEndpoints();
    const auto a = endpoints.find(first), b = endpoints.find(second);
    if (a == endpoints.end() || b == endpoints.end() ||
        a->second.role != loopz::windows::EndpointRole::Feed ||
        b->second.role != loopz::windows::EndpointRole::Feed ||
        a->second.mixKey == b->second.mixKey)
        throw std::invalid_argument("Requires two distinct LoopZ Feed roots");
}
void refuseHardwareMonitors() {
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        throw std::runtime_error("Cannot inspect marker monitor configuration");
    const auto path = std::filesystem::path(local) / L"LoopZ" / L"routing.json";
    CoTaskMemFree(local);
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("Saved marker configuration is too large");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read marker monitor configuration");
    const auto document = loopz::decodeDocument(
        loopz::parseControlJSON(std::string(std::istreambuf_iterator<char>(file), {})));
    for (const auto& device : document.devices)
        if (!device.monitors.empty())
            throw std::runtime_error("Virtual marker refuses saved hardware monitors");
}
struct VirtualMarkerGuard {
    std::array<std::wstring, 2> feeds;
    static bool allowed(const void* raw) {
        const auto& guard = *static_cast<const VirtualMarkerGuard*>(raw);
        const auto active = activeMarkerEndpoint(GetCurrentProcessId());
        return active == guard.feeds[0] || active == guard.feeds[1];
    }
};
struct CompletionEvent {
    HANDLE value{};
    std::wstring name;
    explicit CompletionEvent(const UniqueMarker& image)
        : name(L"Local\\LoopZ-Automatic-Stopped-" + image.directory.filename().wstring()) {
        value = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        if (!value)
            throw std::runtime_error("Cannot create virtual marker completion event");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(value);
            value = nullptr;
            throw std::runtime_error("Virtual marker completion event already exists");
        }
    }
    ~CompletionEvent() {
        if (value)
            CloseHandle(value);
    }
};
int automaticCaptureRoute(const std::filesystem::path& executable, const char* first,
                          const char* second) {
    requireVirtualPair(first, second);
    refuseHardwareMonitors();
    const auto defaults = loopz::windows::audioDefaultEndpoints();
    const auto a = wideArgument(first), b = wideArgument(second);
    UniqueMarker image(executable);
    CompletionEvent completion(image);
    Child marker(image.executable, L"--automatic-virtual-tone", 10, a, false, b, completion.name);
    Signal firstSignal{}, secondSignal{};
    std::exception_ptr failure;
    const auto waitFor = [&](const std::wstring& expected) {
        const auto deadline = GetTickCount64() + 5000;
        do {
            if (WaitForSingleObject(marker.process.hProcess, 0) != WAIT_TIMEOUT)
                throw std::runtime_error("Virtual automatic marker exited early");
            const auto active = activeMarkerEndpoint(marker.process.dwProcessId);
            if (!active.empty() && (expected.empty() || active == expected))
                return;
            Sleep(100);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error("Virtual automatic marker did not reach its target Feed");
    };
    // Windows needs an initialized audio session to resolve this application's
    // routing identity. The marker sends only silence outside the two Feeds.
    waitFor(std::wstring{});
    loopz::windows::AppAudioPolicy policy;
    if (!policy.available())
        throw std::runtime_error(policy.error());
    MarkerRouting routing(policy, marker.process.dwProcessId);
    loopz::windows::ProcessCapture capture;
    try {
        routing.apply(loopz::windows::AppAudioPolicy::renderInterface(first));
        waitFor(a);
        std::string error;
        if (!capture.start(marker.process.dwProcessId, error))
            throw std::runtime_error(error);
        Sleep(800);
        firstSignal = measure(capture);
        routing.apply(loopz::windows::AppAudioPolicy::renderInterface(second));
        waitFor(b);
        Sleep(800);
        secondSignal = measure(capture);
    } catch (...) {
        failure = std::current_exception();
    }
    capture.stop();
    // The child signals only after all render clients are stopped/released, and
    // then stays alive for policy restoration. Never restore a physical default
    // while this automatic stream could still contain a marker.
    if (WaitForSingleObject(completion.value, 35000) != WAIT_OBJECT_0) {
        TerminateProcess(marker.process.hProcess, 1);
        WaitForSingleObject(marker.process.hProcess, 5000);
        routing.restore();
        throw std::runtime_error("Virtual marker did not confirm stopped rendering");
    }
    routing.restore();
    if (failure)
        std::rethrow_exception(failure);
    DWORD childResult{};
    if (WaitForSingleObject(marker.process.hProcess, 25000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(marker.process.hProcess, &childResult) || childResult != 0)
        throw std::runtime_error("Virtual automatic marker did not complete successfully");
    const bool defaultsPreserved = defaults == loopz::windows::audioDefaultEndpoints();
    const bool ok = firstSignal.ok && secondSignal.ok && defaultsPreserved;
    std::cout << "{\"probe\":\"automatic-virtual-capture-routing\","
                 "\"activeStreamMoved\":true,\"renderingStoppedBeforeRestore\":true,"
                 "\"preferencesRestored\":true,\"defaultsPreserved\":"
              << (defaultsPreserved ? "true" : "false") << ",\"firstFeed\":";
    firstSignal.write();
    std::cout << ",\"secondFeed\":";
    secondSignal.write();
    std::cout << ",\"ok\":" << (ok ? "true" : "false") << "}\n";
    return ok ? 0 : 1;
}
class MarkerMute {
    ComPtr<ISimpleAudioVolume> volume_;
    BOOL original_ = FALSE;
    bool changed_ = false;
    const GUID context_{
        0x413713b5, 0xdaac, 0x4bf6, {0xbc, 0x4b, 0x88, 0xf2, 0x25, 0x5c, 0xe1, 0xc6}};

  public:
    explicit MarkerMute(DWORD ownedChild, bool requireUnmuted = true) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> endpoints;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)))
            throw std::runtime_error("Cannot enumerate marker sessions");
        UINT count{};
        if (FAILED(endpoints->GetCount(&count)) || count > 4096)
            throw std::runtime_error("Invalid render endpoint enumeration");
        for (UINT endpoint = 0; endpoint < count; ++endpoint) {
            ComPtr<IMMDevice> device;
            ComPtr<IAudioSessionManager2> manager;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(endpoints->Item(endpoint, &device)) ||
                FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                        &manager)) ||
                FAILED(manager->GetSessionEnumerator(&sessions)))
                continue;
            int n{};
            if (FAILED(sessions->GetCount(&n)) || n < 0 || n > 4096)
                throw std::runtime_error("Invalid marker session enumeration");
            for (int index = 0; index < n; ++index) {
                ComPtr<IAudioSessionControl> session;
                ComPtr<IAudioSessionControl2> detail;
                DWORD pid{};
                if (FAILED(sessions->GetSession(index, &session)) || FAILED(session.As(&detail)) ||
                    detail->GetProcessId(&pid) != S_OK || pid != ownedChild)
                    continue;
                if (volume_)
                    throw std::runtime_error(
                        "Marker has more than one session; mute probe refused");
                if (FAILED(session.As(&volume_)))
                    throw std::runtime_error("Marker session has no volume control");
            }
        }
        if (!volume_ || FAILED(volume_->GetMute(&original_)) || (requireUnmuted && original_))
            throw std::runtime_error("Requires an initially unmuted owned marker session");
    }
    void mute() {
        if (FAILED(volume_->SetMute(TRUE, &context_)))
            throw std::runtime_error("Cannot mute owned marker session");
        changed_ = true;
        BOOL actual{};
        if (FAILED(volume_->GetMute(&actual)) || !actual)
            throw std::runtime_error("Marker mute state did not apply");
    }
    void restore() {
        if (FAILED(volume_->SetMute(original_, &context_)))
            throw std::runtime_error("Cannot restore owned marker session");
        BOOL actual{};
        if (FAILED(volume_->GetMute(&actual)) || actual != original_)
            throw std::runtime_error("Marker mute state did not restore");
        changed_ = false;
    }
    ~MarkerMute() {
        if (changed_)
            volume_->SetMute(original_, &context_);
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        const bool silentProbe = argc == 3 && std::string_view(argv[1]) == "--silent-probe";
        const bool muteProbe = argc == 2 && std::string_view(argv[1]) == "--mute-probe";
        const bool policyProbe = argc == 2 && std::string_view(argv[1]) == "--policy-probe";
        const bool routeProbe = argc == 3 && std::string_view(argv[1]) == "--route-probe";
        const bool automaticProbe =
            argc == 3 && std::string_view(argv[1]) == "--automatic-route-probe";
        const bool lifecycleProbe =
            argc == 3 && std::string_view(argv[1]) == "--automatic-lifecycle-probe";
        const bool automaticChild = argc == 3 && std::string_view(argv[1]) == "--automatic-silence";
        const bool automaticCapture =
            argc == 4 && std::string_view(argv[1]) == "--automatic-capture-route-probe";
        const bool automaticToneChild =
            argc == 6 && std::string_view(argv[1]) == "--automatic-virtual-tone";
        if (argc != 1 && !muteProbe && !policyProbe && !routeProbe && !silentProbe &&
            !automaticProbe && !lifecycleProbe && !automaticChild && !automaticCapture &&
            !automaticToneChild)
            throw std::runtime_error(
                "Use no arguments, --mute-probe, --policy-probe, --route-probe RENDER_UID, "
                "--silent-probe FEED_UID, or --automatic-route-probe FEED_UID");
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized))
            throw std::runtime_error("Cannot initialize isolation probe COM");
        struct Apartment {
            ~Apartment() {
                CoUninitialize();
            }
        } apartment;
        if (automaticToneChild) {
            if (std::string_view(argv[2]) != "10")
                throw std::invalid_argument("Virtual automatic marker requires fixed duration");
            const auto eventName = wideArgument(argv[5]);
            const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
            if (!event)
                throw std::runtime_error("Cannot open virtual marker completion event");
            int result = 0;
            try {
                requireVirtualPair(argv[3], argv[4]);
                refuseHardwareMonitors();
                const VirtualMarkerGuard guard{{wideArgument(argv[3]), wideArgument(argv[4])}};
                result = loopz::windows::probe::automaticVirtualTone(
                    10, VirtualMarkerGuard::allowed, &guard);
            } catch (...) {
                result = 1;
            }
            // automaticVirtualTone has released its client before this signal.
            if (!SetEvent(event))
                result = 1;
            CloseHandle(event);
            Sleep(20000); // Keep the image/PID alive while the parent restores preferences.
            return result;
        }
        if (automaticChild) {
            // This internal child mode has no tone-generation path.
            if (std::string_view(argv[2]) != "30")
                throw std::invalid_argument("Automatic child requires the fixed 30-second fixture");
            return loopz::windows::probe::automaticSilence(30);
        }
        wchar_t module[32768]{};
        GetModuleFileNameW(nullptr, module, 32768);
        const auto executable =
            std::filesystem::path(module).parent_path() / L"loopz-windows-probe.exe";
        if (lifecycleProbe)
            return automaticRoutingLifecycle(std::filesystem::path(module), argv[2]);
        if (automaticCapture)
            return automaticCaptureRoute(std::filesystem::path(module), argv[2], argv[3]);
        if (automaticProbe) {
            const auto endpoints = loopz::windows::loopzEndpoints();
            if (!endpoints.contains(argv[2]) ||
                endpoints.at(argv[2]).role != loopz::windows::EndpointRole::Feed)
                throw std::invalid_argument("Automatic routing requires a LoopZ Feed endpoint");
            const auto defaultsBefore = loopz::windows::audioDefaultEndpoints();
            const auto target = loopz::windows::AppAudioPolicy::renderInterface(argv[2]);
            const int length =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, nullptr, 0);
            if (length <= 1)
                throw std::invalid_argument("Invalid automatic routing target");
            std::wstring targetUid(static_cast<size_t>(length), L'\0');
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, targetUid.data(),
                                     length))
                throw std::runtime_error("Cannot decode automatic routing target");
            targetUid.pop_back();
            UniqueMarker image{std::filesystem::path(module)};
            Child marker(image.executable, L"--automatic-silence", 30);
            const auto waitEndpoint = [&](const std::wstring* expected) {
                const auto deadline = GetTickCount64() + 5000;
                std::wstring current;
                do {
                    if (WaitForSingleObject(marker.process.hProcess, 0) != WAIT_TIMEOUT)
                        throw std::runtime_error("Automatic silent marker exited early");
                    current = activeMarkerEndpoint(marker.process.dwProcessId);
                    if (!current.empty() && (!expected || current == *expected))
                        return current;
                    Sleep(100);
                } while (GetTickCount64() < deadline);
                return current;
            };
            const auto before = waitEndpoint(nullptr);
            if (before.empty() || before == targetUid)
                throw std::runtime_error("Automatic marker needs a distinct initial endpoint");
            loopz::windows::AppAudioPolicy policy;
            if (!policy.available())
                throw std::runtime_error(policy.error());
            MarkerRouting routing(policy, marker.process.dwProcessId);
            std::wstring during, after;
            try {
                routing.apply(target);
                during = waitEndpoint(&targetUid);
                routing.restore();
                after = waitEndpoint(&before);
            } catch (...) {
                const auto failure = std::current_exception();
                // Report restoration failure while the owned process is still
                // alive; do not rely only on a best-effort destructor restore.
                routing.restore();
                std::rethrow_exception(failure);
            }
            const bool defaultsPreserved =
                defaultsBefore == loopz::windows::audioDefaultEndpoints();
            const bool ok = during == targetUid && after == before && defaultsPreserved;
            std::cout << "{\"probe\":\"automatic-silent-routing\",\"preferencesRestored\":true,"
                         "\"activeStreamMoved\":"
                      << (during == targetUid ? "true" : "false")
                      << ",\"activeStreamRestored\":" << (after == before ? "true" : "false")
                      << ",\"defaultsPreserved\":" << (defaultsPreserved ? "true" : "false")
                      << ",\"submittedOnlySilence\":true,\"ok\":" << (ok ? "true" : "false")
                      << "}\n";
            return ok ? 0 : 1;
        }
        if (routeProbe) {
            const auto target = loopz::windows::AppAudioPolicy::renderInterface(argv[2]);
            const int length =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, nullptr, 0);
            if (length <= 1)
                throw std::runtime_error("Invalid target endpoint UID");
            std::wstring targetUid(static_cast<size_t>(length), L'\0');
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, targetUid.data(),
                                     length))
                throw std::runtime_error("Cannot decode target endpoint UID");
            targetUid.pop_back();
            UniqueMarker image(executable);
            Child marker(image.executable, L"--tone", 30);
            loopz::windows::ProcessCapture capture;
            std::string error;
            if (!capture.start(marker.process.dwProcessId, error))
                throw std::runtime_error(error);
            Sleep(2000);
            const auto before = activeMarkerEndpoint(marker.process.dwProcessId);
            const auto baseline = measure(capture);
            if (before.empty() || !baseline.ok)
                throw std::runtime_error("Requires active isolated marker and valid capture");
            loopz::windows::AppAudioPolicy policy;
            if (!policy.available())
                throw std::runtime_error(policy.error());
            MarkerRouting routing(policy, marker.process.dwProcessId);
            routing.apply(target);
            Sleep(3000);
            const auto during = activeMarkerEndpoint(marker.process.dwProcessId);
            const auto routed = measure(capture);
            Child reopened(image.executable, L"--tone", 20);
            loopz::windows::ProcessCapture reopenedCapture;
            if (!reopenedCapture.start(reopened.process.dwProcessId, error))
                throw std::runtime_error(error);
            Sleep(2000);
            const auto reopenedEndpoint = activeMarkerEndpoint(reopened.process.dwProcessId);
            const auto reopenedSignal = measure(reopenedCapture);
            routing.restore();
            Sleep(3000);
            const auto after = activeMarkerEndpoint(marker.process.dwProcessId);
            const auto restored = measure(capture);
            const bool moved = !during.empty() && during == targetUid;
            const bool returned = after == before && restored.ok;
            std::cout << "{\"preferencesApplied\":true,\"preferencesRestored\":true,"
                         "\"activeStreamMoved\":"
                      << (moved ? "true" : "false")
                      << ",\"captureSurvivesRouting\":" << (routed.ok ? "true" : "false")
                      << ",\"newStreamUsesTarget\":"
                      << (reopenedEndpoint == targetUid ? "true" : "false")
                      << ",\"newStreamCaptureValid\":" << (reopenedSignal.ok ? "true" : "false")
                      << ",\"activeStreamRestored\":" << (returned ? "true" : "false")
                      << ",\"baseline\":";
            baseline.write();
            std::cout << ",\"routed\":";
            routed.write();
            std::cout << ",\"reopened\":";
            reopenedSignal.write();
            std::cout << ",\"restored\":";
            restored.write();
            std::cout << "}\n";
            reopenedCapture.stop();
            capture.stop();
            return returned ? 0 : 1;
        }
        if (policyProbe) {
            Child marker(executable, L"--tone");
            Sleep(2000);
            MarkerMute sessionCheck(marker.process.dwProcessId, false); // read-only session check
            loopz::windows::AppAudioPolicy policy;
            if (!policy.available())
                throw std::runtime_error(policy.error());
            unsigned readable = 0;
            std::cout << "{\"available\":true,\"activeOwnedMarker\":true,\"roleReads\":[";
            for (unsigned role = 0; role < 3; ++role) {
                std::wstring value;
                std::string error;
                const bool ok = policy.get(marker.process.dwProcessId, role, value, error);
                readable += ok ? 1 : 0;
                if (role)
                    std::cout << ',';
                // The adapter's errors are fixed ASCII diagnostics plus an HRESULT.
                std::cout << "{\"role\":" << role << ",\"readable\":" << (ok ? "true" : "false")
                          << ",\"unset\":" << (ok && value.empty() ? "true" : "false")
                          << ",\"error\":" << std::quoted(error) << '}';
            }
            std::cout << "],\"readableRoles\":" << readable << ",\"preferencesChanged\":false}\n";
            return readable ? 0 : 1;
        }
        std::wstring silentEndpoint;
        if (silentProbe) {
            const std::string uid = argv[2];
            const auto endpoints = loopz::windows::loopzEndpoints();
            if (!endpoints.contains(uid) ||
                endpoints.at(uid).role != loopz::windows::EndpointRole::Feed)
                throw std::invalid_argument("Silent isolation requires a LoopZ Feed endpoint");
            // A Feed can be monitored by a saved mix. Refuse every saved hardware
            // monitor before generating any marker; never change user routing.
            PWSTR local = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
                throw std::runtime_error("Cannot inspect saved monitor configuration");
            const auto saved = std::filesystem::path(local) / L"LoopZ" / L"routing.json";
            CoTaskMemFree(local);
            if (std::filesystem::file_size(saved) > 1024 * 1024)
                throw std::runtime_error("Saved configuration is too large");
            std::ifstream file(saved, std::ios::binary);
            if (!file)
                throw std::runtime_error("Cannot read saved monitor configuration");
            const auto document = loopz::decodeDocument(
                loopz::parseControlJSON(std::string(std::istreambuf_iterator<char>(file), {})));
            for (const auto& device : document.devices)
                if (!device.monitors.empty())
                    throw std::runtime_error("Silent isolation refuses saved hardware monitors");
            const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, uid.data(),
                                                   static_cast<int>(uid.size()), nullptr, 0);
            if (!count)
                throw std::invalid_argument("Invalid Feed UID");
            silentEndpoint.resize(static_cast<size_t>(count));
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, uid.data(),
                                    static_cast<int>(uid.size()), silentEndpoint.data(),
                                    count) != count)
                throw std::invalid_argument("Cannot decode Feed UID");
        }
        Child selected(executable, silentProbe ? L"--feed-tone" : L"--tone", muteProbe ? 16 : 8,
                       silentEndpoint);
        Child excluded(executable, silentProbe ? L"--feed-tone-high" : L"--tone-high",
                       muteProbe ? 16 : 8, silentEndpoint);
        loopz::windows::ProcessCapture capture;
        std::string error;
        if (!capture.start(selected.process.dwProcessId, error))
            throw std::runtime_error(error);
        loopz::windows::ProcessCapture excludedCapture;
        if (silentProbe && !excludedCapture.start(excluded.process.dwProcessId, error))
            throw std::runtime_error(error);
        Sleep(2000);
        const auto baseline = measure(capture);
        bool ok = baseline.ok;
        if (silentProbe) {
            const auto control = measure(excludedCapture, 880, 440);
            const auto strict = [](const Signal& signal) {
                return signal.ok && signal.target > 0.027 && signal.target < 0.033 &&
                       signal.other < signal.target * 0.01;
            };
            ok = strict(baseline) && strict(control);
            std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"selected\":";
            baseline.write();
            std::cout << ",\"excludedControl\":";
            control.write(true);
            std::cout << "}\n";
            excludedCapture.stop();
        } else if (muteProbe) {
            if (!baseline.ok)
                throw std::runtime_error("Requires a valid isolated marker before muting");
            MarkerMute session(selected.process.dwProcessId);
            session.mute();
            Sleep(2000);
            const auto muted = measure(capture);
            session.restore();
            Sleep(2000);
            const auto restored = measure(capture);
            const bool survives = muted.ok && muted.target > baseline.target * 0.9 &&
                                  muted.target < baseline.target * 1.1;
            ok = ok && restored.ok;
            std::cout << "{\"ok\":" << (ok ? "true" : "false")
                      << ",\"muteApplied\":true,\"muteRestored\":true,\"captureSurvivesMute\":"
                      << (survives ? "true" : "false") << ",\"baseline\":";
            baseline.write();
            std::cout << ",\"muted\":";
            muted.write();
            std::cout << ",\"restored\":";
            restored.write();
            std::cout << "}\n";
        } else {
            baseline.write();
            std::cout << '\n';
        }
        capture.stop();
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
