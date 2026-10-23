#pragma once
#include "audio_format.hpp"
namespace loopz_driver {
// Caller supplies monotonic elapsed 100ns units. Exact rational frame carry;
// no millisecond rounding and no large-duration multiplication overflow.
class FrameClock {
  public:
    bool configure(unsigned rate) noexcept {
        if (!AudioFormat{rate, 2}.valid())
            return false;
        rate_ = rate;
        reset();
        return true;
    }
    static unsigned long long timeForFrames(unsigned long long frames,
                                            unsigned rate = 48000) noexcept {
        if (!AudioFormat{rate, 2}.valid())
            return 0;
        return (frames / rate) * units + (frames % rate) * units / rate;
    }
    static unsigned long long toQpc(unsigned long long time,
                                    unsigned long long frequency) noexcept {
        // QPC clocks on supported Windows hardware are at most GHz scale.
        return (time / units) * frequency + (time % units) * frequency / units;
    }
    void reset() noexcept {
        remainder_ = 0;
    }
    static unsigned long long completedPackets(unsigned long long linearBytes,
                                               unsigned bufferBytes,
                                               unsigned notifications) noexcept {
        if (!notifications || !bufferBytes || bufferBytes % notifications)
            return 0;
        const auto packetBytes = bufferBytes / notifications;
        return packetBytes ? linearBytes / packetBytes : 0;
    }
    unsigned long long advance(unsigned long long elapsed) noexcept {
        const auto partial = (elapsed % units) * rate_ + remainder_;
        const auto frames = (elapsed / units) * rate_ + partial / units;
        remainder_ = partial % units;
        return frames;
    }
    unsigned long long fractionalTime() const noexcept {
        return remainder_ / rate_;
    }

  private:
    static constexpr unsigned long long units = 10000000;
    unsigned rate_ = 48000;
    unsigned long long remainder_ = 0;
};
} // namespace loopz_driver
