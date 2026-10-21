#pragma once

#include <array>
#include <cstdint>

namespace loopz {
struct AudioDeliveryEvent {
    uint64_t elapsedNs{}, hostIntervalNs{}, callbackIntervalNs{}, processingNs{};
    uint32_t previousFrames{}, frames{};
    double sourceFrameDelta{}, expectedSourceFrameDelta{};
    bool sourceFrameValid{};
};
struct AudioDeliverySnapshot {
    uint64_t callbacks{}, frames{}, warmupCallbacks{}, rejected{};
    uint32_t minimumFrames{}, maximumFrames{};
    int64_t minimumEntryAgeNs{}, maximumEntryAgeNs{};
    int64_t minimumReadyEndAgeNs{}, maximumReadyEndAgeNs{};
    uint64_t minimumGapNs{}, maximumGapNs{}, maximumProcessingNs{};
    uint64_t maximumAnchorErrorNs{}, unsupportedAnchorIntervals{}, sourceFrameDiscontinuities{};
    uint32_t eventCount{};
    std::array<AudioDeliveryEvent, 16> events{};
};

// One serial callback writer. Read/reset only after its callbacks have joined.
// Times share one monotonic nanosecond domain. Ages preserve future timestamps
// as negative values; end age is relative to the exclusive end of the block.
// This observes delivery, not the minimum safe queue or acoustic latency.
class AudioDeliveryTiming {
  public:
    explicit AudioDeliveryTiming(uint64_t warmupNs = 1000000000) : warmupNs_(warmupNs) {}
    void observe(uint64_t firstHostNs, uint32_t frames, double rate, uint64_t enteredNs,
                 uint64_t readyNs, double sourceFrame = 0, bool sourceFrameValid = false) noexcept;
    AudioDeliverySnapshot snapshot() const noexcept;
    void reset() noexcept;

  private:
    const uint64_t warmupNs_;
    uint64_t firstEntryNs_{}, previousEntryNs_{};
    uint64_t previousSampleNs_{};
    uint32_t previousFrames_{};
    double previousRate_{};
    double previousSourceFrame_{};
    bool previousSourceValid_{};
    AudioDeliverySnapshot value_{};
};
} // namespace loopz
