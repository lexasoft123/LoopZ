#pragma once

#include <loopz/audio_meter.hpp>
#include <loopz/route_document.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <optional>

namespace loopz {
enum class ResamplingFilter;

// A reader cache belongs to one serial render owner. Keeping it shared across
// prepared scene replacements preserves unchanged source clock anchors. Neither
// the control thread nor a second render thread may mutate it concurrently.
struct AudioReadCursor {
    uint64_t frame{0}, hostNs{0}, generation{0};
    double rate{0};
    uint32_t frames{0};
};
struct AudioReadResult {
    uint32_t missingFrames;
    bool online;
};

class AudioSource {
  public:
    virtual ~AudioSource() = default;
    virtual uint32_t channels() const noexcept = 0;
    // Render-domain contract: no allocation, locks, logging or OS operations.
    // Fill all requested planes; absent frames are zero. Independent readers
    // use separate cursors and do not consume one another's history.
    virtual AudioReadResult read(uint64_t firstHostNs, double rate, RoutingOutput output,
                                 uint32_t frames, AudioReadCursor& cursor) const noexcept = 0;
    // Optional native pre-gain levels, without converting audio to the render
    // clock. True means supported; frames == 0 means currently unavailable.
    // The same realtime contract applies. The snapshot is independent of reads.
    virtual bool readMeter(AudioMeterSnapshot&) const noexcept {
        return false;
    }
    // Optional sparse read. Channels are unique, increasing source indices;
    // output planes correspond to that list. Unsupported adapters return nullopt
    // without touching output, allowing the caller to use the full-layout path.
    virtual std::optional<AudioReadResult>
    readChannels(uint64_t, double, RoutingOutput, uint32_t, AudioReadCursor&,
                 std::span<const uint32_t>, std::optional<ResamplingFilter>) const noexcept {
        return std::nullopt;
    }
    // A prepared physical reader may select its filter per scene. The default
    // preserves adapters without selectable resampling. Selection must never
    // prepare coefficients, allocate or change shared producer state here.
    virtual AudioReadResult readWithFilter(uint64_t firstHostNs, double rate, RoutingOutput output,
                                           uint32_t frames, AudioReadCursor& cursor,
                                           ResamplingFilter) const noexcept {
        return read(firstHostNs, rate, output, frames, cursor);
    }
};
struct ResolvedAudioSource {
    std::shared_ptr<const AudioSource> source;
    std::shared_ptr<AudioReadCursor> cursor;
};

class AudioSink {
  public:
    virtual ~AudioSink() = default;
    // Control thread, after native acknowledgement and before scene publication.
    // Prepared sinks must reserve every resource needed by this no-fail step.
    virtual void activate() noexcept {}
    virtual uint32_t channels() const noexcept = 0;
    // Public IO or a configured monitor requests a device. Demand on a virtual
    // dependency also activates its upstream device without a public client.
    virtual bool requested() const noexcept = 0;
    // The backend converts this common-clock planar block to its endpoint rate.
    // These methods have the same nonblocking render-domain contract as read().
    virtual bool publish(uint64_t firstFrame, uint32_t renderRate, RoutingInput input,
                         uint32_t frames) noexcept = 0;
    virtual void idle() noexcept = 0;
};

struct SceneTiming {
    uint32_t renderRate{48000};
    uint32_t blockFrames{256};
    uint32_t sourceDelayFrames{1024};
    uint32_t passThroughDelayFrames{128};
    bool stageTiming{false};
};
struct SceneDeviceStatus {
    bool active{false};
    uint64_t renderedFrames{0}, graphFailures{0}, sinkFailures{0};
    bool stageTiming{false};
    uint64_t inputNs{0}, graphNs{0}, outputNs{0}, sinkNs{0};
};
// Control-domain timing, derived without opening providers or allocating audio
// histories. Latencies use the render clock and follow document device order.
struct SceneTimingPlan {
    std::vector<size_t> renderOrder;
    std::vector<uint32_t> latencyFrames;
};
struct DevicePassThroughDelay {
    std::string device;
    uint32_t frames;
};
// Render-clock budget for one physical or application input binding. Supplied by the
// backend before publication; it does not alter the saved routing document.
struct DeviceInputDelay {
    std::string device;
    std::string source;
    uint32_t frames;
    std::optional<ResamplingFilter> filter{};
};
SceneTimingPlan planSceneTiming(const RouteDocument& document, SceneTiming timing,
                                std::span<const DevicePassThroughDelay> passThroughFloors = {},
                                std::span<const DeviceInputDelay> inputDelays = {});
struct SceneSourceStatus {
    bool online{false};
    uint64_t missingFrames{0};
    std::optional<ResamplingFilter> resamplingFilter{};
    std::optional<uint32_t> historyBudgetFrames{};
};
struct SceneRenderResult {
    bool validBlock;
    uint32_t activeDevices;
    uint32_t failedGraphs;
    uint32_t failedSinks;
};

// One control owner and one serial render owner may share this between scenes.
// Matching histories survive replacement without copying live audio during
// preparation. The store does not own capture providers.
class SceneHistoryStore {
  public:
    SceneHistoryStore();
    ~SceneHistoryStore();
    SceneHistoryStore(const SceneHistoryStore&) = delete;
    SceneHistoryStore& operator=(const SceneHistoryStore&) = delete;

