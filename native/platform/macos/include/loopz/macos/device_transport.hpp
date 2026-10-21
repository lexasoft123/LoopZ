#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace loopz::macos {

// Protocol 2 is a separate, per-device Darwin mapping. It does not change the
// resident feasibility driver's protocol-1 layout or service negotiation.
inline constexpr uint32_t kDeviceAudioProtocol = 2;
inline constexpr uint32_t kDeviceAudioCapacity = 32768;
inline constexpr uint32_t kDeviceAudioMaximumChannels = 64;
inline constexpr size_t kDeviceAudioMappingAlignment = 65536;
inline constexpr uint64_t kDeviceAudioHeartbeatTimeoutNs = 250000000;
// A block's clock can precede a concurrently published heartbeat. Callers that
// hold such a snapshot may opt into this bounded allowance; exact-time callers
// retain strict future rejection by default. This never changes sample delay.
inline constexpr uint64_t kDeviceAudioConcurrentHeartbeatSkewNs = 50000000;

struct alignas(64) DeviceAudioHeader {
    uint64_t magic;
    uint32_t protocol;
    uint32_t headerBytes;
    uint64_t mappingBytes;
    uint64_t generation;
    uint32_t channels;
    uint32_t sampleRate;
    uint32_t capacityFrames;
    uint32_t reserved;
    std::atomic<uint64_t> helperHeartbeatNs{0};
    std::atomic<uint64_t> driverHeartbeatNs{0};
    std::atomic<uint32_t> helperOnline{0};
    std::atomic<uint32_t> driverOnline{0};
    std::atomic<uint64_t> captureReadFrames{0};
    std::atomic<uint64_t> captureMissingFrames{0};
    std::atomic<uint64_t> playbackReadFrames{0};
    std::atomic<uint64_t> playbackMissingFrames{0};
    std::atomic<uint32_t> retired{0};
    uint64_t mappingIdentityHigh;
    uint64_t mappingIdentityLow;
};

struct DeviceAudioTransfer {
    bool validBlock;
    uint32_t missingFrames;
};

class DeviceAudioView {
  public:
    // Control-thread validation. The mapping must stay alive and its immutable
    // header must not change until all borrowers have stopped. No ownership or
    // reference counting occurs in the audio methods below.
    static DeviceAudioView attach(void* mapping, size_t bytes);
    DeviceAudioHeader& header() const noexcept;
    uint32_t channels() const noexcept;
    uint32_t sampleRate() const noexcept;
    uint64_t generation() const noexcept;
    std::array<uint64_t, 2> mappingIdentity() const noexcept;

    // One serial writer per direction, arbitrary non-destructive readers.
    // Buffers are interleaved and declare their capacity through their span.
    // Invalid reads silence only that span. Valid missing frames also silence.
    bool writeCapture(uint64_t firstFrame, std::span<const float> samples,
                      uint32_t frames) const noexcept;
    bool writePlayback(uint64_t firstFrame, std::span<const float> samples,
                       uint32_t frames) const noexcept;
    DeviceAudioTransfer readCapture(uint64_t firstFrame, std::span<float> samples, uint32_t frames,
                                    uint64_t nowNs,
                                    uint64_t concurrentHeartbeatSkewNs = 0) const noexcept;
    DeviceAudioTransfer readPlayback(uint64_t firstFrame, std::span<float> samples, uint32_t frames,
                                     uint64_t nowNs,
                                     uint64_t concurrentHeartbeatSkewNs = 0) const noexcept;

  private:
    DeviceAudioView(void* mapping, uint32_t channels, uint32_t sampleRate, uint64_t generation,
                    size_t captureTags, size_t captureSamples, size_t playbackTags,
                    size_t playbackSamples) noexcept;
    bool write(bool capture, uint64_t firstFrame, std::span<const float> samples,
               uint32_t frames) const noexcept;
    DeviceAudioTransfer read(bool capture, uint64_t firstFrame, std::span<float> samples,
                             uint32_t frames, uint64_t nowNs,
                             uint64_t concurrentHeartbeatSkewNs = 0) const noexcept;
    std::byte* mapping_;
    uint32_t channels_;
    uint32_t sampleRate_;
    uint64_t generation_;
    size_t captureTags_, captureSamples_, playbackTags_, playbackSamples_;
};

class DeviceAudioRegion {
  public:
    // Allocation, construction and final unmapping are control-thread work.
    // A new format/generation always receives a new mapping; old borrowers keep
    // their previous region pinned rather than observing a mutated layout.
    static std::shared_ptr<DeviceAudioRegion> create(uint32_t channels, uint32_t sampleRate,
                                                     uint64_t generation);
    ~DeviceAudioRegion();
    DeviceAudioRegion(const DeviceAudioRegion&) = delete;
    DeviceAudioRegion& operator=(const DeviceAudioRegion&) = delete;
    DeviceAudioView view() const;
    DeviceAudioHeader& header() const noexcept;
    // Irreversible for this generation, including late online/heartbeat writes.
    void retire() noexcept;
    void* data() const noexcept;
    size_t bytes() const noexcept;

  private:
    DeviceAudioRegion(void* mapping, size_t bytes) noexcept;
    void* mapping_;
    size_t bytes_;
};

} // namespace loopz::macos
