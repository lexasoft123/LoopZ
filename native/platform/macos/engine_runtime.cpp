#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/capture_sources.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/macos/device_settings.hpp>
#include <loopz/macos/input_timing.hpp>
#include <loopz/application_timing.hpp>
#include <sys/stat.h>
#include <uuid/uuid.h>

#include <cerrno>
#include <algorithm>
#include <utility>

namespace loopz::macos {
namespace {
std::string editorEpoch() {
    uuid_t value;
    uuid_generate_random(value);
    uuid_string_t text;
    uuid_unparse_lower(value, text);
    return text;
}
class ValidationSource final : public SourceEndpoint {
  public:
    explicit ValidationSource(uint32_t count) : count_(count) {}
    uint32_t channels() const noexcept override {
        return count_;
    }
    bool open(SourceOpenPolicy, std::string& error) override {
        error = "Capture is disabled during installation validation";
        return false;
    }
    void close() noexcept override {}
    bool healthy(std::string&) override {
        return false;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0);
        return {frames, false};
    }

  private:
    const uint32_t count_;
};
struct FileStamp {
    uint64_t device{0}, inode{0}, bytes{0};
    int64_t modified{0}, modifiedNs{0}, changed{0}, changedNs{0};
    int error{0};
    bool operator==(const FileStamp&) const = default;
};
FileStamp stamp(const std::filesystem::path& path) {
    struct stat value{};
    if (lstat(path.c_str(), &value))
        return {0, 0, 0, 0, 0, 0, 0, errno};
    return {static_cast<uint64_t>(value.st_dev),  static_cast<uint64_t>(value.st_ino),
            static_cast<uint64_t>(value.st_size), value.st_mtimespec.tv_sec,
            value.st_mtimespec.tv_nsec,           value.st_ctimespec.tv_sec,
            value.st_ctimespec.tv_nsec,           0};
}
} // namespace
struct EngineRuntime::Impl {
    DriverConfigurationLink& link;
    const EngineRuntimeOptions options;
    DeviceSettings deviceSettings;
    SourceRegistry sources;
    MonitorOutputs monitors;
    SceneRenderer renderer;
    std::shared_ptr<PublicPassThroughTiming> publicTiming;
    std::unique_ptr<ConfigurationController> controller;
    std::unique_ptr<EditorController> editor;
    RenderThread render;
    GraphRecovery renderRecovery;
    bool started{false};
    uint64_t nextFilePoll{0};
    uint64_t renderEpochNs{0}, hardwareBufferGeneration{0};
    std::string documentError, controlError;
    std::array<FileStamp, 2> loadedStamp{};
    bool fileLoaded{false};
    uint64_t rateSession{0}, nextRatePoll{0};
    std::vector<DriverRateRequest> acceptedRates;
    const std::vector<DeviceTransport> emptyDevices;
    Impl(DriverConfigurationLink& connection, EngineRuntimeOptions config,
         SourceRegistry::Factory capture, MonitorOutputs::HostFactory output)
        : link(connection), options(std::move(config)),
          deviceSettings(options.installationValidation ? std::filesystem::path{}
                                                        : options.deviceSettingsPath),
          sources(
              options.installationValidation
                  ? SourceRegistry::Factory{[](SourceKind, const std::string&, uint32_t channels) {
                        return std::make_shared<ValidationSource>(channels);
                    }}
              : capture
                  ? std::move(capture)
                  : captureSourceFactory({options.applicationBufferFrames,
                                          options.captureDeliveryDiagnostics, options.inputFilter}),
              [] {
                  return hostNowNs() / 1000000;
              }),
          monitors(
              options.scene.renderRate, options.monitorDelayFrames, std::move(output),
              [] {
                  return hostNowNs() / 1000000;
              },
              options.scene.blockFrames, options.monitorPeriodReserve),
          renderer(options.scene, sources,
                   [this](const auto& device, auto sink) {
                       if (options.installationValidation)
                           return sink;
                       return monitors.decorate(device, std::move(sink));
                   }),
          render(renderer, {options.scene.renderRate, options.scene.blockFrames,
                            options.renderLeadFrames, options.maximumCatchupFrames}) {
        renderRecovery.setDesired({{"renderer", {}}}, 0);
        (void)AudioScene({}, options.scene, {}, {});
        if (options.inputDelayFrames && *options.inputDelayFrames > kCapacity / 2)
            throw std::invalid_argument("Physical input delay exceeds history capacity");
        if (options.automaticPassThroughTiming && !options.installationValidation) {
            publicTiming = std::make_shared<PublicPassThroughTiming>(
                options.scene, options.publicTimingProbe, options.renderLeadFrames);
            renderer.setTimingFloors([timing = publicTiming](const auto& document) {
                return timing->floors(document);
            });
        }
        SceneRenderer::InputTiming physicalTiming;
        if (options.inputDelayFrames && !options.installationValidation) {
            physicalTiming = [delay = *options.inputDelayFrames,
                              filter = options.inputFilter](const auto& document) {
                std::vector<DeviceInputDelay> result;
                for (const auto& input : planPhysicalInputFilters(document, filter))
                    result.push_back({input.device, input.source, delay, input.filter});
                return result;
            };
        } else if (options.automaticInputTiming && !options.installationValidation) {
            auto timing = std::make_shared<PhysicalInputTiming>(
                options.scene, options.renderLeadFrames, PhysicalInputTiming::Probe{},
                options.inputFilter);
            physicalTiming = [this, timing](const auto& document) {
                auto active = renderer.observe();
                // Keep identities needed by the committed scene until a new
                // native acknowledgement actually replaces that scene.
                return timing->delays(document, active ? &active->document() : nullptr);
            };
        } else if (options.inputFilter == ResamplingFilter::MinimumPhase &&
                   !options.installationValidation) {
            // Explicit global budgets remain diagnostic overrides, but must
            // not introduce unlike filter phase into independently sourced mixes.
            physicalTiming = [delay = options.scene.sourceDelayFrames](const auto& document) {
                std::vector<DeviceInputDelay> result;
                for (const auto& input :
                     planPhysicalInputFilters(document, ResamplingFilter::MinimumPhase))
                    result.push_back({input.device, input.source, delay, input.filter});
                return result;
            };
        }
        if (options.automaticApplicationTiming && !options.installationValidation) {
            auto applicationTiming = std::make_shared<ApplicationInputTiming>(
                options.scene, options.renderLeadFrames,
                [this](const auto& selector) {
                    return sources.inputTiming(SourceKind::Application, selector);
                },
                [this](const auto& selector) {
                    sources.rejectInputTiming(SourceKind::Application, selector);
                });
            renderer.setInputTiming(
                [this, physicalTiming, applicationTiming](const auto& document) {
                    auto result =
                        physicalTiming ? physicalTiming(document) : std::vector<DeviceInputDelay>{};
                    auto active = renderer.observe();
                    auto application =
                        applicationTiming->delays(document, active ? &active->document() : nullptr);
                    if (active)
                        retainActiveApplicationTiming(document, *active.get(), application);
                    result.insert(result.end(), application.begin(), application.end());
                    return result;
                });
        } else if (physicalTiming) {
            renderer.setInputTiming(std::move(physicalTiming));
        }
        controller = std::make_unique<ConfigurationController>(link, options.documentPath,
                                                               &renderer, false, 250);
        editor = std::make_unique<EditorController>(*controller, editorEpoch());
        acceptedRates.reserve(kMaximumVirtualDevices);
    }
    void load() {
        if (options.documentPath.empty())
            return;
        const auto before = stamps();
        controller->reloadDesired();
        loadedStamp = before;
        fileLoaded = before == stamps();
        documentError.clear();
    }
    std::array<FileStamp, 2> stamps() const {
        return {stamp(options.documentPath), stamp(options.documentPath.string() + ".backup")};
    }
    void pollRates(uint64_t now) {
        const auto session = link.session();
        if (!session || !controller->desired() || now < nextRatePoll)
            return;
        nextRatePoll = now + 100;
        const auto requests = link.rateRequests();
        if (session != link.session())
            return;
        validateDriverRateRequests(requests);
        if (rateSession != session) {
            acceptedRates.clear();
            rateSession = session;
        }
        if (requests.empty())
            return;
        auto accepted = acceptedRates;
        std::erase_if(accepted, [&](const auto& saved) {
            return std::none_of(requests.begin(), requests.end(), [&](const auto& request) {
                return saved.device == request.device;
            });
        });
        auto desired = *controller->desired();
        bool persist = false;
        for (const auto& request : requests) {
            const auto previous =
                std::find_if(accepted.begin(), accepted.end(), [&](const auto& item) {
                    return item.device == request.device;
                });
            if (previous != accepted.end() && previous->origin == request.origin &&
                previous->sequence >= request.sequence) {
                if (previous->sequence == request.sequence &&
                    previous->sampleRate != request.sampleRate)
                    throw std::runtime_error(
                        "A native rate request changed its value without a new token");
                continue;
            }
            const auto device =
                std::find_if(desired.devices.begin(), desired.devices.end(), [&](const auto& item) {
                    return item.id == request.device;
                });
            if (device != desired.devices.end() &&
                device->formatRequestReceipt != driverRateReceipt(request)) {
                if (device->routing.enabled)
                    device->sampleRate = request.sampleRate;
                device->formatRequestReceipt = driverRateReceipt(request);
                desired.version = kRouteDocumentVersion;
                persist = true;
            }
            if (previous != accepted.end())
                *previous = request;
            else {
                accepted.push_back(request);
            }
        }
        if (persist) {
            if (options.documentPath.empty())
                controller->setDesired(std::move(desired));
            else {
                controller->saveDesired(std::move(desired));
                documentError.clear();
                fileLoaded = false;
            }
        }
        // Save and validate before acknowledging. A failed/lost acknowledgement
        // only retries this token, not the intent change; newer tokens stay pending.
        acceptedRates.swap(accepted);
        if (session == link.session())
            link.acknowledgeRates(requests);
    }
};
EngineRuntime::EngineRuntime(DriverConfigurationLink& link, EngineRuntimeOptions options,
                             SourceRegistry::Factory sources, MonitorOutputs::HostFactory outputs)
    : impl_(std::make_unique<Impl>(link, std::move(options), std::move(sources),
                                   std::move(outputs))) {}
