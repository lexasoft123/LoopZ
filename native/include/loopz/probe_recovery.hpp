#pragma once

#include <loopz/probe_channels.hpp>
#include <atomic>
#include <vector>

namespace loopz::probe {
enum class RecoveryPhase : uint32_t { Warmup, Baseline, Disconnected, Recovered };
static_assert(std::atomic<RecoveryPhase>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

struct RecoverySegment {
    std::vector<ChannelPoint> points;
    size_t count{};
    uint64_t frames{}, zeroSamples{}, nonfinite{}, timestampErrors{}, overflows{};
    uint64_t zeroFrames{}, longestZeroRun{}, zeroRun{};
};

// One callback writes samples. Control advances phases monotonically. After
// observed reaches a later phase, prior segments are immutable and can be read
// on control without stopping the client. Read the final segment only after IO
// has stopped. Storage is allocated before opening any audio endpoint.
class RecoveryCapture {
  public:
    RecoveryCapture(uint32_t channels, uint32_t rate) : channels_(channels), rate_(rate) {
        if (!channels || channels > 64 || rate < 8000 || rate > 192000)
            throw std::invalid_argument("Invalid recovery capture format");
        segments_[1].points.resize(8800);
        segments_[2].points.resize(30800);
        segments_[3].points.resize(5500);
    }
    void advance(RecoveryPhase next) {
        const auto current = requested_.load();
        if (static_cast<uint32_t>(next) > static_cast<uint32_t>(RecoveryPhase::Recovered) ||
            static_cast<uint32_t>(next) != static_cast<uint32_t>(current) + 1)
            throw std::invalid_argument("Recovery phases must advance once in order");
        requested_.store(next, std::memory_order_release);
    }
    RecoveryPhase observed() const noexcept {
        return observed_.load(std::memory_order_acquire);
    }
    uint64_t continuousSignalFrames() const noexcept {
        return signalFrames_.load(std::memory_order_acquire);
    }
    const RecoverySegment& segment(RecoveryPhase phase) const noexcept {
        return segments_[static_cast<uint32_t>(phase)];
    }
    void append(const float* const* input, uint32_t frames, uint64_t hostNs,
                bool timestampValid) noexcept {
        const auto phase = requested_.load(std::memory_order_acquire);
        if (phase != previousPhase_) {
            consecutiveSignalFrames_ = 0;
            previousPhase_ = phase;
        }
        if (phase != RecoveryPhase::Warmup) {
            auto& segment = segments_[static_cast<uint32_t>(phase)];
            bool healthy =
                timestampValid && (!nextHostNs_ || std::abs(static_cast<long double>(hostNs) -
                                                            nextHostNs_) <= 2e9L / rate_);
            if (!healthy)
                ++segment.timestampErrors;
            nextHostNs_ = hostNs + static_cast<uint64_t>(static_cast<double>(frames) * 1e9 / rate_);
            const auto step = std::max(1U, rate_ / 1000);
            for (uint32_t frame = 0; frame < frames; ++frame) {
                bool zero = true;
                for (uint32_t channel = 0; channel < channels_; ++channel) {
                    const auto sample = input[channel][frame];
                    healthy &= std::isfinite(sample) && sample != 0;
                    if (!std::isfinite(sample))
                        ++segment.nonfinite;
                    else if (sample == 0)
                        ++segment.zeroSamples;
                    zero &= sample == 0;
                }
                if (zero) {
                    ++segment.zeroFrames;
                    segment.longestZeroRun = std::max(segment.longestZeroRun, ++segment.zeroRun);
                } else {
                    segment.zeroRun = 0;
                }
                if ((segment.frames + frame) % step)
                    continue;
                if (segment.count == segment.points.size()) {
                    ++segment.overflows;
                    continue;
                }
                auto& point = segment.points[segment.count++];
                point.hostNs = hostNs + static_cast<uint64_t>((frame + 0.5) * 1e9 / rate_);
                for (uint32_t channel = 0; channel < channels_; ++channel)
                    point.samples[channel] = input[channel][frame];
            }
            segment.frames += frames;
            consecutiveSignalFrames_ =
                healthy ? std::min(consecutiveSignalFrames_, UINT64_MAX - frames) + frames : 0;
            signalFrames_.store(consecutiveSignalFrames_, std::memory_order_release);
        }
        observed_.store(phase, std::memory_order_release);
    }

  private:
    const uint32_t channels_, rate_;
    std::array<RecoverySegment, 4> segments_;
    std::atomic<RecoveryPhase> requested_{RecoveryPhase::Warmup};
    std::atomic<RecoveryPhase> observed_{RecoveryPhase::Warmup};
    std::atomic<uint64_t> signalFrames_{0};
    RecoveryPhase previousPhase_{RecoveryPhase::Warmup};
    uint64_t consecutiveSignalFrames_{};
    uint64_t nextHostNs_{};
};

inline bool recoveredSignal(const RecoverySegment& segment, uint32_t channels) {
    if (segment.count < 500 || segment.nonfinite || segment.timestampErrors || segment.overflows ||
        segment.zeroSamples)
        return false;
    const auto signal = checkChannels({segment.points.data(), segment.count}, channels);
    return signal.finite && signal.minimumCorrelation > 0.90 &&
           signal.maximumCrossCorrelation < 0.35 && signal.minimumGain > 0.90 &&
           signal.maximumGain < 1.10;
}
} // namespace loopz::probe