  private:
    friend class AudioScene;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// All preparation, resolver calls, shared ownership changes and destruction
// happen on the control thread. One audio thread renders the whole DAG at one
// internal sample rate. Physical/public-device clock conversion stays behind
// the source/sink interfaces. Inputs to a mix share one origin time; virtual
// dependencies add only the difference in upstream and downstream delay.
class AudioScene {
  public:
    using SourceResolver = std::function<ResolvedAudioSource(
        const VirtualDeviceDefinition&, const SourceBinding&, const SourceRouting&)>;
    using SinkResolver = std::function<std::shared_ptr<AudioSink>(const VirtualDeviceDefinition&)>;
    AudioScene(RouteDocument document, SceneTiming timing, SourceResolver source, SinkResolver sink,
               std::shared_ptr<SceneHistoryStore> histories = {},
               std::span<const DevicePassThroughDelay> passThroughFloors = {},
               std::span<const DeviceInputDelay> inputDelays = {});
    ~AudioScene();
    AudioScene(const AudioScene&) = delete;
    AudioScene& operator=(const AudioScene&) = delete;

    SceneRenderResult render(uint64_t firstFrame, uint32_t frames,
                             uint32_t previewDevices = 0) noexcept;
    void activate() noexcept;
    const RouteDocument& document() const noexcept;
    SceneTiming timing() const noexcept;
    bool hasRendered() const noexcept;
    // Required compensation histories contain the current requested interval.
    // This does not certify capture availability or non-silent signal content.
    bool timingReady() const noexcept;
    // One coherent demand mask in document device order (at most 32 devices).
    uint32_t demandedDevices() const noexcept;
    // Fresh endpoint demand without rendering or mutating a prepared scene.
    uint32_t requestedDevices(uint32_t previewDevices = 0) const noexcept;
    SceneDeviceStatus deviceStatus(size_t device) const noexcept;
    SceneSourceStatus sourceStatus(size_t device, size_t source) const noexcept;
    bool deviceMeters(size_t device, AudioMeterSnapshot& output) const noexcept;
    bool sourceMeters(size_t device, size_t source, AudioMeterSnapshot& output) const noexcept;
    uint32_t deviceLatencyFrames(size_t device) const noexcept;
    // Control-domain ownership view for source lifecycle coordination.
    const ResolvedAudioSource* input(size_t device, size_t source) const noexcept;
    const AudioSink* sink(size_t device) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
