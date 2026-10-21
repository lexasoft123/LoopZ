#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace loopz::probe {
struct ZeroSpan {
    uint64_t capturedFrame, hostNs, receivedNs, callback;
    uint32_t frames, channels, renderFlags;
};
struct CallbackGap {
    uint64_t capturedFrame, hostNs, receivedNs, callback;
    double sampleFrameJump;
    int64_t hostTimeJumpNs;
    bool timestampValid;
};

// One serial callback producer; inspect only after callbacks have drained.
// Zero spans are split at callback boundaries and contain no audio samples.
class CaptureTrace {
  public:
    static constexpr size_t capacity = 512;
    void observe(std::span<const float> left, std::span<const float> right, double sampleTime,
                 uint64_t hostNs, uint64_t receivedNs, double rate, bool valid,
                 uint32_t renderFlags) noexcept {
        if (left.empty() || left.size() != right.size() || left.size() > 4096 ||
            !std::isfinite(rate) || rate < 8000 || rate > 192000) {
            ++invalidBlocks;
            return;
        }
        const auto frames = static_cast<uint32_t>(left.size());
        valid = valid && hostNs && hostNs <= UINT64_MAX - 1000000000ULL &&
                std::isfinite(sampleTime) && std::abs(sampleTime) <= double(uint64_t{1} << 53);
        double sampleJump = 0;
        int64_t hostJump = 0;
        if (valid && previousValid_) {
            sampleJump = sampleTime - expectedSample_;
            const auto difference =
                hostNs >= expectedHost_ ? hostNs - expectedHost_ : expectedHost_ - hostNs;
            const auto bounded = static_cast<int64_t>(std::min(difference, uint64_t{INT64_MAX}));
            hostJump = hostNs >= expectedHost_ ? bounded : -bounded;
        }
        if (!valid || (previousValid_ && (std::abs(sampleJump) > .5 ||
                                          std::abs(static_cast<double>(hostJump)) > 2e9 / rate))) {
            if (gapCount_ < gaps_.size())
                gaps_[gapCount_++] = {capturedFrames, hostNs,   receivedNs, callbacks,
                                      sampleJump,     hostJump, valid};
            else
                ++overflows;
        }
        expectedSample_ = sampleTime + frames;
        expectedHost_ =
            valid ? hostNs + static_cast<uint64_t>(std::llround(frames * 1e9 / rate)) : 0;
        previousValid_ = valid;
        uint32_t mask = 0, begin = 0;
        const auto publish = [&](uint32_t end) {
            if (!mask)
                return;
            if (zeroCount_ < zeros_.size()) {
                const auto time = valid ? hostNs + static_cast<uint64_t>(begin * 1e9 / rate) : 0;
                zeros_[zeroCount_++] = {capturedFrames + begin,
                                        time,
                                        receivedNs,
                                        callbacks,
                                        end - begin,
                                        mask,
                                        renderFlags};
            } else
                ++overflows;
        };
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const uint32_t next = (left[frame] == 0 ? 1U : 0U) | (right[frame] == 0 ? 2U : 0U);
            zeroSamples += (next & 1) + ((next >> 1) & 1);
            if (next != mask) {
                publish(frame);
                mask = next;
                begin = frame;
            }
        }
        publish(frames);
        if (!callbacks)
            firstReceivedNs = receivedNs;
        lastReceivedNs = receivedNs;
        ++callbacks;
        capturedFrames += frames;
    }
    std::span<const ZeroSpan> zeros() const noexcept {
        return {zeros_.data(), zeroCount_};
    }
    std::span<const CallbackGap> gaps() const noexcept {
        return {gaps_.data(), gapCount_};
    }
    uint64_t capturedFrames{0}, callbacks{0}, zeroSamples{0}, overflows{0}, invalidBlocks{0};
    uint64_t firstReceivedNs{0}, lastReceivedNs{0};

  private:
    std::array<ZeroSpan, capacity> zeros_{};
    std::array<CallbackGap, capacity> gaps_{};
    size_t zeroCount_{0}, gapCount_{0};
    bool previousValid_{false};
    double expectedSample_{0};
    uint64_t expectedHost_{0};
};
} // namespace loopz::probe
