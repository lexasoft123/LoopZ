#include "runtime.hpp"
#include "audio_kernels.hpp"
#include "broker_coordinator.hpp"
#include "endpoint_roles.hpp"
#include "driver_identity.hpp"
#include "login_startup.hpp"
#include "process_capture.hpp"
#include "process_capture_group.hpp"
#include "app_routing_preferences.hpp"
#include "app_routing_sessions.hpp"
#include "app_routing_store.hpp"
#include "app_routing_startup.hpp"
#include "app_routing_sink_lifetime.hpp"
#include "virtual_cable.hpp"
#include "source_refresh.hpp"
#include "scene_domains.hpp"
#include "worker_recovery.hpp"
#include <loopz/edit_history.hpp>
#include <loopz/monitor.hpp>
#include <loopz/sample_rates.hpp>
#include "driver/audio_format.hpp"
#include <loopz/render_schedule.hpp>
#include <loopz/timeline.hpp>
#include <zcore/device/audio_host.h>
#include <zcore/device/audio_input.h>
#include <windows.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace loopz::windows {
using Microsoft::WRL::ComPtr;
std::string utf8(std::wstring_view value) {
    if (value.empty())
        return {};
    const int count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        throw std::runtime_error("Invalid Windows text");
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring wide(std::string_view value) {
    if (value.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
        throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), count);
    return result;
}
std::filesystem::path userDirectory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path)))
        throw std::runtime_error("Cannot locate local application data");
    const auto result = std::filesystem::path(path) / L"LoopZ";
    CoTaskMemFree(path);
    std::filesystem::create_directories(result);
    return result;
}
namespace {
uint64_t nowNs() noexcept {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const auto ticks = static_cast<uint64_t>(counter.QuadPart),
               rate = static_cast<uint64_t>(frequency.QuadPart);
    return ticks / rate * 1000000000ULL + ticks % rate * 1000000000ULL / rate;
}
std::string identity() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("Cannot generate engine identity");
    wchar_t text[40]{};
    StringFromGUID2(guid, text, 40);
    return utf8(text);
}
void save(const std::filesystem::path& path, const std::string& bytes) {
    const auto temp = path.wstring() + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot write routing document");
    DWORD written = 0;
    const bool ok =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Cannot flush routing document");
    }
    const auto backup = path.wstring() + L".bak";
    if (std::filesystem::exists(path)) {
        if (!ReplaceFileW(path.c_str(), temp.c_str(), backup.c_str(), 0, nullptr, nullptr))
            throw std::runtime_error("Cannot atomically replace routing document");
    } else if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish routing document");
}
JSON meter(const AudioMeterSnapshot& value, bool valid) {
    JSON peaks = JSON::array();
    if (valid)
        for (uint32_t c = 0; c < value.channels; ++c)
            peaks.push_back(value.peak[c]);
    return {{"valid", valid}, {"peak", peaks}};
}
bool matchesRoutingApplication(const SourceBinding& binding,
                               ProcessRoutingPreferences& preferences) {
    if (binding.kind != SourceKind::Application)
        return false;
    const auto application = preferences.application();
    const auto& process = preferences.processIdentity();
    if (binding.selector == application || binding.selector == utf8(process.reportedImage))
        return true;
    if (!binding.selector.starts_with("windows-routing-v1|")) {
        try {
            AppRoutingProcess selected(process.pid, wide(binding.selector), application);
            return true;
        } catch (const std::exception&) {
        }
    }
    return false;
}
struct App {
    std::string selector, name;
    uint32_t pid;
    std::string application;
    uint64_t created100ns{};
    std::shared_ptr<AppRoutingProcess> process;
};
bool captureApplicationMatches(const App& app, const std::string& selector) {
    if (selector == app.selector || selector == app.application)
        return true;
    if (selector.starts_with("windows-routing-v1|"))
        return false;
    try {
        AppRoutingProcess selected(app.pid, wide(selector), app.application);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
std::vector<std::shared_ptr<AppRoutingProcess>> captureMembers(const std::vector<App>& apps,
                                                               const std::string& selector) {
    std::string application;
    for (const auto& app : apps)
        if (captureApplicationMatches(app, selector)) {
            if (!application.empty() && application != app.application)
                throw std::runtime_error(
                    "Application identity is ambiguous; select the application again");
            application = app.application;
        }
    std::vector<std::shared_ptr<AppRoutingProcess>> result;
    for (const auto& app : apps)
        if (app.application == application && !application.empty())
            result.push_back(app.process);
    return result;
}
std::vector<App> applications(bool requireComplete = false) {
    std::vector<App> result;
    std::vector<AppRoutingSession> sessions;
    try {
        sessions = appRoutingSessions();
    } catch (...) {
        if (requireComplete)
            throw;
        return result; // Display discovery may be unavailable; never authorize routing from it.
    }
    std::set<uint32_t> seen;
    for (const auto& session : sessions) {
        const auto pid = session.pid;
        if (!pid || pid == GetCurrentProcessId() || !seen.insert(pid).second)
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            continue;
        wchar_t path[32768]{};
        DWORD length = 32768;
        const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!ok)
            continue;
        try {
            const std::filesystem::path image(std::wstring(path, length));
            auto pinned = std::make_shared<AppRoutingProcess>(pid, image);
            const auto identity = pinned->identity();
            result.push_back({utf8(image.native()), utf8(image.stem().native()), pid,
                              identity.application, identity.created100ns, std::move(pinned)});
        } catch (const std::exception&) {
            // Foreign-user/session, exited or unauthenticated hosts cannot
            // authorize a selected-application capture group.
        }
    }
    return result;
}
// Each resolved binding gets its own serial reader and retained channel anchors.
// Capture histories/providers may be shared, but render cache state is not.
class HardwareReader final : public AudioSource {
  public:
    explicit HardwareReader(std::vector<std::shared_ptr<AudioHistory>> histories)
        : histories_(std::move(histories)), cursors_(histories_.size()) {}
    uint32_t channels() const noexcept override {
        return static_cast<uint32_t>(histories_.size());
    }
    AudioReadResult read(uint64_t ns, double rate, RoutingOutput out, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        if (out.channels.size() != histories_.size()) {
            for (auto* plane : out.channels)
                if (plane)
                    std::fill_n(plane, std::min({frames, out.capacityFrames, kBlock}), 0.0f);
            return {frames, false};
        }
        AudioReadResult result{0, true};
        for (size_t c = 0; c < histories_.size(); ++c) {
            const auto r = histories_[c]->read(
                ns, rate, {{&out.channels[c], 1}, out.capacityFrames}, frames, cursors_[c]);
            result.missingFrames = std::max(result.missingFrames, r.missingFrames);
            result.online = result.online && r.online;
        }
        return result;
    }

  private:
    const std::vector<std::shared_ptr<AudioHistory>> histories_;
    mutable std::vector<AudioReadCursor> cursors_;
};
class HardwareSource final {
  public:
    HardwareSource(std::string uid, uint32_t channels,
                   const std::vector<singz::AudioInputDevice>& inventory) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            auto history = std::make_shared<AudioHistory>(1);
            auto input = std::make_unique<singz::AudioInput>();
            const auto opened = input->start(
                {uid, channel, 32},
                [history](const singz::AudioInputBlockView& b) {
                    if (!(b.capture.flags & singz::AudioInputSampleHostTimeValid)) {
                        history->stop();
                        return;
                    }
                    if (b.capture.flags & singz::AudioInputDiscontinuous)
                        history->stop();
                    const std::array<const float*, 1> planes{b.mono};
                    (void)history->append({planes, b.frames}, b.frames, b.capture.sampleHostTimeNs,
                                          b.sampleRate, static_cast<double>(b.capture.sourceFrame),
                                          (b.capture.flags & singz::AudioInputSourceFrameValid) !=
                                              0);
                },
                &inventory);
            if (!opened.ok)
                throw std::runtime_error(opened.error);
            history_.push_back(history);
            input_.push_back(std::move(input));
        }
    }
    ~HardwareSource() {
        for (auto& input : input_)
            input->stop();
        for (auto& h : history_)
            h->stop();
    }
    bool healthy() const {
        return std::all_of(input_.begin(), input_.end(), [](const auto& input) {
            return input->state() == singz::AudioInputState::Running;
        });
    }
    JSON diagnostics() const {
        auto result = JSON::array();
        for (size_t channel = 0; channel < input_.size(); ++channel) {
            const auto stats = input_[channel]->stats();
            result.push_back({{"channel", channel},
                              {"deliveredFrames", stats.deliveredFrames},
                              {"overruns", stats.overruns},
                              {"discontinuities", history_[channel]->status().discontinuities}});
        }
        return result;
    }
    std::shared_ptr<const AudioSource> reader() const {
        return std::make_shared<HardwareReader>(history_);
    }

  private:
    std::vector<std::shared_ptr<AudioHistory>> history_;
    std::vector<std::unique_ptr<singz::AudioInput>> input_;
};
// Per-scene reader timing. The wrapper preserves every source capability and
// owns its counters, so scene replacement cannot race a counter reset.
class TimedSource final : public AudioSource {
  public:
    explicit TimedSource(std::shared_ptr<const AudioSource> source) : source_(std::move(source)) {}
    uint32_t channels() const noexcept override {
        return source_->channels();
    }
    bool readMeter(AudioMeterSnapshot& meter) const noexcept override {
        return source_->readMeter(meter);
    }
    AudioReadResult read(uint64_t ns, double rate, RoutingOutput out, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override {
        const auto start = nowNs();
        const auto result = source_->read(ns, rate, out, frames, cursor);
        record(start);
        return result;
    }
    AudioReadResult readWithFilter(uint64_t ns, double rate, RoutingOutput out, uint32_t frames,
                                   AudioReadCursor& cursor,
                                   ResamplingFilter filter) const noexcept override {
        const auto start = nowNs();
        const auto result = source_->readWithFilter(ns, rate, out, frames, cursor, filter);
        record(start);
        return result;
    }
    std::optional<AudioReadResult>
    readChannels(uint64_t ns, double rate, RoutingOutput out, uint32_t frames,
                 AudioReadCursor& cursor, std::span<const uint32_t> channels,
                 std::optional<ResamplingFilter> filter) const noexcept override {
        const auto start = nowNs();
        const auto result = source_->readChannels(ns, rate, out, frames, cursor, channels, filter);
        record(start);
        return result;
    }
    JSON timing() const {
        return {
            {"calls", calls_.load()}, {"totalNs", total_.load()}, {"maximumNs", maximum_.load()}};
    }

  private:
    void record(uint64_t start) const noexcept {
        const auto elapsed = nowNs() - start;
        calls_.fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(elapsed, std::memory_order_relaxed);
        maximum_.store(std::max(maximum_.load(std::memory_order_relaxed), elapsed),
                       std::memory_order_relaxed);
    }
    const std::shared_ptr<const AudioSource> source_;
    mutable std::atomic<uint64_t> calls_{0}, total_{0}, maximum_{0};
};
class BusSink final : public AudioSink {
  public:
    explicit BusSink(uint32_t channels, uint32_t rate)
        : bus(std::make_shared<MonitorBus>(channels, rate, AudioHistory::kDefaultCapacityFrames,
                                           true)) {}
    uint32_t channels() const noexcept override {
        return bus->channels();
    }
    bool requested() const noexcept override {
        return true;
    }
    bool publish(uint64_t frame, uint32_t rate, RoutingInput in,
                 uint32_t frames) noexcept override {
        return bus->publish(frame, rate, in, frames);
    }
    void idle() noexcept override {
        bus->idle();
    }
    std::shared_ptr<MonitorBus> bus;
};
struct Output {
    std::string device, id, error, signature, renderEndpoint;
    bool publication{false};
    std::atomic<int64_t> outputLeadNs{0};
    std::unique_ptr<MonitorProgram> program;
    std::shared_ptr<VirtualCable> cable;
    WorkerRecovery recovery;
    singz::AudioHost host;
    bool connected() const noexcept {
        return error.empty() &&
               (cable ? cable->healthy() : host.status().state == singz::AudioHostState::Running);
    }
    MonitorProgram* activeProgram() const noexcept {
        return cable ? cable->program() : program.get();
    }
    static bool render(void* raw, const singz::AudioHostRenderBlock& b) noexcept {
        auto& self = *static_cast<Output*>(raw);
        self.outputLeadNs.store(static_cast<int64_t>(b.outputHostTimeNs) -
                                static_cast<int64_t>(b.callbackHostTimeNs));
        return self.program
            ->render(b.outputHostTimeNs, b.sampleRate, {{b.output, b.outputChannels}, b.frames},
                     b.frames, b.outputFrame)
            .validBlock;
    }
    ~Output() {
        host.stop();
    }
};
} // namespace

