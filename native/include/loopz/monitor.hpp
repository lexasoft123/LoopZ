#pragma once

#include <loopz/audio_history.hpp>

namespace loopz {

// The graph publishes one common-clock mix. Each physical monitor has an
// independent non-destructive reader, gain, wiring, meter and physical clock.
// An unchanged bus survives public endpoint rate/mapping and scene replacement.
class MonitorBus final {
  public:
    MonitorBus(uint32_t channels, uint32_t renderRate, uint32_t capacityFrames = 32768);
    uint32_t channels() const noexcept;
    uint32_t renderRate() const noexcept;
    std::shared_ptr<const AudioSource> source() const noexcept;
    // Single graph render owner, also across scene replacements.
    bool publish(uint64_t firstFrame, uint32_t renderRate, RoutingInput input,
                 uint32_t frames) noexcept;
    void idle() noexcept;

  private:
    std::shared_ptr<AudioHistory> history_;
    const uint32_t rate_;
    bool producing_{false};
};

// A monitor keeps its device (and upstream virtual dependencies) demanded even
// while its physical endpoint is absent or muted. Public sink failure never
// suppresses publication to the monitor bus.
class MonitoringSink final : public AudioSink {
  public:
    MonitoringSink(std::shared_ptr<AudioSink> publicSink, std::shared_ptr<MonitorBus> bus,
                   bool monitorDemand);
    void activate() noexcept override;
    uint32_t channels() const noexcept override;
    bool requested() const noexcept override;
    bool publish(uint64_t firstFrame, uint32_t rate, RoutingInput input,
                 uint32_t frames) noexcept override;
    void idle() noexcept override;
    const std::shared_ptr<MonitorBus>& bus() const noexcept;

  private:
    const std::shared_ptr<AudioSink> public_;
    const std::shared_ptr<MonitorBus> bus_;
    const bool monitorDemand_;
};

// Control-thread cache. The caller retains each sink until its render readers
// leave. Preparing an unpublished sink does not stop or mutate an existing bus.
class MonitorBuses {
  public:
    explicit MonitorBuses(uint32_t renderRate);
    std::shared_ptr<MonitoringSink> decorate(const VirtualDeviceDefinition& device,
                                             std::shared_ptr<AudioSink> publicSink);
    void collect();

  private:
    struct Entry {
        std::string device;
        uint32_t channels;
        std::weak_ptr<MonitorBus> bus;
    };
    const uint32_t rate_;
    std::vector<Entry> entries_;
};

struct MonitorTiming {
    uint32_t renderRate{48000};
    double outputRate{48000};
    // This additional queue follows graph/source lookback. It is explicit,
    // measured in internal frames, and is not a hardware latency estimate.
    uint32_t delayFrames{1024};
};
struct MonitorRenderResult {
    bool validBlock{false};
    bool sourceOnline{false};
    uint32_t missingFrames{0}, failedChunks{0};
};
struct MonitorStatus {
    uint64_t callbacks{0}, missingFrames{0}, graphFailures{0}, invalidBlocks{0};
    bool sourceOnline{false};
    double measuredOutputRate{0};
};

// Prepared on the control thread, one serial hardware callback owner. The
// physical callback can exceed the DSP block size; it is split without heap
// activity. Silence/missing history is a valid callback, not a device failure.
class MonitorProgram {
  public:
    MonitorProgram(const VirtualDeviceDefinition& device, const MonitorDefinition& monitor,
                   std::shared_ptr<const AudioSource> source, MonitorTiming timing);
    ~MonitorProgram();
    MonitorProgram(const MonitorProgram&) = delete;
    MonitorProgram& operator=(const MonitorProgram&) = delete;
    // outputRate describes the negotiated nominal format. measuredOutputRate
    // optionally supplies the physical clock estimate (within 0.5%).
    MonitorRenderResult render(uint64_t firstOutputHostNs, double outputRate, RoutingOutput output,
                               uint32_t frames, uint64_t position,
                               double measuredOutputRate = 0) noexcept;
    // Last DSP chunk, not an RMS aggregate over a larger hardware callback.
    bool meters(AudioMeterSnapshot& snapshot) const noexcept;
    MonitorStatus status() const noexcept;
    uint32_t channels() const noexcept;
    MonitorTiming timing() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A direct monitor is valid only when every audible source belongs to this
// physical endpoint. Other clocks, apps and virtual dependencies use MonitorBus.
uint32_t directMonitorInputChannels(const VirtualDeviceDefinition& device,
                                    const std::string& physicalUid);

// One duplex callback clock, two full DSP stages: device mix then monitor mix.
// Captured channels are a contiguous physical prefix. Unavailable logical
// channels are zero; no resampling, timestamp lookback or audio queue is added.
class DirectMonitorProgram {
  public:
    DirectMonitorProgram(const VirtualDeviceDefinition& device, const MonitorDefinition& monitor,
                         const std::string& physicalUid, uint32_t physicalInputChannels,
                         double sampleRate);
    ~DirectMonitorProgram();
    DirectMonitorProgram(const DirectMonitorProgram&) = delete;
    DirectMonitorProgram& operator=(const DirectMonitorProgram&) = delete;
    MonitorRenderResult render(RoutingInput input, RoutingOutput output, uint32_t frames,
                               uint64_t position) noexcept;
    bool meters(AudioMeterSnapshot& snapshot) const noexcept;
    MonitorStatus status() const noexcept;
    bool equivalentDevice(const VirtualDeviceDefinition& device) const noexcept;
    uint32_t inputChannels() const noexcept;
    double sampleRate() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
