#pragma once

#include <loopz/probe_signal.hpp>
#include <limits>
#include <span>
#include <stdexcept>

namespace loopz::probe {
struct ChannelPoint {
    uint64_t hostNs{};
    std::array<float, 64> samples{};
};
// One callback owner. Observe every block, not only the downsampled marker
// points: monotonic points alone cannot detect a missing callback interval.
struct CallbackCadence {
    uint64_t callbacks{}, errors{};
    uint32_t minimumFrames{UINT32_MAX}, maximumFrames{};

    bool observe(uint64_t hostNs, uint64_t sourceFrame, uint64_t sequence, uint32_t frames,
                 double rate, bool timestampValid) noexcept {
        const bool shape = frames && hostNs && std::isfinite(rate) && rate >= 8000 &&
                           rate <= 192000 && sourceFrame <= UINT64_MAX - frames;
        const bool contiguous =
            !previous_ || (shape && sourceFrame == nextFrame_ && sequence == nextSequence_ &&
                           rate == previousRate_ &&
                           std::abs(static_cast<long double>(hostNs) - nextHostNs_) <= 2e9L / rate);
        const bool valid = shape && timestampValid && contiguous;
        ++callbacks;
        minimumFrames = std::min(minimumFrames, frames);
        maximumFrames = std::max(maximumFrames, frames);
        if (!valid)
            ++errors;
        previous_ = shape && timestampValid;
        if (previous_) {
            nextFrame_ = sourceFrame + frames;
            nextSequence_ = sequence + 1;
            nextHostNs_ = hostNs + static_cast<long double>(frames) * 1e9L / rate;
            previousRate_ = rate;
        }
        return valid;
    }

  private:
    bool previous_{false};
    uint64_t nextFrame_{}, nextSequence_{};
    long double nextHostNs_{};
    double previousRate_{};
};
struct ChannelCheck {
    double delayMs{}, minimumCorrelation{1}, maximumCrossCorrelation{0};
    double minimumGain{std::numeric_limits<double>::infinity()}, maximumGain{0};
    double maximumMagnitude{0};
    bool finite{true};
};

// Analyze after capture has stopped. Every channel carries its own time code;
// an aggregate peak or a stereo-only check cannot certify wide channel wiring.
inline ChannelCheck checkChannels(std::span<const ChannelPoint> points, uint32_t channels) {
    if (channels == 0 || channels > 64 || points.size() < 500)
        throw std::invalid_argument("Channel marker check needs 1–64 channels and 500 points");
    ChannelCheck result;
    double best = -1;
    uint64_t delayNs = 0;
    const auto timingChannels = std::min<uint32_t>(channels, 4);
    for (uint64_t delay = 0; delay <= 100000000; delay += 250000) {
        double dot = 0, energy = 0;
        for (const auto& point : points) {
            if (point.hostNs < delay)
                throw std::invalid_argument("Channel marker timestamp precedes its search window");
            for (uint32_t channel = 0; channel < timingChannels; ++channel) {
                const double sample = point.samples[channel];
                if (!std::isfinite(sample)) {
                    result.finite = false;
                    continue;
                }
                dot += sample * marker(point.hostNs - delay, channel);
                energy += sample * sample;
            }
        }
        const double correlation =
            energy > 0 ? dot / std::sqrt(energy * points.size() * timingChannels * 0.03 * 0.03) : 0;
        if (correlation > best) {
            best = correlation;
            delayNs = delay;
        }
    }
    result.delayMs = static_cast<double>(delayNs) / 1000000;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        double energy = 0, dot = 0;
        std::array<double, 64> cross{};
        for (const auto& point : points) {
            const double sample = point.samples[channel];
            if (!std::isfinite(sample)) {
                result.finite = false;
                continue;
            }
            energy += sample * sample;
            result.maximumMagnitude = std::max(result.maximumMagnitude, std::abs(sample));
            for (uint32_t other = 0; other < channels; ++other) {
                const auto value = sample * marker(point.hostNs - delayNs, other);
                if (other == channel)
                    dot += value;
                else
                    cross[other] += value;
            }
        }
        const double denominator = std::sqrt(energy * points.size() * 0.03 * 0.03);
        result.minimumCorrelation =
            std::min(result.minimumCorrelation, denominator > 0 ? dot / denominator : 0);
        const auto gain = dot / (points.size() * 0.03 * 0.03);
        result.minimumGain = std::min(result.minimumGain, gain);
        result.maximumGain = std::max(result.maximumGain, gain);
        for (uint32_t other = 0; other < channels; ++other)
            if (other != channel && denominator > 0)
                result.maximumCrossCorrelation =
                    std::max(result.maximumCrossCorrelation, std::abs(cross[other] / denominator));
    }
    return result;
}
} // namespace loopz::probe
