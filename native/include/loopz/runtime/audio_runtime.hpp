#pragma once
#include <loopz/audio_scene.hpp>
#include <loopz/audio_history.hpp>
#include <loopz/monitor.hpp>
#include <zcore/device/audio_host.h>
#include <zcore/device/audio_input_backend.h>
#include <functional>
#include <optional>
#include <string_view>

namespace loopz::runtime {
struct EndpointStatus {
    std::string id, error;
    bool connected{false};
    singz::AudioHostStatus audio{};
    bool captureTelemetry{false};
    AudioHistoryStatus captureHistory{};
    singz::AudioInputTimestampQuality timestampQuality{singz::AudioInputTimestampQuality::Unknown};
    // Received-PCM provenance is software continuity, not a hardware position.
    bool sourceSequenceAvailable{false}, sourceCounterHardware{false};
    bool captureMetadataValid{false};
    singz::AudioInputCaptureMetadata captureMetadata{};
    uint64_t observedXruns{0}, captureBoundaries{0};
    AudioHistoryReadSnapshot captureReads{};
    // Provider timestamps remain raw in captureMetadata. These fields describe
    // only the history's explicitly estimated received-PCM timeline.
    bool captureHistoryProjected{false};
    uint64_t captureMaximumRawPhaseStepNs{0}, captureMaximumProjectionErrorNs{0};
};
// Borrowed native input reader. The runtime owns its capture; this value owns
// no hardware and has no implicit release/destructor. Stop and join every
// reader before releaseInput(id) or runtime destruction invalidates source.
struct InputSubscription {
    uint64_t id{0};
    std::string deviceUid;
    uint32_t channels{0};
    const AudioSource* source{nullptr};
};
struct SourceTelemetry {
    std::string id;
    SceneSourceStatus status{};
    AudioMeterSnapshot meter{};
};
struct BusTelemetry {
    std::string id;
    bool active{false};
    AudioMeterSnapshot meter{};
    std::vector<SourceTelemetry> sources;
};
struct MonitorTelemetry {
    std::string device, monitor;
    AudioMeterSnapshot meter{};
    MonitorStatus status{};
    bool historyBudgetKnown{false};
    uint32_t historyDelayFrames{0}, queuedOutputFrames{0}, outputPeriodFrames{0},
        filterFutureFrames{0}, schedulerReserveFrames{0};
};
struct RuntimeStatus {
    bool active{false}, restored{false};
    uint64_t renderedFrames{0}, graphFailures{0}, missedDeadlines{0};
    uint64_t renderLateFrames{0}, renderSkippedFrames{0}, renderReanchors{0};
    // Linux only: the mix renderer obtained SCHED_FIFO just below zCore's ALSA
    // I/O workers. False means inherited scheduling (for example, no
    // RLIMIT_RTPRIO), a non-Linux host, or a renderer that has not started.
    bool rendererRealtime{false};
    std::vector<EndpointStatus> inputs, outputs;
    std::vector<AudioMeterSnapshot> buses;
    std::vector<BusTelemetry> devices;
    std::vector<MonitorTelemetry> monitors;
};
// Standalone runtime admission budgets complement the portable document's
// 32-bus/64-channel schema. Counts include failed demanded endpoints; graph
// storage budgets include disabled routes because preparation reserves them.
// Retained wider capture profiles also count against the capture-channel budget.
struct RuntimeLimits {
    static constexpr uint32_t maximumPhysicalCaptures = 8;
    static constexpr uint32_t maximumCaptureChannels = 128;
    static constexpr uint32_t maximumInputSubscriptions = 64;
    static constexpr uint32_t maximumPhysicalOutputs = 8;
    static constexpr uint32_t maximumOutputChannels = 128;
    static constexpr uint32_t maximumBusChannels = 128;
    static constexpr uint32_t maximumSourceChannels = 512;
    static constexpr uint32_t maximumConnections = 8192;
};
// Single control owner. Prepared replacements must be committed or destroyed
// before another prepare. All callbacks are drained before destruction.
inline constexpr size_t kMaximumNativeSources = 64;
class AudioRuntime {
  public:
    using HostFactory = std::function<std::unique_ptr<singz::AudioHostBackend>()>;
    using InputFactory = std::function<std::unique_ptr<singz::AudioInputBackend>()>;
    struct Prepared;
    explicit AudioRuntime(HostFactory host = singz::createPlatformAudioHostBackend,
                          InputFactory input = singz::createPlatformAudioInputBackend);
    ~AudioRuntime();
    AudioRuntime(const AudioRuntime&) = delete;
    AudioRuntime& operator=(const AudioRuntime&) = delete;
    singz::AudioHostInventory inventory() const;
    // Opens new endpoints without retiring current owners. Conflicting changes
    // fail explicitly; the active route remains usable. Preparation may throw.
    std::shared_ptr<Prepared> prepare(const RouteDocument& document);
    // Call after durable desired-state storage. Prepared storage makes this
    // publication no-fail; it does not allocate or close hardware callbacks.
    void commit(std::shared_ptr<Prepared> prepared) noexcept;
    void poll();
    RuntimeStatus status() const;
    // Serialized control-owner APIs; never call from a callback. Explicit,
    // nonempty device UIDs only ("default" is intentionally rejected). A live
    // public/retired default capture conflicts with private subscriptions.
    // minimumChannels is physical prefix capacity, not a channel remapping.
    // Reuse a sufficiently wide owner; never expand/reopen one that is live.
    // Provider-offline acquisition retains an offline lease for poll() retry;
    // validation, resource admission and in-use profile conflicts throw.
    InputSubscription acquireInput(std::string deviceUid, uint32_t minimumChannels);
    // Caller must first stop/join its reader. False means a stale/unknown id.
    bool releaseInput(uint64_t id);
    std::optional<EndpointStatus> inputStatus(uint64_t id) const;
    // A state reader may request meters for otherwise undemanded buses for
    // 500ms. Repeated reads renew this bounded preview; expiry never removes
    // persistent monitor/dependency demand or changes saved hardware settings.
    void requestTelemetryPreview() noexcept;
    // Application selectors beginning native: resolve through this reusable
    // in-process seam. Registration happens before preparation; PCM never
    // crosses the control protocol. Source ownership remains on control.
    void bindNativeSource(std::string selector, std::shared_ptr<const AudioSource> source);
    // Removes future resolution only. Published scenes retain the source until
    // replaced. Stop its producer first to make a playing song silent without
    // changing microphone routes, then remove its binding and unregister it.
    bool unbindNativeSource(std::string_view selector);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::runtime