struct Runtime::Impl {
    std::filesystem::path storage;
    RouteDocument document;
    uint64_t generation{0}, activeGeneration{0};
    std::string epoch{identity()}, loadError, runtimeError;
    EditHistory history;
    std::unique_ptr<BrokerCoordinator> microphoneSetup;
    std::string microphoneSetupError;
    AppRoutingSinkDemand submittedSinkDemand{AppRoutingSinkDemand::Unknown};
    uint64_t submittedSinkGeneration{};
    bool savedDocumentAuthoritative{};
    bool routingRecoveryEnabled{};
    std::unique_ptr<AppRoutingStore> routingStore;
    JSON routingRecoveryState = {{"examined", false}, {"pending", 0}, {"error", ""}};
    std::chrono::steady_clock::time_point nextRoutingRecovery{};
    struct RoutingAttempt {
        std::vector<ProcessInstance> membership;
        std::string endpoint;
        uint64_t deadlineNs{}, retryAfterNs{};
        bool verified{};
    };
    std::map<std::string, RoutingAttempt> routingAttempts;
    singz::AudioHostInventory inventory;
    std::map<std::string, EndpointRole> endpointRoles;
    std::map<std::string, DriverEndpoint> driverEndpoints;
    std::wstring ownerSid{currentOwnerSid()};
    std::vector<App> apps;
    std::map<std::string, std::shared_ptr<HardwareSource>> hardware;
    std::map<std::string, std::shared_ptr<ProcessCaptureGroup>> processes;
    std::map<std::string, std::string> sourceErrors;
    std::map<std::string, std::shared_ptr<BusSink>> sinks;
    std::map<std::string, std::shared_ptr<VirtualCable>> cables;
    std::vector<std::unique_ptr<Output>> outputs;
    std::unique_ptr<SceneDomains> scene;
    std::shared_ptr<SceneHistoryStore> sceneHistories{std::make_shared<SceneHistoryStore>()};
    SceneDomainSchedules schedules;
    std::jthread render;
    std::atomic<uint64_t> renderCalls{0}, renderTotalNs{0}, renderMaximumNs{0},
        renderMaximumLatenessNs{0}, renderReanchors{0}, renderSkippedFrames{0};
    std::atomic<bool> renderMmcssRegistered{false};
    std::atomic<DWORD> renderMmcssError{0};
    std::atomic<bool> renderHighResolutionTimer{false};
    std::atomic<DWORD> renderTimerCreationError{0};
    std::chrono::steady_clock::time_point refreshed{};

