#pragma once

#include <array>
#include <cstdint>

namespace loopz {
struct RenderScheduleTiming {
    uint32_t rate{48000}, blockFrames{256}, leadFrames{64}, maximumCatchupFrames{4096};
};
struct ScheduledRender {
    bool valid{false}, reanchored{false};
    uint64_t firstFrame{0}, nextWakeNs{0}, skippedFrames{0}, lateFrames{0};
    uint32_t blocks{0};
};

// One render owner. Pure clock arithmetic, without allocation or OS calls.
// Each step bounds catch-up work to four blocks; an already-due nextWakeNs
// requests another step. Long stalls re-anchor once rather than replaying an
// arbitrarily old queue. Ordinary late blocks retain contiguous timestamps.
class RenderSchedule {
  public:
    explicit RenderSchedule(RenderScheduleTiming timing);
    ScheduledRender step(uint64_t nowNs) noexcept;
    void reset() noexcept;
    RenderScheduleTiming timing() const noexcept;

  private:
    const RenderScheduleTiming timing_;
    uint64_t next_{0}, previousNow_{0};
    bool initialized_{false};
};
} // namespace loopz
