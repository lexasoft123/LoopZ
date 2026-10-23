#pragma once
#include "probe_recording_limits.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace loopz::windows {
// Diagnostic recorder only. One capture producer and one analysis consumer;
// all allocation precedes streaming. Queue exhaustion fails the measurement
// explicitly rather than blocking capture or silently discarding evidence.
class StreamingProbeCapture {
    static_assert(std::atomic<uint64_t>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free);

  public:
    static constexpr unsigned slotFrames = 4096, slotCount = 32;
    struct Spectrum {
        uint64_t frames{};
        double peak{}, target{}, other{};
    };
    explicit StreamingProbeCapture(unsigned rate, unsigned channels) : channels_(channels) {
        if (!validStressProbeRecording(rate, channels, 1500))
            throw std::invalid_argument("Unsupported streaming probe format");
        samples_.resize(size_t(slotCount) * slotFrames * channels);
        constexpr double tau = 6.283185307179586;
        for (unsigned c = 0; c < channels; ++c) {
            targetCos_[c] = otherCos_[c] = 1;
            const double a = tau * (443 + c * 37) / rate;
            const double b = tau * (443 + ((c + 1) % channels) * 37) / rate;
            targetStepSin_[c] = std::sin(a);
            targetStepCos_[c] = std::cos(a);
            otherStepSin_[c] = std::sin(b);
            otherStepCos_[c] = std::cos(b);
        }
    }
    StreamingProbeCapture(const StreamingProbeCapture&) = delete;
    StreamingProbeCapture& operator=(const StreamingProbeCapture&) = delete;

    // Only the capture producer calls push(). The input remains caller-owned.
    bool push(const float* interleaved, unsigned frames, bool silent = false) noexcept {
        if (!frames)
            return true;
        const uint64_t needed = (uint64_t(frames) + slotFrames - 1) / slotFrames;
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        if (finished_.load(std::memory_order_relaxed) || (!interleaved && !silent) ||
            needed > slotCount || head - tail > slotCount - needed) {
            droppedFrames_ += frames;
            return false;
        }
        unsigned offset = 0;
        for (uint64_t index = 0; index < needed; ++index) {
            const auto slot = static_cast<unsigned>((head + index) % slotCount);
            const unsigned count = std::min(frames - offset, slotFrames);
            auto* destination = samples_.data() + size_t(slot) * slotFrames * channels_;
            if (silent)
                std::fill_n(destination, size_t(count) * channels_, 0.0f);
            else
                std::memcpy(destination, interleaved + size_t(offset) * channels_,
                            size_t(count) * channels_ * sizeof(float));
            counts_[slot] = count;
            offset += count;
        }
        head_.store(head + needed, std::memory_order_release);
        acceptedFrames_ += frames;
        return true;
    }

    // Publish completion after the final push. A consumer that observes this
    // must drain once more: its previous empty observation may precede the final push.
    void finishProducer() noexcept {
        finished_.store(true, std::memory_order_release);
    }
    bool producerFinished() const noexcept {
        return finished_.load(std::memory_order_acquire);
    }

    // Only the analysis consumer calls consumeOne(). No allocation or I/O.
    bool consumeOne() noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        const auto slot = static_cast<unsigned>(tail % slotCount);
        const auto* source = samples_.data() + size_t(slot) * slotFrames * channels_;
        for (unsigned frame = 0; frame < counts_[slot]; ++frame) {
            bool zero = true;
            for (unsigned c = 0; c < channels_; ++c) {
                double value = source[size_t(frame) * channels_ + c];
                if (!std::isfinite(value)) {
                    ++invalidSamples_;
                    value = 0;
                }
                zero = zero && value == 0;
                peak_[c] = std::max(peak_[c], std::abs(value));
                if (frames_)
                    maximumStep_ = std::max(maximumStep_, std::abs(value - previous_[c]));
                previous_[c] = value;
                real_[c] += value * targetCos_[c];
                imaginary_[c] += value * targetSin_[c];
                otherReal_[c] += value * otherCos_[c];
                otherImaginary_[c] += value * otherSin_[c];
                const double nextTarget =
                    targetSin_[c] * targetStepCos_[c] + targetCos_[c] * targetStepSin_[c];
                targetCos_[c] =
                    targetCos_[c] * targetStepCos_[c] - targetSin_[c] * targetStepSin_[c];
                targetSin_[c] = nextTarget;
                const double nextOther =
                    otherSin_[c] * otherStepCos_[c] + otherCos_[c] * otherStepSin_[c];
                otherCos_[c] = otherCos_[c] * otherStepCos_[c] - otherSin_[c] * otherStepSin_[c];
                otherSin_[c] = nextOther;
            }
            if (zero) {
                ++zeroFrames_;
                if (++zeroRun_ > longestZeroRun_) {
                    longestZeroRun_ = zeroRun_;
                    longestZeroStart_ = frames_ + 1 - zeroRun_;
                }
            } else
                zeroRun_ = 0;
            ++frames_;
        }
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Inspect results only after both producer and consumer have stopped.
    Spectrum spectrum(unsigned channel) const {
        if (channel >= channels_)
            throw std::out_of_range("Streaming probe channel");
        const double scale = frames_ ? 2.0 / double(frames_) : 0;
        return {frames_, peak_[channel], std::hypot(real_[channel], imaginary_[channel]) * scale,
                std::hypot(otherReal_[channel], otherImaginary_[channel]) * scale};
    }
    uint64_t frames() const noexcept {
        return frames_;
    }
    uint64_t droppedFrames() const noexcept {
        return droppedFrames_;
    }
    uint64_t acceptedFrames() const noexcept {
        return acceptedFrames_;
    }
    uint64_t pendingPackets() const noexcept {
        return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed);
    }
    uint64_t invalidSamples() const noexcept {
        return invalidSamples_;
    }
    uint64_t zeroFrames() const noexcept {
        return zeroFrames_;
    }
    uint64_t longestZeroRun() const noexcept {
        return longestZeroRun_;
    }
    uint64_t longestZeroStart() const noexcept {
        return longestZeroStart_;
    }
    double maximumStep() const noexcept {
        return maximumStep_;
    }
    size_t sampleStorageBytes() const noexcept {
        return samples_.capacity() * sizeof(float);
    }

  private:
    unsigned channels_;
    std::vector<float> samples_;
    std::array<unsigned, slotCount> counts_{};
    alignas(64) std::atomic<uint64_t> head_{};
    alignas(64) std::atomic<uint64_t> tail_{};
    std::atomic<bool> finished_{};
    uint64_t droppedFrames_{}, acceptedFrames_{}, frames_{}, invalidSamples_{}, zeroFrames_{},
        zeroRun_{}, longestZeroRun_{}, longestZeroStart_{};
    double maximumStep_{};
    std::array<double, 64> targetSin_{}, targetCos_{}, otherSin_{}, otherCos_{}, targetStepSin_{},
        targetStepCos_{}, otherStepSin_{}, otherStepCos_{}, real_{}, imaginary_{}, otherReal_{},
        otherImaginary_{}, peak_{}, previous_{};
};
} // namespace loopz::windows
