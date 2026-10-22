#pragma once

#include <loopz/monitor.hpp>
#include <zcore/device/audio_host.h>

namespace loopz::macos {
struct MonitorConnectionStatus {
    std::string device, monitor, selector, resolvedUid;
    bool demanded{false}, connected{false}, complete{false};
    uint32_t requestedChannels{0}, physicalChannels{0}, queueFrames{0};
    double sampleRate{0};
    uint64_t attempts{0};
    std::string error;
    MonitorStatus audio;
    uint64_t lastCallbackMs{0};
    bool directHardwareClock{false};
    uint64_t hardwareTimestampErrors{0}, hardwareDiscontinuities{0}, hostXruns{0};
    uint32_t bufferFrames{0};
};

// One control owner. Preparation allocates DSP and reserves publication capacity
// without opening hardware or changing a running monitor. AudioScene::activate
// publishes the staged monitor programs only after the native transaction commits.
// poll() acquires/retries only endpoints in that committed scene. Stop joins all
// callbacks before resources are destroyed. This owner outlives its scene renderer.
class MonitorOutputs {
  public:
    using HostFactory = std::function<std::unique_ptr<singz::AudioHost>()>;
    using Clock = std::function<uint64_t()>;
    using InputPermission = std::function<bool()>;
    // delayFrames is a floor. Streams reserve the selected number of negotiated
    // output periods plus one graph quantum, converted to internal frames.
    // One period is an opt-in diagnostic profile requiring real continuity
    // validation; two remains the default. Shared periods are never changed.
    // An optional control clock refreshes each endpoint's retry/deadline after
    // blocking host calls; callback-stall grace begins at admission. Metadata
    // inspection is bounded separately from callback health checks. Native
    // recovery events invalidate it immediately through retryNow().
    explicit MonitorOutputs(uint32_t renderRate, uint32_t delayFrames = 1024,
                            HostFactory factory = {}, Clock controlClock = {},
                            uint32_t renderBlockFrames = 256, uint32_t periodReserve = 2,
                            InputPermission inputPermission = {},
                            uint32_t deviceInspectionIntervalMs = 250);
    ~MonitorOutputs();
    MonitorOutputs(const MonitorOutputs&) = delete;
    MonitorOutputs& operator=(const MonitorOutputs&) = delete;
    std::shared_ptr<AudioSink> decorate(const VirtualDeviceDefinition& device,
                                        std::shared_ptr<AudioSink> publicSink);
    // now uses the same monotonic millisecond domain as callbackHostTimeNs.
    void poll(const AudioScene* committed, uint64_t nowMilliseconds);
    void retryNow(uint64_t nowMilliseconds) noexcept;
    void stop() noexcept;
    bool restored() const noexcept;
    std::vector<MonitorConnectionStatus> status() const;
    bool meters(const std::string& device, const std::string& monitor,
                AudioMeterSnapshot& output) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