    explicit Impl(std::filesystem::path path, bool manageMicrophones)
        : storage(std::move(path)), routingRecoveryEnabled(manageMicrophones) {
        prepareAudioKernels();
        if (std::filesystem::exists(storage)) {
            try {
                document = load(storage);
                savedDocumentAuthoritative = true;
            } catch (const std::exception& e) {
                loadError = e.what();
                try {
                    document = load(storage.wstring() + L".bak");
                    loadError = "Recovered the previous saved routing document.";
                } catch (...) {
                    throw std::runtime_error("Routing document and backup cannot be read; preserve "
                                             "them before repairing setup.");
                }
            }
        }
        if (manageMicrophones) {
            try {
                microphoneSetup = std::make_unique<BrokerCoordinator>();
                submitMicrophones();
            } catch (const std::exception& error) {
                microphoneSetupError = error.what();
            }
        }
        refresh();
        try {
            rebuild();
        } catch (const std::exception& error) {
            // A readable saved document must remain editable even if its audio
            // configuration cannot run on this Windows installation.
            runtimeError = error.what();
        }
    }
    ~Impl() {
        microphoneSetup.reset();
        stop();
    }
    bool commitDocument(RouteDocument candidate, EditHistory nextHistory) {
        validateCableFeedback(candidate);
        const auto bytes = encodeDocument(candidate).dump();
        if (bytes == encodeDocument(document).dump())
            return false;
        if (bytes.size() > 1024 * 1024)
            throw std::invalid_argument("Routing document exceeds its size limit");
        save(storage, bytes);
        document = std::move(candidate);
        history.swap(nextHistory);
        ++generation;
        savedDocumentAuthoritative = true;
        try {
            rebuild();
        } catch (const std::exception& error) {
            runtimeError = error.what();
        }
        // Close obsolete KS handles before publishing setup/PnP intent.
        submitMicrophones();
        return true;
    }
    void submitMicrophones(AppRoutingSinkDemand demand = AppRoutingSinkDemand::Unknown) {
        if (!microphoneSetup)
            return;
        try {
            microphoneSetup->submit(document, savedDocumentAuthoritative, generation, demand);
            submittedSinkDemand = demand;
            submittedSinkGeneration = generation;
            microphoneSetupError.clear();
        } catch (const std::exception& error) {
            microphoneSetupError = error.what();
        }
    }
    bool wantsApplicationOutputRouting() const {
        for (const auto& device : document.devices)
            if (device.routing.enabled)
                for (const auto& binding : device.bindings)
                    if (binding.kind == SourceKind::Application && binding.muteOriginal)
                        for (const auto& source : device.routing.sources)
                            if (source.id == binding.id && source.enabled)
                                return true;
        return false;
    }
    void reconcileRoutingSink() {
        AppRoutingSinkFacts facts;
        facts.savedAuthoritative = savedDocumentAuthoritative;
        // Missing storage never proves that an existing sink has no leases.
        facts.recoveryExamined = routingStore && routingRecoveryState.value("examined", false);
        facts.pendingReceipts = routingRecoveryState.value("pending", size_t{}) != 0;
        facts.desiredApplications = wantsApplicationOutputRouting();
        JSON observed = {{"error", ""}};
        try {
            AppRoutingLocalRenderClients localClients;
            localClients.examined = true;
            // Only Output::host renders through WASAPI. HardwareSource and
            // ProcessCapture are capture-only; publications use separate normal
            // mix KS transports, never the typed internal routing sink.
            for (const auto& output : outputs)
                if (!output->publication && !output->renderEndpoint.empty())
                    localClients.endpoints.push_back(output->renderEndpoint);
            const auto sink = observeAppRoutingSink(ownerSid, localClients);
            facts.endpointsExamined = sink.endpointsExamined;
            facts.endpointOccupied = sink.occupied;
            facts.defaultsExamined = sink.defaultsExamined;
            facts.defaultEndpoint = sink.defaultEndpoint;
            observed["endpoint"] = sink.endpoint;
            observed["error"] = sink.error;
        } catch (const std::exception& error) {
            observed["error"] = error.what();
        }
        const auto observedDemand = appRoutingSinkDemand(facts);
        // Unknown preserves an existing sink but cannot provision a new one.
        // Durable history and complete recovery must precede setup intent.
        const auto demand = facts.recoveryExamined ? observedDemand : AppRoutingSinkDemand::Unknown;
        observed["demand"] = demand == AppRoutingSinkDemand::Required   ? "required"
                             : demand == AppRoutingSinkDemand::Released ? "released"
                                                                        : "unknown";
        observed["endpointsExamined"] = facts.endpointsExamined;
        observed["occupied"] = facts.endpointOccupied;
        observed["defaultsExamined"] = facts.defaultsExamined;
        observed["defaultEndpoint"] = facts.defaultEndpoint;
        observed["recoveryExamined"] = facts.recoveryExamined;
        routingRecoveryState["sink"] = std::move(observed);
        // Other control operations submit Unknown, invalidating a cached release.
        // Only a fresh complete MTA observation may authorize sink removal.
        if (submittedSinkDemand != demand || submittedSinkGeneration != generation)
            submitMicrophones(demand);
    }
    JSON microphoneSetupState() const {
        JSON result = {{"enabled", microphoneSetup != nullptr},
                       {"authoritative", savedDocumentAuthoritative},
                       {"error", microphoneSetupError}};
        if (!microphoneSetup)
            return result;
        const auto status = microphoneSetup->state();
        const bool current = status.generation == generation;
        result["generation"] = std::to_string(status.generation);
        result["examined"] = current && status.examined;
        result["accepted"] = current && status.accepted;
        if (result["error"].get<std::string>().empty() && current)
            result["error"] = status.error;
        if (current && status.routingSink) {
            const auto& sink = *status.routingSink;
            result["routingSink"] = {{"phase", sink.phase},
                                     {"error", sink.error},
                                     {"revision", std::to_string(sink.revision)},
                                     {"removing", sink.removing}};
        }
        result["devices"] = JSON::object();
        if (current)
            for (const auto& [id, device] : status.devices)
                result["devices"][id] = {{"phase", device.phase},
                                         {"error", device.error},
                                         {"revision", std::to_string(device.revision)},
                                         {"removing", device.removing}};
        return result;
    }
    static RouteDocument load(const std::filesystem::path& path) {
        if (std::filesystem::file_size(path) > 1024 * 1024)
            throw std::runtime_error("Routing file is too large");
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open saved routing");
        return decodeDocument(
            parseControlJSON(std::string(std::istreambuf_iterator<char>(file), {})));
    }
    void stop() {
        if (render.joinable()) {
            render.request_stop();
            render.join();
        }
        outputs.clear();
        cables.clear();
        scene.reset();
        sinks.clear();
        processes.clear();
        hardware.clear();
        sourceErrors.clear();
    }
    void refresh() {
        singz::AudioHost host;
        inventory = host.enumerate();
        driverEndpoints = loopzEndpoints();
        endpointRoles.clear();
        for (const auto& [id, endpoint] : driverEndpoints)
            endpointRoles[id] = endpoint.role;
        apps = applications();
        refreshed = std::chrono::steady_clock::now();
    }
    void validateCableFeedback(const RouteDocument& candidate) const {
        bool feedsCable = false, capturesCable = false;
        for (const auto& device : candidate.devices) {
            for (const auto& monitor : device.monitors)
                feedsCable |=
                    endpointRole(endpointRoles, monitor.deviceSelector) == EndpointRole::Feed;
            for (const auto& source : device.bindings) {
                if (source.kind != SourceKind::InputDevice)
                    continue;
                const auto& uid =
                    source.selector == "default" ? inventory.defaultInputUid : source.selector;
                capturesCable |= endpointRole(endpointRoles, uid) == EndpointRole::Microphone;
            }
        }
        if ((feedsCable || !candidate.devices.empty()) && capturesCable)
            throw std::invalid_argument(
                "Use a mix source to route between LoopZ mixes. Selecting a LoopZ "
                "microphone as a hardware input can create feedback.");
    }
    void rebuild() {
        struct FailureGuard {
            Impl& owner;
            bool complete{false};
            ~FailureGuard() {
                if (!complete)
                    owner.stop();
            }
        } failure{*this};
        runtimeError.clear();
        try {
            validateCableFeedback(document);
        } catch (const std::exception& error) {
            runtimeError = error.what();
            return;
        }
        const auto domainPlan = planSceneDomains(document);
        std::map<std::string, uint32_t> deviceRates;
        for (const auto& domain : domainPlan)
            for (const auto index : domain.devices)
                deviceRates.emplace(document.devices[index].id, domain.rate);
        for (const auto& device : document.devices) {
            if (!supportedVirtualSampleRate(device.sampleRate) ||
                !loopz_driver::AudioFormat{static_cast<unsigned>(device.sampleRate),
                                           device.routing.outputChannels}
                     .valid())
                throw std::invalid_argument("Unsupported Windows microphone format");
        }
        auto previousOutputs = std::move(outputs);
        outputs.clear();
        sourceErrors.clear();
        std::set<std::string> wantedHardware, wantedProcesses;
        std::map<std::string, std::shared_ptr<ProcessCaptureGroup>> preparedProcesses;
        std::map<std::string, std::string> failedProcesses;
        const auto inputs = singz::enumerateAudioInputDevices();
        for (const auto& d : document.devices)
            for (const auto& b : d.bindings) {
                if (!d.routing.enabled)
                    continue;
                const auto found = std::find_if(d.routing.sources.begin(), d.routing.sources.end(),
                                                [&](const auto& s) {
                                                    return s.id == b.id;
                                                });
                const bool needsPlaybackCapture =
                    b.kind == SourceKind::Application && b.muteOriginal;
                if (found == d.routing.sources.end() || !found->enabled ||
                    (found->muted && !needsPlaybackCapture))
                    continue;
                const auto key = b.selector + ":" + std::to_string(found->channels);
                std::string processApplication;
                try {
                    if (b.kind == SourceKind::InputDevice) {
                        wantedHardware.insert(key);
                        if (hardware.contains(key) && !hardware.at(key)->healthy())
                            hardware.erase(key);
                    }
                    if (b.kind == SourceKind::Application) {
                        wantedProcesses.insert(b.selector);
                        const auto members = captureMembers(apps, b.selector);
                        if (members.empty()) {
                            processes.erase(b.selector);
                            throw std::runtime_error(
                                "Application has no authenticated audio process");
                        }
                        processApplication = members.front()->identity().application;
                        if (failedProcesses.contains(processApplication))
                            throw std::runtime_error(failedProcesses.at(processApplication));
                        auto prepared = preparedProcesses.find(processApplication);
                        if (prepared == preparedProcesses.end()) {
                            const auto previous = std::find_if(
                                processes.begin(), processes.end(), [&](const auto& item) {
                                    return item.second->application() == processApplication;
                                });
                            std::shared_ptr<ProcessCaptureGroup> group;
                            if (previous != processes.end() && previous->second->matches(members) &&
                                previous->second->error().empty())
                                group = previous->second;
                            else
                                group = std::make_shared<ProcessCaptureGroup>(
                                    members,
                                    previous == processes.end() ? nullptr : previous->second.get());
                            prepared =
                                preparedProcesses.emplace(processApplication, std::move(group))
                                    .first;
                        }
                        if (!prepared->second->matches(members))
                            throw std::runtime_error(
                                "Application capture membership changed during preparation");
                        processes[b.selector] = prepared->second;
                    }
                    if (b.kind == SourceKind::InputDevice && !hardware.contains(key)) {
                        auto uid = b.selector;
                        if (uid == "default") {
                            const auto it =
                                std::find_if(inputs.begin(), inputs.end(), [](const auto& i) {
                                    return i.isDefault;
                                });
                            if (it == inputs.end())
                                throw std::runtime_error("Default microphone is unavailable");
                            uid = it->uid;
                        } else if (!uid.starts_with("wasapi:"))
                            uid = "wasapi:" + uid;
                        hardware[key] =
                            std::make_shared<HardwareSource>(uid, found->channels, inputs);
                    }
                } catch (const std::exception& e) {
                    if (b.kind == SourceKind::Application) {
                        processes.erase(b.selector);
                        if (!processApplication.empty()) {
                            failedProcesses[processApplication] = e.what();
                            preparedProcesses.erase(processApplication);
                            std::erase_if(processes, [&](const auto& item) {
                                return item.second->application() == processApplication;
                            });
                        }
                    } // No partial group may authorize routing through another alias.
                    sourceErrors[b.selector] = e.what();
                }
            }
        std::set<std::string> wantedCables;
        for (const auto& device : document.devices) {
            if (!device.routing.enabled)
                continue;
            const auto targetRenderRate = deviceRates.at(device.id);
            const auto mixKey = driverMixKey(ownerSid, device.id);
            std::string endpointId;
            for (const auto& [uid, endpoint] : driverEndpoints)
                if (endpoint.role == EndpointRole::Feed && endpoint.mixKey == mixKey) {
                    if (!endpointId.empty())
                        throw std::runtime_error("Multiple playback endpoints belong to one mix");
                    endpointId = uid;
                }
            if (endpointId.empty())
                continue;
            const auto installed = std::find_if(inventory.devices.begin(), inventory.devices.end(),
                                                [&](const auto& item) {
                                                    return item.uid == endpointId;
                                                });
            // Do not reopen the old adapter while its format change is pending.
            // Even a connect/query/close cycle acquires a KS handle and can race
            // the broker's PnP restart. Inventory changes trigger a fresh rebuild.
            if (installed == inventory.devices.end() ||
                installed->outputChannels != device.routing.outputChannels ||
                installed->nominalSampleRate != device.sampleRate)
                continue;
            wantedCables.insert(device.id);
            auto& sink = sinks[device.id];
            if (!sink || sink->channels() != device.routing.outputChannels ||
                sink->bus->renderRate() != targetRenderRate)
                sink = std::make_shared<BusSink>(device.routing.outputChannels, targetRenderRate);
            auto& cable = cables[device.id];
            if (cable && (!cable->healthy() || cable->endpoint() != endpointId ||
                          !cable->matches(device, targetRenderRate))) {
                cable->stop();
                cable.reset();
            }
            if (!cable) {
                cable = std::make_shared<VirtualCable>(endpointId, device, sink->bus->source(),
                                                       targetRenderRate);
                cable->start();
            }
        }
        std::erase_if(cables, [&](const auto& entry) {
            if (wantedCables.contains(entry.first))
                return false;
            entry.second->stop();
            return true;
        });
        wchar_t stageTimingSetting[2]{};
        const bool stageTiming =
            GetEnvironmentVariableW(L"LOOPZ_SCENE_STAGE_TIMING", stageTimingSetting, 2) == 1 &&
            stageTimingSetting[0] == L'1';
        auto preparedScene = std::make_unique<SceneDomains>(
            document, domainPlan,
            [&](const auto& device, const SourceBinding& b,
                const SourceRouting& s) -> ResolvedAudioSource {
                std::shared_ptr<const AudioSource> source;
                const auto key = b.selector + ":" + std::to_string(s.channels);
                if (b.kind == SourceKind::InputDevice && hardware.contains(key))
                    source = hardware.at(key)->reader();
                if (b.kind == SourceKind::Application && processes.contains(b.selector))
                    source = processes.at(b.selector)->reader();
                if (b.kind == SourceKind::PassThrough && cables.contains(device.id))
                    source = cables.at(device.id)->playback();
                if (source)
                    source = std::make_shared<TimedSource>(std::move(source));
                return {source, std::make_shared<AudioReadCursor>()};
            },
            [&](const VirtualDeviceDefinition& d) -> std::shared_ptr<AudioSink> {
                const auto targetRenderRate = deviceRates.at(d.id);
                auto& sink = sinks[d.id];
                if (!sink || sink->channels() != d.routing.outputChannels ||
                    sink->bus->renderRate() != targetRenderRate)
                    sink = std::make_shared<BusSink>(d.routing.outputChannels, targetRenderRate);
                return sink;
            },
            sceneHistories, schedules, stageTiming);
        for (const auto& d : document.devices) {
            const auto targetRenderRate = deviceRates.at(d.id);
            auto destinations = d.monitors;
            const auto key = driverMixKey(ownerSid, d.id);
            std::string feed;
            for (const auto& [uid, endpoint] : driverEndpoints)
                if (endpoint.role == EndpointRole::Feed && endpoint.mixKey == key) {
                    if (!feed.empty()) {
                        feed.clear();
                        break;
                    }
                    feed = uid;
                }
            // Public microphone publication is independent of saved hardware monitors.
            MonitorDefinition publication;
            publication.id = "__loopz_public_microphone";
            publication.deviceSelector = feed;
            publication.channels = d.routing.outputChannels;
            publication.enabled = true;
            for (uint32_t channel = 0; channel < publication.channels; ++channel)
                publication.connections.push_back({channel, channel, 1});
            destinations.push_back(publication);
            for (const auto& m : destinations) {
                if (!d.routing.enabled || !m.enabled)
                    continue;
                JSON wires = JSON::array();
                for (const auto& wire : m.connections)
                    wires.push_back({wire.sourceChannel, wire.outputChannel, wire.gain});
                const auto endpoint = std::find_if(inventory.devices.begin(),
                                                   inventory.devices.end(), [&](const auto& item) {
                                                       return item.uid == m.deviceSelector;
                                                   });
                const auto signature =
                    JSON::array(
                        {d.id, m.id, m.deviceSelector, m.channels, m.gain, m.muted,
                         d.routing.outputChannels, d.sampleRate, targetRenderRate, wires,
                         endpoint == inventory.devices.end() ? 0 : endpoint->nominalSampleRate})
                        .dump();
                const auto existing = std::find_if(
                    previousOutputs.begin(), previousOutputs.end(), [&](const auto& item) {
                        return item && item->signature == signature && item->connected();
                    });
                if (existing != previousOutputs.end()) {
                    outputs.push_back(std::move(*existing));
                    continue;
                }
                auto output = std::make_unique<Output>();
                output->signature = signature;
                output->device = d.id;
                output->id = m.id;
                output->publication = m.id == "__loopz_public_microphone";
                try {
                    if (output->publication && feed.empty())
                        throw std::runtime_error("Microphone unavailable. Use Settings → Install "
                                                 "or repair microphones.");
                    if (output->publication) {
                        const auto cable = cables.find(d.id);
                        if (cable == cables.end())
                            throw std::runtime_error(
                                "Microphone format is updating. Waiting for device setup.");
                        output->cable = cable->second;
                        if (!output->cable->healthy())
                            throw std::runtime_error(output->cable->error());
                        outputs.push_back(std::move(output));
                        continue;
                    }
                    if (!output->publication &&
                        endpointRole(endpointRoles, m.deviceSelector) == EndpointRole::Feed)
                        throw std::runtime_error(
                            "LoopZ Feed is managed automatically; choose a hardware monitor.");
                    const auto device = std::find_if(inventory.devices.begin(),
                                                     inventory.devices.end(), [&](const auto& i) {
                                                         return i.uid == m.deviceSelector;
                                                     });
                    if (device == inventory.devices.end() || device->outputChannels < m.channels)
                        throw std::runtime_error("Monitor endpoint is unavailable");
                    singz::AudioHostConfig config;
                    config.outputDeviceUid = device->uid;
                    output->renderEndpoint =
                        device->uid.starts_with("wasapi:") ? device->uid.substr(7) : device->uid;
                    for (uint32_t c = 0; c < m.channels; ++c)
                        config.outputChannels.push_back(c);
                    const auto opened = output->host.open(config, Output::render, output.get());
                    if (!opened.ok)
                        throw std::runtime_error(opened.message);
                    const auto prepared = output->host.status();
                    const auto endpointFrames = std::max(prepared.diagnostics.outputBufferFrames,
                                                         opened.format.nominalBufferFrames);
                    // WASAPI requests samples for their future presentation time,
                    // not the callback's receipt time. Queue the actual negotiated
                    // endpoint buffer, reported stream latency and scheduling headroom.
                    const auto queue =
                        std::ceil(static_cast<double>(endpointFrames) * targetRenderRate /
                                  opened.format.sampleRate) +
                        (prepared.diagnostics.outputStreamLatency100ns
                             ? std::ceil(static_cast<double>(
                                             prepared.diagnostics.outputStreamLatency100ns) *
                                         targetRenderRate / 10000000.0)
                             // An unreported stream latency is not a zero-delay
                             // hardware path. Reserve another negotiated buffer.
                             : std::ceil(static_cast<double>(endpointFrames) * targetRenderRate /
                                         opened.format.sampleRate)) +
                        targetRenderRate * 1280.0 / 48000;
                    if (!std::isfinite(queue) || queue > kCapacity / 2)
                        throw std::runtime_error("The negotiated output buffer exceeds the "
                                                 "preview's monitor history budget");
                    output->program = std::make_unique<MonitorProgram>(
                        d, m, sinks.at(d.id)->bus->source(),
                        MonitorTiming{targetRenderRate, opened.format.sampleRate,
                                      static_cast<uint32_t>(queue)});
                    const auto started = output->host.start();
                    if (!started.ok)
                        throw std::runtime_error(started.message);
                } catch (const std::exception& e) {
                    output->error = e.what();
                    output->host.stop();
                }
                outputs.push_back(std::move(output));
            }
        }
        // Providers and unchanged output callbacks remain live during preparation.
        // Only pause the serial graph writer for the final ownership swap.
        if (render.joinable()) {
            render.request_stop();
            render.join();
        }
        scene = std::move(preparedScene);
        std::erase_if(schedules, [&](const auto& item) {
            return std::none_of(domainPlan.begin(), domainPlan.end(), [&](const auto& domain) {
                return domain.rate == item.first;
            });
        });
        scene->activate();
        renderCalls.store(0);
        renderTotalNs.store(0);
        renderMaximumNs.store(0);
        renderMaximumLatenessNs.store(0);
        renderReanchors.store(0);
        renderSkippedFrames.store(0);
        renderMmcssRegistered.store(false);
        renderMmcssError.store(0);
        renderHighResolutionTimer.store(false);
        renderTimerCreationError.store(0);
        std::erase_if(hardware, [&](const auto& item) {
            return !wantedHardware.contains(item.first);
        });
        std::erase_if(processes, [&](const auto& item) {
            return !wantedProcesses.contains(item.first);
        });
        std::erase_if(sinks, [&](const auto& item) {
            return std::none_of(document.devices.begin(), document.devices.end(),
                                [&](const auto& device) {
                                    return device.id == item.first;
                                });
        });
        if (document.devices.empty()) {
            failure.complete = true;
            activeGeneration = generation;
            return;
        }
        render = std::jthread([this](std::stop_token stop) {
            DWORD task = 0;
            HANDLE priority = AvSetMmThreadCharacteristicsW(L"Audio", &task);
            renderMmcssError.store(priority ? 0 : GetLastError());
            renderMmcssRegistered.store(priority != nullptr);
            HANDLE timer = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            renderHighResolutionTimer.store(timer != nullptr);
            renderTimerCreationError.store(timer ? 0 : GetLastError());
            if (!timer)
                timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            while (!stop.stop_requested()) {
                uint64_t nextWakeNs = UINT64_MAX;
                for (const auto& domain : scene->domains()) {
                    const auto step = domain.schedule->step(nowNs());
                    if (step.reanchored)
                        renderReanchors.fetch_add(1, std::memory_order_relaxed);
                    renderSkippedFrames.fetch_add(step.skippedFrames, std::memory_order_relaxed);
                    for (uint32_t b = 0; b < step.blocks; ++b) {
                        const auto started = nowNs();
                        const auto scheduled = frameNs(step.firstFrame + b * 256, domain.rate);
                        auto& timing = *domain.timing;
                        const auto lateness = started > scheduled ? started - scheduled : 0;
                        timing.maximumLatenessNs.store(
                            std::max(timing.maximumLatenessNs.load(std::memory_order_relaxed),
                                     lateness),
                            std::memory_order_relaxed);
                        if (started > scheduled)
                            renderMaximumLatenessNs.store(
                                std::max(renderMaximumLatenessNs.load(std::memory_order_relaxed),
                                         started - scheduled),
                                std::memory_order_relaxed);
                        (void)domain.scene->render(step.firstFrame + b * 256, 256);
                        const auto elapsed = nowNs() - started;
                        timing.calls.fetch_add(1, std::memory_order_relaxed);
                        timing.totalNs.fetch_add(elapsed, std::memory_order_relaxed);
                        timing.maximumNs.store(
                            std::max(timing.maximumNs.load(std::memory_order_relaxed), elapsed),
                            std::memory_order_relaxed);
                        renderCalls.fetch_add(1, std::memory_order_relaxed);
                        renderTotalNs.fetch_add(elapsed, std::memory_order_relaxed);
                        renderMaximumNs.store(
                            std::max(renderMaximumNs.load(std::memory_order_relaxed), elapsed),
                            std::memory_order_relaxed);
                    }
                    nextWakeNs = std::min(nextWakeNs, step.nextWakeNs);
                }
                const auto now = nowNs();
                if (timer && nextWakeNs > now) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>((nextWakeNs - now) / 100 + 1);
                    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                        (void)WaitForSingleObject(timer, 100);
                    else
                        Sleep(1);
                } else if (!timer)
                    Sleep(1);
            }
            if (timer)
                CloseHandle(timer);
            if (priority)
                AvRevertMmThreadCharacteristics(priority);
        });
        activeGeneration = generation;
        failure.complete = true;
    }
    void recoverAudio() {
        const auto now = nowNs();
        for (const auto& output : outputs)
            if (output->cable && output->recovery.attempt(now, output->cable->healthy()))
                if (output->cable->start())
                    output->error.clear();
    }
    JSON state() {
        if (std::chrono::steady_clock::now() - refreshed > std::chrono::seconds(3)) {
            const auto previous = inventory;
            const auto oldApps = apps;
            refresh();
            bool changed =
                previous.defaultInputUid != inventory.defaultInputUid ||
                previous.defaultOutputUid != inventory.defaultOutputUid ||
                previous.devices.size() != inventory.devices.size() ||
                sourceRefreshRequired(document, oldApps, apps, sourceErrors,
                                      [](const auto& app, const auto& selector) {
                                          return captureApplicationMatches(app, selector);
                                      });
            for (const auto& [key, input] : hardware) {
                (void)key;
                changed = changed || !input->healthy();
            }
            if (!changed)
                for (size_t i = 0; i < inventory.devices.size(); ++i)
                    changed =
                        changed || inventory.devices[i].uid != previous.devices[i].uid ||
                        inventory.devices[i].nominalSampleRate !=
                            previous.devices[i].nominalSampleRate ||
                        inventory.devices[i].inputChannels != previous.devices[i].inputChannels ||
                        inventory.devices[i].outputChannels != previous.devices[i].outputChannels;
            for (const auto& o : outputs) {
                if (!o->cable)
                    changed = changed || (o->error.empty() && !o->connected());
            }
            for (const auto& [id, p] : processes) {
                (void)id;
                changed = changed || !p->error().empty();
            }
            if (changed) {
                try {
                    rebuild();
                } catch (const std::exception& e) {
                    runtimeError = e.what();
                }
            }
        }
        recoverAudio();
        JSON devices = JSON::array(), applicationsJSON = JSON::array(), meters = JSON::array(),
             sources = JSON::array(), monitors = JSON::array();
        JSON captureDiagnostics = JSON::array();
        for (const auto& [selector, input] : hardware)
            captureDiagnostics.push_back(
                {{"selector", selector}, {"channels", input->diagnostics()}});
        JSON applicationCaptures = JSON::array();
        for (const auto& [selector, capture] : processes) {
            JSON roots = JSON::array();
            for (const auto& root : capture->roots())
                roots.push_back({{"pid", root.id}, {"created100ns", root.generation}});
            applicationCaptures.push_back({{"selector", selector},
                                           {"roots", roots},
                                           {"ready", capture->readyToRoute()},
                                           {"error", capture->error()}});
        }
        for (const auto& i : inventory.devices)
            devices.push_back(
                {{"uid", i.uid},
                 {"name", i.label},
                 {"inputChannels", i.inputChannels},
                 {"outputChannels", i.outputChannels},
                 {"rate", i.nominalSampleRate},
                 {"defaultInput", i.defaultInput},
                 {"defaultOutput", i.defaultOutput},
                 {"virtualRole",
                  endpointRole(endpointRoles, i.uid) == EndpointRole::Feed
                      ? "loopz-feed"
                      : (endpointRole(endpointRoles, i.uid) == EndpointRole::Microphone
                             ? "loopz-microphone"
                             : "")}});
        std::set<std::string> listedApplications;
        for (const auto& a : apps)
            if (listedApplications.insert(a.application).second)
                applicationsJSON.push_back(
                    {{"bundle", a.process->identity().applicationUserModelId.empty()
                                    ? a.selector
                                    : a.application},
                     {"name", a.name}});
        if (scene)
            for (size_t index = 0; index < document.devices.size(); ++index) {
                const auto& d = document.devices[index];
                AudioMeterSnapshot value;
                const auto stages = scene->deviceStatus(index);
                JSON row{{"id", d.id},
                         {"active", scene->deviceStatus(index).active},
                         {"renderRate", scene->rate(index)},
                         {"renderedFrames", scene->deviceStatus(index).renderedFrames},
                         {"graphFailures", scene->deviceStatus(index).graphFailures},
                         {"sinkFailures", scene->deviceStatus(index).sinkFailures},
                         {"sceneTiming",
                          {{"enabled", stages.stageTiming},
                           {"inputNs", stages.inputNs},
                           {"graphNs", stages.graphNs},
                           {"outputNs", stages.outputNs},
                           {"sinkNs", stages.sinkNs}}},
                         {"meter", meter(value, false)},
                         {"sources", JSON::array()}};
                row["meter"] = meter(value, scene->deviceMeters(index, value));
                const auto key = driverMixKey(ownerSid, d.id);
                row["microphone"] = {{"mixKey", key}, {"installed", false}};
                bool publicFormatReady = false;
                for (const auto& [uid, endpoint] : driverEndpoints)
                    if (endpoint.mixKey == key && endpoint.role == EndpointRole::Microphone) {
                        row["microphone"]["installed"] = true;
                        row["microphone"]["uid"] = uid;
                        const auto named =
                            std::find_if(inventory.devices.begin(), inventory.devices.end(),
                                         [&](const auto& item) {
                                             return item.uid == uid;
                                         });
                        if (named != inventory.devices.end()) {
                            row["microphone"]["name"] = named->label;
                            publicFormatReady = named->inputChannels == d.routing.outputChannels &&
                                                named->nominalSampleRate == d.sampleRate;
                        }
                    }
                row["microphone"]["formatReady"] = publicFormatReady;
                row["microphone"]["connected"] = false;
                for (const auto& output : outputs)
                    if (output->device == d.id && output->publication) {
                        row["microphone"]["connected"] = output->connected() && publicFormatReady;
                        row["microphone"]["detail"] =
                            output->cable ? output->cable->error() : output->error;
                        if (output->connected() && !publicFormatReady)
                            row["microphone"]["detail"] =
                                "Waiting for Windows to publish the selected microphone format.";
                        if (output->cable) {
                            const auto timing = output->cable->diagnostics();
                            row["microphone"]["transportTiming"] = {
                                {"version", timing.transportVersion},
                                {"bitsPerSample", timing.bitsPerSample},
                                {"threadCpuTimingAvailable", timing.threadCpuTimingAvailable},
                                {"threadCpuNs", timing.threadCpuNs},
                                {"threadId", timing.threadId},
                                {"mmcssRegistered", timing.mmcssRegistered},
                                {"mmcssError", timing.mmcssError},
                                {"maximumLatenessNs", timing.maximumLatenessNs},
                                {"maximumDrainNs", timing.maximumDrainNs},
                                {"maximumRenderNs", timing.maximumRenderNs},
                                {"maximumReadNs", timing.maximumReadNs},
                                {"maximumWriteNs", timing.maximumWriteNs},
                                {"deadlineMisses", timing.deadlineMisses},
                                {"lastDeadlineMissNs", timing.lastDeadlineMissNs},
                                {"ingressPackets", timing.ingressPackets},
                                {"ingressFrames", timing.ingressFrames},
                                {"ingressSkippedPackets", timing.ingressSkippedPackets},
                                {"ingressSequenceResets", timing.ingressSequenceResets},
                                {"maximumIngressPublicationLagNs",
                                 timing.maximumIngressPublicationLagNs},
                                {"maximumIngressPublicationGapNs",
                                 timing.maximumIngressPublicationGapNs},
                                {"packetDiscontinuities", timing.packetDiscontinuities},
                                {"timestampResets", timing.timestampResets},
                                {"completedIngressZeroRuns", timing.completedIngressZeroRuns},
                                {"maximumCompletedIngressZeroFrames",
                                 timing.maximumCompletedIngressZeroFrames}};
                            row["microphone"]["transportTiming"]["streamingChunksEnabled"] =
                                timing.streamingChunksEnabled;
                            row["microphone"]["transportTiming"]["publicationFlowVersion"] =
                                timing.publicationFlowVersion;
                            row["microphone"]["transportTiming"]["publicationBatchFrames"] =
                                timing.publicationBatchFrames;
                            row["microphone"]["transportTiming"]["maximumChunkRenderNs"] =
                                timing.maximumChunkRenderNs;
                            row["microphone"]["transportTiming"]["restartAttempts"] =
                                timing.restartAttempts;
                            row["microphone"]["transportTiming"]["outgoingFrames"] =
                                timing.outgoingFrames;
                            row["microphone"]["transportTiming"]["outgoingAllZeroFrames"] =
                                timing.outgoingAllZeroFrames;
                            row["microphone"]["transportTiming"]["publicationWaits"] =
                                timing.publicationWaits;
                            row["microphone"]["transportTiming"]["publicationWaitTimeouts"] =
                                timing.publicationWaitTimeouts;
                            row["microphone"]["transportTiming"]["maximumPublicationWaitNs"] =
                                timing.maximumPublicationWaitNs;
                            row["microphone"]["transportTiming"]["publicationWaitEnabled"] =
                                timing.publicationWaitEnabled;
                            row["microphone"]["transportTiming"]["driverQueue"] = {
                                {"available", timing.driverQueueAvailable},
                                {"missingFrames", timing.driverMissingFrames},
                                {"droppedFrames", timing.driverDroppedFrames},
                                {"underruns", timing.driverUnderruns},
                                {"lastUnderrunTime100ns", timing.driverLastUnderrunTime100ns},
                                {"recordingFrames", timing.driverRecordingFrames},
                                {"leaseSilentFrames", timing.driverLeaseSilentFrames},
                                {"sampleTime100ns", timing.driverSampleTime100ns},
                                {"queuedFrames", timing.driverQueuedFrames},
                                {"ingressPackets", timing.driverIngressPackets},
                                {"lastUnderrunQueued", timing.driverLastUnderrunQueued},
                                {"lastUnderrunRequested", timing.driverLastUnderrunRequested}};
                            const auto monitor = output->cable->program()->status();
                            row["microphone"]["publicationRead"] = {
                                {"callbacks", monitor.callbacks},
                                {"missingFrames", monitor.missingFrames},
                                {"graphFailures", monitor.graphFailures},
                                {"invalidBlocks", monitor.invalidBlocks}};
                            const auto ingressHistory = output->cable->playback();
                            const auto read = ingressHistory->readDiagnostics();
                            const auto inputStatus = ingressHistory->status();
                            row["microphone"]["inputHistory"] = {
                                {"sampledAtHostNs", nowNs()},
                                {"publishedBlockEndHostNs",
                                 ingressHistory->publishedBlockEndHostNs()},
                                {"enabled", read.enabled},
                                {"reads", read.reads},
                                {"requestedFrames", read.requestedFrames},
                                {"resampleBlockCounts", read.resampleBlockCounts},
                                {"missingByReason", read.missingFrames},
                                {"unpublishedSampleReads", read.unpublishedSampleReads},
                                {"expiredSampleReads", read.expiredSampleReads},
                                {"racingSampleReads", read.racingSampleReads},
                                {"maximumVerifiedClockStepNs", read.maximumVerifiedClockStepNs},
                                {"verifiedClockSteps", read.verifiedClockSteps},
                                {"callbacks", inputStatus.callbacks},
                                {"discontinuities", inputStatus.discontinuities},
                                {"measuredRate", inputStatus.measuredRate}};
                        }
                    }
                if (sinks.contains(d.id)) {
                    const auto busHistory = std::dynamic_pointer_cast<const AudioHistory>(
                        sinks.at(d.id)->bus->source());
                    if (busHistory) {
                        const auto read = busHistory->readDiagnostics();
                        row["microphone"]["publicationRead"]["history"] = {
                            {"enabled", read.enabled},
                            {"reads", read.reads},
                            {"requestedFrames", read.requestedFrames},
                            {"resampleBlockCounts", read.resampleBlockCounts},
                            {"missingByReason", read.missingFrames},
                            {"unpublishedSampleReads", read.unpublishedSampleReads},
                            {"expiredSampleReads", read.expiredSampleReads},
                            {"racingSampleReads", read.racingSampleReads},
                            {"absentSampleReads", read.absentSampleReads}};
                        row["busLastHostNs"] = busHistory->status().lastHostNs;
                        row["busDiscontinuities"] = busHistory->status().discontinuities;
                        row["busLagNs"] = nowNs() - busHistory->status().lastHostNs;
                    }
                }
                for (size_t s = 0; s < d.routing.sources.size(); ++s) {
                    const auto& b = *std::find_if(d.bindings.begin(), d.bindings.end(),
                                                  [&](const auto& binding) {
                                                      return binding.id == d.routing.sources[s].id;
                                                  });
                    const auto status = scene->sourceStatus(index, s);
                    AudioMeterSnapshot level;
                    row["sources"].push_back(
                        {{"id", b.id},
                         {"online", status.online},
                         {"missingFrames", status.missingFrames},
                         {"meter", meter(level, scene->sourceMeters(index, s, level))}});
                    if (const auto* input = scene->input(index, s); input && input->source)
                        if (const auto* timed =
                                dynamic_cast<const TimedSource*>(input->source.get()))
                            row["sources"].back()["readTiming"] = timed->timing();
                    sources.push_back(
                        {{"kind", b.kind == SourceKind::Application   ? "application"
                                  : b.kind == SourceKind::InputDevice ? "input-device"
                                  : b.kind == SourceKind::PassThrough ? "pass-through"
                                                                      : "virtual-device"},
                         {"selector", b.selector},
                         {"connected", status.online},
                         {"complete", status.online},
                         {"detail",
                          sourceErrors.contains(b.selector) ? sourceErrors.at(b.selector) : ""}});
                    if (b.kind == SourceKind::Application) {
                        bool verified = false;
                        const auto provider = processes.find(b.selector);
                        if (b.muteOriginal && provider != processes.end()) {
                            const auto attempt =
                                routingAttempts.find(provider->second->application());
                            verified = attempt != routingAttempts.end() &&
                                       attempt->second.verified && provider->second->readyToRoute();
                        }
                        sources.back()["playbackRouting"] = {{"requested", b.muteOriginal},
                                                             {"verified", verified}};
                    }
                }
                meters.push_back(row);
            }
        for (const auto& o : outputs) {
            AudioMeterSnapshot level;
            const bool connected = o->connected();
            auto* program = o->activeProgram();
            const bool metered = program && program->meters(level);
            monitors.push_back(
                {{"device", o->device},
                 {"monitor", o->id},
                 {"publication", o->publication},
                 {"connected", connected},
                 {"complete", connected},
                 {"outputLeadNs", o->cable ? int64_t(20000000) : o->outputLeadNs.load()},
                 {"queueFrames", program ? program->timing().delayFrames : 0},
                 {"outputStreamLatency100ns",
                  o->host.status().diagnostics.outputStreamLatency100ns},
                 {"detail", o->cable ? o->cable->error() : o->error},
                 {"callbacks", o->cable ? program->status().callbacks : o->host.status().callbacks},
                 {"renderFailures",
                  o->cable ? program->status().graphFailures : o->host.status().renderFailures},
                 {"missingFrames", program ? program->status().missingFrames : 0},
                 {"meter", meter(level, metered)}});
        }
        const bool restored = runtimeError.empty() && sourceErrors.empty() &&
                              std::all_of(outputs.begin(), outputs.end(), [](const auto& output) {
                                  return output->connected();
                              });
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool cpuTimingAvailable =
            render.joinable() &&
            GetThreadTimes(render.native_handle(), &created, &exited, &kernel, &user);
        const auto ticks = [](FILETIME value) {
            return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        };
        const auto threadCpuNs = cpuTimingAvailable ? (ticks(kernel) + ticks(user)) * 100 : 0;
        JSON renderDomains = JSON::array();
        if (scene)
            for (const auto& domain : scene->domains()) {
                const auto& timing = *domain.timing;
                renderDomains.push_back(
                    {{"sampleRate", domain.rate},
                     {"calls", timing.calls.load(std::memory_order_relaxed)},
                     {"totalNs", timing.totalNs.load(std::memory_order_relaxed)},
                     {"maximumNs", timing.maximumNs.load(std::memory_order_relaxed)},
                     {"maximumLatenessNs",
                      timing.maximumLatenessNs.load(std::memory_order_relaxed)}});
            }
        return {
            {"version", 1},
            {"connection", {{"connected", true}, {"ready", true}}},
            {"platform", "windows"},
            {"background",
             {{"login",
               {{"available", true},
                {"registered", loginStartupEnabled()},
                {"detail", "Windows Startup Apps can also disable automatic startup."}}}}},
            {"previewNotice",
             endpointRoles.empty()
                 ? "Windows preview: microphone and app mixing with hardware monitoring. A virtual "
                   "microphone driver is not installed."
                 : "Windows preview: each installed mix publishes its own microphone. "
                   "Saved mix changes update microphones automatically."},
            {"inventory",
             {{"devices", devices}, {"applications", applicationsJSON}, {"devicesFresh", true}}},
            {"snapshot",
             {{"version", 1},
              {"revision", {{"epoch", epoch}, {"generation", std::to_string(generation)}}},
              {"document", encodeDocument(document)},
              {"canCreateDocument", false},
              {"history",
               {{"canUndo", history.canUndo(encodeDocument(document).dump())},
                {"canRedo", history.canRedo(encodeDocument(document).dump())}}},
              {"restoration",
               {{"phase", !runtimeError.empty() ? "retry" : (restored ? "restored" : "partial")},
                {"desiredGeneration", std::to_string(generation)},
                {"activeGeneration", std::to_string(activeGeneration)}}},
              {"runtime",
               {{"metersFresh", true},
                {"renderTiming",
                 {{"domains", renderDomains},
                  {"calls", renderCalls.load()},
                  {"threadId", render.joinable() ? GetThreadId(render.native_handle()) : 0},
                  {"threadCpuTimingAvailable", cpuTimingAvailable},
                  {"threadCpuNs", threadCpuNs},
                  {"mmcssRegistered", renderMmcssRegistered.load()},
                  {"mmcssError", renderMmcssError.load()},
                  {"highResolutionTimer", renderHighResolutionTimer.load()},
                  {"timerCreationError", renderTimerCreationError.load()},
                  {"totalNs", renderTotalNs.load()},
                  {"maximumNs", renderMaximumNs.load()},
                  {"maximumLatenessNs", renderMaximumLatenessNs.load()},
                  {"reanchors", renderReanchors.load()},
                  {"skippedFrames", renderSkippedFrames.load()}}},
                {"restored", restored},
                {"sampleRates", kVirtualSampleRates},
                {"captureDiagnostics", captureDiagnostics},
                {"applicationCaptures", applicationCaptures},
                {"editCapabilities", {{"removeOutputPair", true}, {"appOutputRouting", true}}},
                {"devices", meters},
                {"sources", sources},
                {"monitors", monitors}}}}},
            {"microphoneSetup", microphoneSetupState()},
            {"applicationRoutingRecovery", routingRecoveryState},
            {"diagnostics", {{"load", loadError}, {"runtime", runtimeError}}}};
    }
};

