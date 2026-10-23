#pragma once
#include <cstdint>

namespace loopz::windows {
// One connection/worker owns this tracker. Sequence wraps are valid; a
// backwards jump is reported separately rather than counted as enormous loss.
class IngressSequence {
  public:
    struct Observation {
        uint64_t skippedPackets{};
        bool reset{};
    };
    Observation append(uint64_t sequence) noexcept {
        Observation result;
        if (initialized_) {
            const auto distance = sequence - next_;
            if (distance < (uint64_t{1} << 63))
                result.skippedPackets = distance;
            else
                result.reset = true;
        }
        next_ = sequence + 1;
        initialized_ = true;
        return result;
    }

  private:
    uint64_t next_{};
    bool initialized_{};
};
} // namespace loopz::windows
