#pragma once

#include <loopz/audio_scene.hpp>
#include <loopz/macos/configuration_controller.hpp>
#include <loopz/realtime_publication.hpp>
#include <loopz/source_registry.hpp>

namespace loopz::macos {

// The render scheduler supplies one monotonic host time per block. Endpoint
// adapters use it for lease validation without calling clock APIs per sample.
struct RenderClock {
    std::atomic<uint64_t> nowNs{0};
};

// One serial render owner, including across scene replacements. Retain this
// object for an unchanged mapping to preserve boundary interpolation history.
class TransportAudioSink final : public AudioSink {
  public:
    static uint32_t delayFrames(uint32_t renderRate, uint32_t endpointRate) noexcept;
    TransportAudioSink(std::shared_ptr<DeviceAudioRegion> region,
                       std::shared_ptr<const RenderClock> clock, uint32_t renderRate = 48000);
    ~TransportAudioSink() override;
    uint32_t channels() const noexcept override;
    bool requested() const noexcept override;
    // Serial control owner, only for a host-approved visibility transition
    // that reuses this exact capture mapping. Never starts an idle producer.
    bool beginConfigurationHold() noexcept;
    void endConfigurationHold() noexcept;
    bool publish(uint64_t firstFrame, uint32_t renderRate, RoutingInput input,
                 uint32_t frames) noexcept override;
    void idle() noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class TransportDirection { Capture, Playback };
// Independent non-destructive readers, with no source consumption. Each adapter
// retains its own mapping; a retired mapping always produces silence.
class TransportAudioSource final : public AudioSource {
  public:
    TransportAudioSource(std::shared_ptr<DeviceAudioRegion> region,
                         std::shared_ptr<const RenderClock> clock, TransportDirection direction);
    ~TransportAudioSource() override;
    uint32_t channels() const noexcept override;
    AudioReadResult read(uint64_t firstHostNs, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Control prepares a complete graph before native submission. Only commit
// publishes it. A rejected, uncertain or disconnected transaction cannot expose
// a tentative graph to the single render caller. Source providers keep desired
// identities alive and report missing endpoints through AudioSource::read.
class SceneRenderer final : public GraphActivation {
  public:
    using Observation = RealtimePublication<AudioScene>::Borrow;
    using TimingFloors = std::function<std::vector<DevicePassThroughDelay>(const RouteDocument&)>;
    using InputTiming = std::function<std::vector<DeviceInputDelay>(const RouteDocument&)>;
    using SinkDecorator = std::function<std::shared_ptr<AudioSink>(const VirtualDeviceDefinition&,
                                                                   std::shared_ptr<AudioSink>)>;
    explicit SceneRenderer(SceneTiming timing, AudioScene::SourceResolver externalSources,
                           SinkDecorator sinks = {}, size_t retirementLimit = 8);
    // The registry outlives this renderer. Preparation plans all shared sources;
    // pollSources() opens only those demanded by the committed render snapshot.
    SceneRenderer(SceneTiming timing, SourceRegistry& sources, SinkDecorator sinks = {},
                  size_t retirementLimit = 8);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;
    std::vector<uint32_t> inputLatencyFrames(const RouteDocument& document) const override;
    std::unique_ptr<GraphActivationPlan> plan(const RouteDocument& document) override;
    std::unique_ptr<PreparedGraphActivation>
    prepare(const RouteDocument& document, std::span<const DeviceTransport> devices) override;
    SceneRenderResult render(uint64_t firstFrame, uint32_t frames, uint64_t nowNs) noexcept;
    // Observation only; no second render caller or cursor mutation is allowed.
    Observation observe() noexcept;
    void collect() noexcept;
    void pollSources(uint64_t nowMilliseconds);
    // Control-owned editor demand; rendering stays on the existing audio thread.
    void setEditorPreview(bool enabled) noexcept;
    // Control only, before use. Each prepared scene pins its exact timing plan.
    void setTimingFloors(TimingFloors floors);
    void setInputTiming(InputTiming timing);

  private:
    std::unique_ptr<PreparedGraphActivation>
    prepareWithTiming(const RouteDocument& document, std::span<const DeviceTransport> devices,
                      std::span<const DevicePassThroughDelay> floors,
                      std::span<const DeviceInputDelay> inputs);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