Runtime::Runtime(std::filesystem::path storage, bool manageMicrophones)
    : impl_(std::make_unique<Impl>(std::move(storage), manageMicrophones)) {}
Runtime::~Runtime() = default;
void Runtime::recoverAudio() {
    auto& self = *impl_;
    self.recoverAudio();
    const auto now = std::chrono::steady_clock::now();
    if (!self.routingRecoveryEnabled || now < self.nextRoutingRecovery)
        return;
    self.nextRoutingRecovery = now + std::chrono::seconds(3);
    try {
        if (!self.routingStore) {
            const auto intent =
                self.savedDocumentAuthoritative && self.wantsApplicationOutputRouting()
                    ? AppRoutingStorageIntent::InitializeForRequest
                    : AppRoutingStorageIntent::RecoverOnly;
            self.routingStore = openAppRoutingStore(appRoutingStoreRoot(), intent);
            if (!self.routingStore)
                self.routingRecoveryState = {
                    {"examined", false}, {"pending", 0}, {"error", ""}, {"storage", "absent"}};
        }
        if (self.routingStore) {
            AppAudioPolicy policy; // Construct/use/destroy on this MTA owner thread.
            self.routingRecoveryState =
                recoverApplicationOutputs(self.routingStore->journal(), policy);
            if (self.routingRecoveryState.value("examined", false))
                self.routingRecoveryState["acquisition"] =
                    acquireApplicationOutputs(self.routingStore->journal(), policy);
            self.routingRecoveryState["pending"] = self.routingStore->journal().snapshot().size();
            self.routingRecoveryState["storage"] = "available";
        }
    } catch (const std::exception& error) {
        self.routingRecoveryState["examined"] = false;
        self.routingRecoveryState["error"] = error.what();
    }
    self.reconcileRoutingSink();
}
JSON Runtime::acquireApplicationOutputs(AppRoutingJournal& journal, AppAudioPolicy& policy) {
    auto& self = *impl_;
    JSON result{{"examined", false}, {"error", ""}, {"applications", JSON::array()}};
    if (!self.savedDocumentAuthoritative) {
        result["error"] = "App routing is waiting for an authoritative saved document";
        return result;
    }
    if (!self.wantsApplicationOutputRouting()) {
        const auto held = journal.snapshot();
        std::erase_if(self.routingAttempts, [&](const auto& entry) {
            return std::none_of(held.begin(), held.end(), [&](const auto& receipt) {
                return receipt.application == entry.first;
            });
        });
        result["examined"] = true;
        result["pendingReceipts"] = held.size();
        return result;
    }
    // No partial endpoint/session inventory may authorize an output setter.
    const auto sessions = appRoutingSessions();
    const auto sink = observeAppRoutingSink(self.ownerSid);
    if (sink.endpoint.empty() || !sink.endpointsExamined || !sink.defaultsExamined ||
        sink.defaultEndpoint || !sink.error.empty()) {
        result["error"] =
            sink.error.empty() ? "App routing sink is unavailable or unsafe" : sink.error;
        return result;
    }
    const auto target = utf8(AppAudioPolicy::renderInterface(sink.endpoint));
    if (!policy.available()) {
        result["error"] = policy.error();
        return result;
    }
    const auto freshApps = applications(true);
    std::map<std::string, std::vector<std::shared_ptr<AppRoutingProcess>>> desired;
    std::map<std::string, std::shared_ptr<ProcessCaptureGroup>> captures;
    std::map<std::string, std::string> unavailable;
    for (const auto& device : self.document.devices) {
        if (!device.routing.enabled)
            continue;
        for (const auto& binding : device.bindings) {
            if (binding.kind != SourceKind::Application || !binding.muteOriginal)
                continue;
            const auto source = std::find_if(device.routing.sources.begin(),
                                             device.routing.sources.end(), [&](const auto& value) {
                                                 return value.id == binding.id;
                                             });
            if (source == device.routing.sources.end() || !source->enabled)
                continue;
            const auto members = captureMembers(freshApps, binding.selector);
            if (members.empty()) {
                result["applications"].push_back({{"selector", binding.selector},
                                                  {"phase", "waiting-for-application"},
                                                  {"error", ""}});
                continue;
            }
            const auto application = members.front()->identity().application;
            desired[application] = members;
            const auto prepared = self.processes.find(binding.selector);
            const bool publicationReady =
                self.scene && self.activeGeneration == self.generation &&
                std::any_of(self.outputs.begin(), self.outputs.end(), [&](const auto& output) {
                    return output->device == device.id && output->publication &&
                           output->connected();
                });
            if (prepared == self.processes.end() || !prepared->second->matches(members) ||
                !prepared->second->readyToRoute() || !publicationReady)
                unavailable[application] = "Complete application capture is not ready";
            else
                captures[application] = prepared->second;
        }
    }
    const auto retainedReceipts = journal.snapshot();
    std::erase_if(self.routingAttempts, [&](const auto& entry) {
        return !desired.contains(entry.first) &&
               std::none_of(retainedReceipts.begin(), retainedReceipts.end(),
                            [&](const auto& receipt) {
                                return receipt.application == entry.first;
                            });
    });
    AppRoutingLease lease(journal);
    const auto findReceipt =
        [&](const std::string& application) -> std::optional<AppRoutingReceipt> {
        const auto all = journal.snapshot();
        const auto found = std::find_if(all.begin(), all.end(), [&](const auto& value) {
            return value.application == application;
        });
        return found == all.end() ? std::nullopt : std::optional<AppRoutingReceipt>(*found);
    };
    for (const auto& [application, members] : desired) {
        JSON row{{"application", application}, {"phase", "waiting-for-capture"}, {"error", ""}};
        auto& attempt = self.routingAttempts[application];
        try {
            std::vector<std::unique_ptr<ProcessRoutingPreferences>> access;
            std::vector<ProcessInstance> membership;
            for (const auto& member : members) {
                member->validate();
                const auto& identity = member->identity();
                access.push_back(std::make_unique<ProcessRoutingPreferences>(
                    policy, identity.pid, identity.reportedImage, application));
                membership.push_back({identity.pid, identity.created100ns});
            }
            std::sort(membership.begin(), membership.end(), [](const auto& a, const auto& b) {
                return a.id < b.id;
            });
            const auto receipt = findReceipt(application);
            const auto rollback = [&] {
                for (auto& member : access)
                    lease.release(*member, AppRoutingReleaseMode::RetainVerified);
                attempt.verified = false;
                attempt.deadlineNs = 0;
                attempt.retryAfterNs = nowNs() + 30000000000ULL;
            };
            if (receipt && (receipt->phase == AppRoutingReceiptPhase::UserOverride ||
                            receipt->phase == AppRoutingReceiptPhase::Releasing)) {
                row["phase"] = "waiting-for-restoration";
            } else if (unavailable.contains(application)) {
                if (receipt)
                    rollback();
                row["error"] = unavailable.at(application);
            } else if (nowNs() < attempt.retryAfterNs) {
                row["phase"] = "retry-backoff";
            } else {
                const auto& capture = captures.at(application);
                // Verify every process before the first mutation. A first lease
                // cannot merge different per-instance originals into one receipt.
                const auto baseline = access.front()->read();
                if (!receipt &&
                    std::find(baseline.begin(), baseline.end(), target) != baseline.end())
                    throw std::runtime_error(
                        "App output already points to the sink without a recovery receipt");
                for (auto& member : access)
                    if (!receipt && member->read() != baseline)
                        throw std::runtime_error(
                            "Application instances have different output preferences");
                bool streamsMoved = true;
                for (const auto& member : members) {
                    bool active = false;
                    for (const auto& session : sessions)
                        if (session.pid == member->identity().pid && session.active) {
                            active = true;
                            streamsMoved &= session.endpoint == sink.endpoint;
                        }
                    streamsMoved &= active;
                }
                bool preferencesOwned =
                    receipt && receipt->phase == AppRoutingReceiptPhase::Applied;
                for (auto& member : access)
                    for (const auto& value : member->read())
                        preferencesOwned &= value == target;
                const bool sameAttempt =
                    attempt.membership == membership && attempt.endpoint == sink.endpoint;
                if (receipt && preferencesOwned && streamsMoved && capture->readyToRoute()) {
                    attempt = {membership, sink.endpoint, 0, 0, true};
                    row["phase"] = "routed";
                } else if (sameAttempt && attempt.deadlineNs && nowNs() >= attempt.deadlineNs) {
                    rollback();
                    row["phase"] = "restoring-after-timeout";
                    row["error"] = "Application streams did not move to the routing sink";
                } else if (sameAttempt && attempt.deadlineNs) {
                    row["phase"] = "waiting-for-streams";
                } else {
                    // Hardware identity and capture membership must survive the
                    // preparation reads before any preference is changed.
                    const auto currentSink = observeAppRoutingSink(self.ownerSid);
                    if (currentSink.endpoint != sink.endpoint || currentSink.defaultEndpoint ||
                        !currentSink.defaultsExamined || !currentSink.endpointsExamined ||
                        !currentSink.error.empty() || !capture->matches(members) ||
                        !capture->readyToRoute())
                        throw std::runtime_error("Application routing preparation changed");
                    attempt = {membership, sink.endpoint, nowNs() + 10000000000ULL, 0, false};
                    bool overridden = false;
                    for (auto& member : access)
                        if (lease.apply(*member, target,
                                        AppRoutingOverrideMode::RetainUntilDesiredStateSaved) ==
                            AppRoutingApplyResult::UserOverride) {
                            overridden = true;
                            break;
                        }
                    if (overridden) {
                        for (auto& member : access)
                            lease.releaseOverride(*member, AppRoutingReleaseMode::RetainVerified);
                        acknowledgeApplicationOutputOverride(*access.front());
                        row["phase"] = "user-override-saved";
                    } else
                        row["phase"] = "waiting-for-streams";
                }
            }
        } catch (const std::exception& error) {
            row["phase"] = "routing-pending";
            row["error"] = error.what();
        }
        result["applications"].push_back(std::move(row));
    }
    result["examined"] = true;
    result["pendingReceipts"] = journal.snapshot().size();
    return result;
}
JSON Runtime::recoverApplicationOutputs(AppRoutingJournal& journal, AppAudioPolicy& policy) {
    auto& self = *impl_;
    JSON result = {{"examined", false},
                   {"controllerPid", GetCurrentProcessId()},
                   {"pending", journal.snapshot().size()},
                   {"error", ""},
                   {"applications", JSON::array()}};
    if (!self.savedDocumentAuthoritative) {
        result["error"] = "Routing recovery is waiting for an authoritative saved document";
        return result;
    }
    const auto sessions = appRoutingSessions();
    const auto occupants = [](const auto& values) {
        JSON entries = JSON::array();
        for (const auto& value : values)
            entries.push_back({{"pid", value.pid},
                               {"endpoint", value.endpoint},
                               {"active", value.active},
                               {"creatorPid", value.creatorPid},
                               {"shared", value.shared},
                               {"systemSounds", value.systemSounds}});
        return entries;
    };
    // Sink lifetime uses every occupant, independently of per-application
    // receipt completion. An empty journal is not permission to remove a sink.
    result["endpointOccupants"] = occupants(sessions);
    const auto receipts = journal.snapshot();
    if (receipts.empty()) {
        result["examined"] = true;
        result["pending"] = 0;
        return result;
    }
    if (!policy.available())
        throw std::runtime_error(policy.error());
    std::map<uint32_t, std::unique_ptr<ProcessRoutingPreferences>> candidates;
    std::set<uint32_t> seen;
    for (const auto& session : sessions) {
        if (!session.pid || !seen.insert(session.pid).second)
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, session.pid);
        if (!process)
            continue; // No authenticated process means no receipt can be acknowledged for it.
        wchar_t path[32768]{};
        DWORD length = 32768;
        const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!queried)
            continue;
        try {
            candidates.emplace(session.pid, std::make_unique<ProcessRoutingPreferences>(
                                                policy, session.pid,
                                                std::filesystem::path(std::wstring(path, length))));
        } catch (const std::exception&) {
            // Other users/sessions, exited processes and inaccessible packaged
            // identities cannot authorize mutation or acknowledgment.
        }
    }
    AppRoutingLease lease(journal);
    for (const auto& receipt : receipts) {
        JSON item = {{"application", receipt.application},
                     {"phase", "waiting-for-application"},
                     {"error", ""}};
        try {
            std::vector<ProcessRoutingPreferences*> members;
            for (const auto& [pid, access] : candidates) {
                (void)pid;
                if (access->application() == receipt.application)
                    members.push_back(access.get());
            }
            if (!members.empty()) {
                bool desired = false;
                for (const auto& device : self.document.devices)
                    for (const auto& binding : device.bindings)
                        if (binding.muteOriginal &&
                            matchesRoutingApplication(binding, *members.front())) {
                            const auto source =
                                std::find_if(device.routing.sources.begin(),
                                             device.routing.sources.end(), [&](const auto& value) {
                                                 return value.id == binding.id;
                                             });
                            desired = desired ||
                                      (device.routing.enabled &&
                                       source != device.routing.sources.end() && source->enabled);
                        }
                bool external = receipt.phase == AppRoutingReceiptPhase::UserOverride;
                if (desired && receipt.phase == AppRoutingReceiptPhase::Applied)
                    for (auto* access : members)
                        for (const auto& value : access->read())
                            external =
                                external || std::find(receipt.attemptedTargets.begin(),
                                                      receipt.attemptedTargets.end(),
                                                      value) == receipt.attemptedTargets.end();
                const bool releasing = receipt.phase == AppRoutingReceiptPhase::Releasing;
                if (desired && !external && !releasing) {
                    item["phase"] = receipt.phase == AppRoutingReceiptPhase::Prepared
                                        ? "waiting-for-route-retry"
                                        : "owned";
                } else {
                    // Restore every authenticated process sharing this durable
                    // identity before acknowledging the shared receipt.
                    for (auto* access : members) {
                        if (external)
                            lease.releaseOverride(*access, AppRoutingReleaseMode::RetainVerified);
                        else if (lease.release(*access, AppRoutingReleaseMode::RetainVerified) ==
                                     AppRoutingReleaseResult::PreservedUserChange &&
                                 desired)
                            external = true;
                    }
                    if (external)
                        for (auto* access : members)
                            lease.releaseOverride(*access, AppRoutingReleaseMode::RetainVerified);
                    const auto released = journal.snapshot();
                    const auto restored =
                        std::find_if(released.begin(), released.end(), [&](const auto& value) {
                            return value.application == receipt.application;
                        });
                    if (restored == released.end())
                        throw std::runtime_error("Application recovery receipt changed");
                    const auto expectedRevision = restored->revision;
                    if (external)
                        acknowledgeApplicationOutputOverride(*members.front());
                    // Fresh observation after setters: an explicit-endpoint or
                    // slow-moving stream must retain the receipt and sink.
                    const auto after = appRoutingSessions();
                    result["endpointOccupants"] = occupants(after);
                    bool stillOwned = false;
                    for (const auto& session : after) {
                        const auto target = utf8(AppAudioPolicy::renderInterface(session.endpoint));
                        if (std::find(receipt.attemptedTargets.begin(),
                                      receipt.attemptedTargets.end(),
                                      target) != receipt.attemptedTargets.end()) {
                            const bool member = std::any_of(
                                members.begin(), members.end(), [&](const auto* access) {
                                    return access->processIdentity().pid == session.pid;
                                });
                            const auto identified = candidates.find(session.pid);
                            const bool otherApplication =
                                identified != candidates.end() &&
                                identified->second->application() != receipt.application;
                            // Only proven-dead inactive clients and classified
                            // inactive system sounds can be excluded without an
                            // authenticated application identity. Inaccessible
                            // live and ambiguous shared clients remain blockers.
                            if (member ||
                                (!otherApplication && appRoutingSessionMayOwnStream(session))) {
                                stillOwned = true;
                                item["blockingSessions"].push_back(
                                    {{"pid", session.pid},
                                     {"endpoint", session.endpoint},
                                     {"active", session.active},
                                     {"member", member},
                                     {"creatorPid", session.creatorPid},
                                     {"shared", session.shared},
                                     {"systemSounds", session.systemSounds}});
                            }
                        }
                    }
                    if (stillOwned) {
                        item["phase"] = "waiting-for-stream-restoration";
                    } else {
                        // Process and preference verification remain necessary
                        // even if the app has just closed its final session.
                        for (auto* access : members) {
                            const auto actions = appRoutingRestoration(receipt, access->read());
                            if (std::find(actions.begin(), actions.end(),
                                          AppRoutingRestore::RestoreOriginal) != actions.end())
                                throw std::runtime_error(
                                    "Application output restoration is pending");
                        }
                        if (!journal.forget(receipt.application, expectedRevision))
                            throw std::runtime_error(
                                "Application recovery acknowledgment is stale");
                        item["phase"] = external ? "user-override-saved" : "released";
                    }
                }
            }
        } catch (const std::exception& error) {
            item["phase"] = "recovery-pending";
            item["error"] = error.what();
        }
        result["applications"].push_back(std::move(item));
    }
    result["examined"] = true;
    result["pending"] = journal.snapshot().size();
    return result;
}
bool Runtime::acknowledgeApplicationOutputOverride(ProcessRoutingPreferences& preferences) {
    auto& self = *impl_;
    if (!self.savedDocumentAuthoritative)
        throw std::runtime_error(
            "Cannot save an app output override without authoritative routing");
    const auto application = preferences.application(); // Validates pinned user/session/process.
    auto candidate = self.document;
    bool changed = false;
    for (auto& device : candidate.devices) {
        for (auto& binding : device.bindings) {
            if (binding.kind != SourceKind::Application || !binding.muteOriginal)
                continue;
            if (matchesRoutingApplication(binding, preferences)) {
                binding.muteOriginal = false;
                changed = true;
            }
        }
    }
    if (!changed)
        return false;
    if (preferences.application() != application)
        throw std::runtime_error("App output override identity changed before saving");
    const auto before = encodeDocument(self.document).dump();
    auto history = self.history.withEdit(before, encodeDocument(candidate).dump());
    return self.commitDocument(std::move(candidate), std::move(history));
}
JSON Runtime::request(const JSON& request) {
    auto& self = *impl_;
    try {
        if (!request.is_object() || request.at("version") != 1)
            throw std::invalid_argument("Unsupported desktop request");
        const auto operation = request.at("operation").get<std::string>();
        if (operation == "state")
            return self.state();
        if (operation == "login") {
            setLoginStartup(request.at("enabled").get<bool>());
            return {{"ok", true}};
        }
        if (operation == "menu-context")
            return {{"ok", true}};
        if (operation == "retry" || operation == "retry-session") {
            self.submitMicrophones();
            self.refresh();
            self.rebuild();
            return {{"ok", true}};
        }
        if (operation != "edit" && operation != "initialize")
            return {{"ok", false},
                    {"code", "unsupported"},
                    {"detail", "This operation is not available in the Windows preview."}};
        const auto& edit = request.at("request");
        if (edit.at("version") != 1 || edit.at("expected").at("epoch") != self.epoch ||
            edit.at("expected").at("generation") != std::to_string(self.generation))
            return {{"ok", false}, {"code", "conflict"}};
        const auto before = encodeDocument(self.document).dump();
        RouteDocument candidate;
        auto nextHistory = self.history;
        if (edit.contains("history")) {
            if (edit.contains("commands"))
                throw std::invalid_argument("Cannot combine history and edits");
            const auto direction = edit.at("history").get<std::string>();
            if (direction != "undo" && direction != "redo")
                throw std::invalid_argument("Unknown history operation");
            nextHistory =
                direction == "undo" ? self.history.withUndo(before) : self.history.withRedo(before);
            candidate = decodeDocument(parseControlJSON(nextHistory.current()));
        } else {
            const auto commands = decodeCommands(edit.at("commands"));
            candidate = edit::applyCommands(self.document, commands);
            for (const auto& d : candidate.devices) {
                if (!supportedVirtualSampleRate(d.sampleRate) ||
                    !loopz_driver::AudioFormat{static_cast<unsigned>(d.sampleRate),
                                               d.routing.outputChannels}
                         .valid())
                    throw std::invalid_argument("Unsupported Windows microphone format");
            }
            nextHistory = self.history.withEdit(before, encodeDocument(candidate).dump());
        }
        self.commitDocument(std::move(candidate), std::move(nextHistory));
        return {{"ok", true}};
    } catch (const std::exception& e) {
        return {{"ok", false}, {"code", "invalid"}, {"detail", e.what()}};
    }
}
} // namespace loopz::windows
