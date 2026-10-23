#pragma once
#include <loopz/monitor.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace loopz::windows {
struct VirtualCableDiagnostics {
    uint64_t maximumLatenessNs{}, maximumDrainNs{}, maximumRenderNs{}, maximumReadNs{},
        maximumWriteNs{};
    uint64_t deadlineMisses{}, lastDeadlineMissNs{};
    uint64_t ingressPackets{}, ingressFrames{}, packetDiscontinuities{}, timestampResets{},
        completedIngressZeroRuns{}, maximumCompletedIngressZeroFrames{}, restartAttempts{},
        outgoingFrames{}, outgoingAllZeroFrames{};
    bool driverQueueAvailable{};
    uint64_t driverMissingFrames{}, driverDroppedFrames{}, driverUnderruns{},
        driverLastUnderrunTime100ns{}, driverRecordingFrames{}, driverLeaseSilentFrames{};
    uint64_t driverSampleTime100ns{};
    uint32_t driverQueuedFrames{}, driverIngressPackets{}, driverLastUnderrunQueued{},
        driverLastUnderrunRequested{};
    uint64_t publicationWaits{}, publicationWaitTimeouts{}, maximumPublicationWaitNs{};
    bool publicationWaitEnabled{}, streamingChunksEnabled{};
    uint64_t maximumChunkRenderNs{};
    unsigned transportVersion{}, bitsPerSample{};
    bool threadCpuTimingAvailable{}, mmcssRegistered{};
    uint64_t threadCpuNs{};
    uint32_t threadId{}, mmcssError{};
    uint32_t publicationBatchFrames{};
    unsigned publicationFlowVersion{};
    uint64_t ingressSkippedPackets{}, ingressSequenceResets{};
    uint64_t maximumIngressPublicationLagNs{}, maximumIngressPublicationGapNs{};
};
// One COM worker owns both private driver directions. KS calls never execute in
// a graph callback. History readers and monitor meter readers remain independent.
class VirtualCable final {
  public:
    VirtualCable(std::string endpoint, const VirtualDeviceDefinition& device,
                 std::shared_ptr<const AudioSource> mix, uint32_t renderRate = 48000);
    ~VirtualCable();
    bool start();
    void stop();
    bool healthy() const noexcept;
    bool matches(const VirtualDeviceDefinition& device, uint32_t renderRate) const noexcept;
    std::string error() const;
    VirtualCableDiagnostics diagnostics() const noexcept;
    const std::string& endpoint() const noexcept;
    std::shared_ptr<AudioHistory> playback() const;
    MonitorProgram* program() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::windows
