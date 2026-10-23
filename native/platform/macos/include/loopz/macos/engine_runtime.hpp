#pragma once

#include <loopz/macos/monitor_outputs.hpp>
#include <loopz/macos/render_thread.hpp>
#include <loopz/macos/editor_controller.hpp>
#include <loopz/macos/public_timing.hpp>
#include <loopz/audio_resampler.hpp>
#include <optional>

namespace loopz::macos {
struct EngineRuntimeOptions {
    SceneTiming scene;
    uint32_t renderLeadFrames{64}, maximumCatchupFrames{4096}, monitorDelayFrames{1024};
    uint32_t applicationBufferFrames{128};
    uint32_t monitorPeriodReserve{2};
    bool captureDeliveryDiagnostics{false};
    // Private diagnostic selection; normal sessions retain linear-phase input.
    ResamplingFilter inputFilter{ResamplingFilter::LinearPhase};
    // Explicit physical-input diagnostic budget; app and pass-through delays
    // retain their independent policies.
    std::optional<uint32_t> inputDelayFrames;
    std::filesystem::path documentPath;
    std::filesystem::path deviceSettingsPath;
    // Installer-only graph publication: external inputs stay unavailable and
    // physical monitors are never constructed. Saved routing stays intact.
    bool installationValidation{false};
    bool automaticPassThroughTiming{true};
    bool automaticInputTiming{true};
    // Verified private stream timing, with conservative history until known.
    bool automaticApplicationTiming{true};
    PublicPassThroughTiming::Probe publicTimingProbe;
};
struct EngineRuntimeStatus {
    bool started{false}, restored{false};
    uint32_t sampleRateMask{kLegacyVirtualSampleRates};
    RestorationStatus configuration{RestorationPhase::NoDesiredDocument, 0, 0, 0, 0, {}, false};
    RenderThreadStatus render;
    std::vector<SourceConnectionStatus> sources;
    std::vector<MonitorConnectionStatus> monitors;
    std::string documentError, controlError;
};

// Single control owner with an independent render thread. The link outlives
// this runtime. Default factories are the real macOS providers; injected
// factories support full lifecycle fixtures without protected permissions.
// Missing/corrupt documents and unavailable services retain the process and
// last good desired graph. No empty snapshot is sent implicitly.
class EngineRuntime {
  public:
    EngineRuntime(DriverConfigurationLink& link, EngineRuntimeOptions options = {},
                  SourceRegistry::Factory sources = {}, MonitorOutputs::HostFactory outputs = {});
    ~EngineRuntime();
    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;
    void start();
    void stop() noexcept;
    void poll();
    void retryNow();
    // A fully restored, all-disabled graph has no live meters. Its status may
    // be reused until the next maintenance tick or any control operation.
    // Read only on the same serialized control owner as poll()/editor calls.
    std::optional<uint64_t> idleStatusToken() const noexcept;
    // Only an authenticated interactive editor requests live source meters.
    // Closing its connection releases preview without changing saved routing.
    void setEditorPreview(bool enabled) noexcept;
    std::string hardwareSnapshot();
    std::shared_ptr<const PublicTimingObservationBatch> publicTimingObservations() const noexcept;
    std::string applyHardwareBuffer(std::string_view request);
    // A system audio-service reset invalidates every cached native handle.
    // Release them on this control thread, retain desired routing and reconnect
    // through the authenticated link; never restart the system audio service.
    void audioServiceReset();
    void setDesired(RouteDocument document, bool save = false);
    void reloadDesired();
    EditorSnapshot editorSnapshot() const;
    EditorCommit applyEditor(const EditorRevision& expected,
                             std::span<const edit::Command> commands);
    EditorCommit initializeEditor(const EditorRevision& expected,
                                  std::span<const edit::Command> commands);
    EditorCommit moveEditorHistory(const EditorRevision& expected,
                                   EditorHistoryDirection direction);
    bool monitorMeters(const std::string& device, const std::string& monitor,
                       AudioMeterSnapshot& output) const noexcept;
    EngineRuntimeStatus status() const;
    SceneRenderer::Observation observe() noexcept;
    const std::vector<DeviceTransport>& devices() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
