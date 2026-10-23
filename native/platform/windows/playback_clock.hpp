#pragma once
#include "driver/audio_format.hpp"
#include <cstdint>
#include <limits>

namespace loopz::windows {
// The software WaveRT clock runs at its exact nominal rate but serializes
// packet timestamps in whole 100 ns ticks. Reconstruct continuous nanosecond
// anchors from frame counts, retaining the first observed timestamp. This
// removes only tick quantization; real gaps and explicit discontinuities reset
// the clock rather than being smoothed into a fictitious sample rate.
class PlaybackClock {
  public:
    struct Stamp {
        bool valid{}, discontinuity{};
        uint64_t hostNs{}, sourceFrame{};
    };
    explicit PlaybackClock(unsigned rate) noexcept : rate_(rate) {}
    void reset() noexcept { anchor_ = frames_ = 0; }
    Stamp append(uint64_t ticks, unsigned frames, bool discontinuity) noexcept {
        constexpr auto maximum = std::numeric_limits<uint64_t>::max();
        if (!loopz_driver::AudioFormat{rate_, 2}.valid() || !ticks || ticks > maximum / 100 ||
            !frames || frames > 480 || frames_ > maximum - frames)
            return {};
        const auto observed = ticks * 100;
        uint64_t predicted = observed;
        if (anchor_ && !discontinuity) {
            const auto seconds = frames_ / rate_;
            const auto remainder = (frames_ % rate_) * 1000000000ULL / rate_;
            if (seconds > maximum / 1000000000ULL)
                return {};
            const auto whole = seconds * 1000000000ULL;
            if (whole > maximum - remainder || anchor_ > maximum - whole - remainder)
                return {};
            predicted = anchor_ + whole + remainder;
            const auto difference = observed > predicted ? observed - predicted : predicted - observed;
            if (difference > 100)
                discontinuity = true;
        }
        if (!anchor_ || discontinuity) {
            anchor_ = predicted = observed;
            frames_ = 0;
        }
        const Stamp result{true, discontinuity, predicted, frames_};
        frames_ += frames;
        return result;
    }
  private:
    const unsigned rate_;
    uint64_t anchor_{}, frames_{};
};
} // namespace loopz::windows
