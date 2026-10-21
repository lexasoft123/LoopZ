#pragma once

#include <cstdint>

namespace loopz {
// One serial output callback owner. Rate follows overlapping 25–50 ms
// timestamp observations; phase remains anchored to the actual callback.
// No allocation, system clock reads or audio queue are needed.
class OutputClock {
  public:
    // First/restarted callbacks use the nominal rate until a window is
    // available. Invalid anchors return zero and discard the estimate.
    double observe(uint64_t frame, uint64_t hostNs, uint32_t frames, double nominalRate,
                   bool discontinuity = false) noexcept;
    void reset() noexcept;

  private:
    uint64_t previousFrame_{0}, previousNs_{0};
    uint64_t originFrame_{0}, originNs_{0}, recentFrame_{0}, recentNs_{0};
    uint32_t previousFrames_{0};
    double nominalRate_{0};
};
} // namespace loopz
