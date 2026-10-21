#pragma once
#include <loopz/audio_history.hpp>
#include <loopz/timeline.hpp>
#include <atomic>

namespace loopz {
// Stereo capture adapter over the portable bandlimited history. Construct on
// control; one serial producer and independent render readers. Rate conversion
// needs both filter wings; missing support is silent without a timestamp shift.
class CaptureBuffer {
  public:
    bool append(const float* left, const float* right, uint32_t frames, uint64_t firstHostNs,
                double nominalRate, double sourceFrame = NAN) noexcept;
    // Call after producer callbacks have joined. Readers may remain active.
    void stop() noexcept;
    struct ReaderState {
        uint64_t frame{0}, ns{0}, generation{0};
        double rate{0};
        uint32_t frames{0};
    };
    uint32_t sample(uint64_t firstHostNs, double outputRate, float* left, float* right,
                    uint32_t frames, ReaderState* reader = nullptr,
                    std::span<uint8_t> availableFrames = {}) const noexcept;
    struct ClockSnapshot {
        uint64_t anchorNs;
        double measuredRate;
    };
    ClockSnapshot clock() const noexcept;
    std::atomic<uint64_t> callbacks{0};
    std::atomic<uint64_t> discontinuities{0};

  private:
    AudioHistory history_{2};
    // Diagnostic clock publication, independent of each reader's history cache.
    std::atomic<uint64_t> version_{0}, anchorNs_{0}, rateBits_{0};
};
} // namespace loopz