EngineRuntime::~EngineRuntime() {
    stop();
}
void EngineRuntime::start() {
    if (!impl_->started) {
        impl_->started = true;
        impl_->renderRecovery.restart(hostNowNs() / 1000000);
        impl_->nextFilePoll = 0;
        impl_->deviceSettings.reload();
    }
}
void EngineRuntime::stop() noexcept {
    impl_->started = false;
    impl_->render.stop();
    impl_->monitors.stop();
    impl_->sources.stop();
    impl_->renderRecovery.restart(0);
}
void EngineRuntime::poll() {
    auto& self = *impl_;
    if (!self.started)
        return;
    const auto now = hostNowNs() / 1000000;
    if (self.controller && self.controller->desired())
        self.deviceSettings.setRoutingDocument(*self.controller->desired());
    self.deviceSettings.poll(now);
    if (self.hardwareBufferGeneration != self.deviceSettings.hardwareGeneration()) {
        // Providers may retain the old negotiated period after a shared buffer write.
        // Reopen our outputs once per accepted batch; the saved graph is retained.
        self.hardwareBufferGeneration = self.deviceSettings.hardwareGeneration();
        self.monitors.stop();
    }
    if (!self.options.documentPath.empty() && now >= self.nextFilePoll) {
        self.nextFilePoll = now + 1000;
        try {
            if (!self.fileLoaded || self.stamps() != self.loadedStamp)
                self.load();
        } catch (const std::exception& error) {
            self.documentError = error.what();
        }
    }
    try {
        self.link.poll(now);
        self.controlError.clear();
    } catch (const std::exception& error) {
        self.controlError = error.what();
    }
    try {
        self.pollRates(hostNowNs() / 1000000);
    } catch (const std::exception& error) {
        self.controlError = error.what();
    }
    try {
        self.controller->poll(hostNowNs() / 1000000);
    } catch (const std::exception& error) {
        self.controlError = error.what();
    }
    const auto completed = hostNowNs() / 1000000;
    if (self.renderRecovery.ready("renderer") && !self.render.active()) {
        self.render.stop();
        self.renderRecovery.lost("renderer", completed);
    }
    if (self.controller->active())
        for (const auto& attempt : self.renderRecovery.attempts(completed)) {
            bool started = false;
            try {
                self.render.start();
                self.renderEpochNs = hostNowNs();
                started = true;
            } catch (const std::exception& error) {
                self.controlError = error.what();
            }
            if (!self.renderRecovery.complete(attempt, started, hostNowNs() / 1000000))
                self.render.stop();
        }
    try {
        self.renderer.pollSources(hostNowNs() / 1000000);
    } catch (const std::exception& error) {
        self.controlError = error.what();
    }
    {
        auto scene = self.renderer.observe();
        try {
            self.monitors.poll(scene.get(), hostNowNs() / 1000000);
        } catch (const std::exception& error) {
            self.controlError = error.what();
        }
    }
    self.renderer.collect();
}
void EngineRuntime::setEditorPreview(bool enabled) noexcept {
    impl_->renderer.setEditorPreview(enabled && !impl_->options.installationValidation);
}
std::string EngineRuntime::hardwareSnapshot() {
    return impl_->deviceSettings.snapshot(hostNowNs() / 1000000);
}
std::shared_ptr<const PublicTimingObservationBatch>
EngineRuntime::publicTimingObservations() const noexcept {
    return impl_->publicTiming ? impl_->publicTiming->observations() : nullptr;
}
std::string EngineRuntime::applyHardwareBuffer(std::string_view request) {
    if (impl_->options.installationValidation)
        throw std::invalid_argument("Buffer changes are disabled during installation validation");
    if (impl_->controller && impl_->controller->desired())
        impl_->deviceSettings.setRoutingDocument(*impl_->controller->desired());
    return impl_->deviceSettings.apply(request);
}
void EngineRuntime::retryNow() {
    const auto now = hostNowNs() / 1000000;
    impl_->nextFilePoll = 0;
    impl_->nextRatePoll = 0;
    impl_->deviceSettings.retryNow();
    impl_->renderRecovery.retryNow(now);
    impl_->sources.retryNow(now);
    impl_->monitors.retryNow(now);
    if (impl_->controller)
        impl_->controller->retryNow(now);
}
void EngineRuntime::audioServiceReset() {
    auto& self = *impl_;
    if (!self.started)
        return;
    self.render.stop();
    self.monitors.stop();
    self.sources.stop();
    self.renderRecovery.restart(hostNowNs() / 1000000);
    self.renderEpochNs = hostNowNs();
    self.link.invalidate();
    self.rateSession = 0;
    self.acceptedRates.clear();
    if (self.publicTiming)
        self.publicTiming->invalidateNativeIdentities();
    retryNow();
}
void EngineRuntime::setDesired(RouteDocument document, bool save) {
    if (impl_->controller && impl_->controller->desired()) {
        for (auto& device : document.devices)
            for (const auto& previous : impl_->controller->desired()->devices)
                if (previous.id == device.id && !previous.formatRequestReceipt.empty()) {
                    device.formatRequestReceipt = previous.formatRequestReceipt;
                    document.version = kRouteDocumentVersion;
                }
    }
    if (!impl_->controller)
        impl_->load();
    if (save)
        impl_->controller->saveDesired(std::move(document));
    else
        impl_->controller->setDesired(std::move(document));
    if (save) {
        impl_->documentError.clear();
        // Another atomic writer may replace the file immediately after save.
        // Re-read on the next watch tick before accepting its metadata stamp.
        impl_->fileLoaded = false;
    }
}
void EngineRuntime::reloadDesired() {
    impl_->load();
}
EditorSnapshot EngineRuntime::editorSnapshot() const {
    auto snapshot = impl_->editor->snapshot();
    if (!snapshot.document && !impl_->options.documentPath.empty() &&
        !impl_->options.installationValidation) {
        const auto stamps = impl_->stamps();
        snapshot.canCreateDocument = stamps[0].error == ENOENT && stamps[1].error == ENOENT;
    }
    return snapshot;
}
EditorCommit EngineRuntime::applyEditor(const EditorRevision& expected,
                                        std::span<const edit::Command> commands) {
    if (impl_->options.installationValidation)
        throw std::invalid_argument("Editor changes are disabled during installation validation");
    // Observe pending external file changes before comparing generations. A
    // failed reload leaves the last good graph routing but refuses this edit.
    if (!impl_->options.documentPath.empty() &&
        (!impl_->fileLoaded || impl_->stamps() != impl_->loadedStamp))
        impl_->load();
    auto result = impl_->editor->apply(expected, commands);
    if (result.changed) {
        impl_->documentError.clear();
        impl_->fileLoaded = false;
        impl_->nextFilePoll = 0;
    }
    return result;
}
EditorCommit EngineRuntime::initializeEditor(const EditorRevision& expected,
                                             std::span<const edit::Command> commands) {
    const auto before = editorSnapshot();
    if (before.revision != expected || before.document)
        throw StaleEditorRevision();
    if (!before.canCreateDocument)
        throw std::invalid_argument(
            "Existing routing must be recovered before creating a document");
    auto result = impl_->editor->initialize(expected, commands);
    impl_->documentError.clear();
    impl_->fileLoaded = false;
    impl_->nextFilePoll = 0;
    return result;
}
EditorCommit EngineRuntime::moveEditorHistory(const EditorRevision& expected,
                                              EditorHistoryDirection direction) {
    if (impl_->options.installationValidation)
        throw std::invalid_argument("Editor changes are disabled during installation validation");
    if (!impl_->options.documentPath.empty() &&
        (!impl_->fileLoaded || impl_->stamps() != impl_->loadedStamp))
        impl_->load();
    auto result = impl_->editor->moveHistory(expected, direction);
    impl_->documentError.clear();
    impl_->fileLoaded = false;
    impl_->nextFilePoll = 0;
    return result;
}
bool EngineRuntime::monitorMeters(const std::string& device, const std::string& monitor,
                                  AudioMeterSnapshot& output) const noexcept {
    return impl_->monitors.meters(device, monitor, output);
}
SceneRenderer::Observation EngineRuntime::observe() noexcept {
    return impl_->renderer.observe();
}
const std::vector<DeviceTransport>& EngineRuntime::devices() const noexcept {
    return impl_->controller ? impl_->controller->devices() : impl_->emptyDevices;
}
EngineRuntimeStatus EngineRuntime::status() const {
    auto& self = *impl_;
    EngineRuntimeStatus result;
    result.sampleRateMask = self.link.sampleRateMask();
    result.started = self.started;
    result.render = self.render.status();
    if (self.controller)
        result.configuration = self.controller->status();
    result.sources = self.sources.status();
    result.monitors = self.monitors.status();
    result.documentError = self.documentError;
    result.controlError = self.controlError;
    auto scene = self.renderer.observe();
    result.restored = self.started && result.render.running && !result.render.error &&
                      result.render.lastBlockHostNs >= self.renderEpochNs &&
                      result.configuration.phase == RestorationPhase::Restored && scene &&
                      scene->hasRendered() && scene->timingReady() && self.sources.restored() &&
                      self.monitors.restored() && self.documentError.empty() &&
                      self.controlError.empty() &&
                      self.deviceSettings.restoredFor(scene->document());
    return result;
}
} // namespace loopz::macos
