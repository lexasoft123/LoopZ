#pragma once
#include <loopz/audio_history.hpp>
#include <zcore/device/audio_host.h>
#include <algorithm>
#include <cmath>
namespace loopz::runtime::detail {
struct EstimatedOutputTimeline {
    struct Result {
        uint64_t ns;
        double hz;
    };
    // Whole host nanoseconds never enter floating arithmetic. In particular,
    // long double is only 53 bits on Windows/Apple arm64: repeatedly adding a
    // period to an absolute uptime loses sub-ns corrections after hours.
    uint64_t projectedNs{0};
    long double projectedFractionNs{0}, phaseErrorNs{0};
    double previousHz{0}, nominalHz{0};
    long double basePeriodNs{0};
    uint64_t acquisitionFrames{0};
    uint64_t previousFrame{0}, previousRawNs{0};
    uint32_t previousFrames{0};
    void clear() noexcept {
        *this = {};
    }
    Result observe(uint64_t frame, uint64_t rawNs, uint32_t frames, double hz,
                   bool discontinuity) noexcept {
        if (!rawNs || rawNs >= UINT64_MAX - 10000000000ULL || !frames ||
            frames > singz::kAudioHostMaxFrames || frame > UINT64_MAX - frames ||
            !std::isfinite(hz) || hz < AudioHistory::kMinimumSampleRate ||
            hz > AudioHistory::kMaximumSampleRate) {
            clear();
            return {rawNs, 0};
        }
        const bool continuous = projectedNs && !discontinuity && nominalHz == hz &&
                                previousFrame <= UINT64_MAX - previousFrames &&
                                frame == previousFrame + previousFrames && rawNs > previousRawNs;
        uint64_t start = rawNs;
        long double fraction = 0;
        double selectedHz = hz;
        bool reset = !continuous;
        if (continuous) {
            const long double advance =
                projectedFractionNs + static_cast<long double>(previousFrames) * 1e9L / previousHz;
            const auto whole = static_cast<uint64_t>(advance);
            fraction = advance - whole;
            start = projectedNs + whole;
            // Subtract the integer clocks first; the remaining error is only
            // a local phase, independent of absolute monotonic-clock uptime.
            const long double error = (rawNs >= start ? static_cast<long double>(rawNs - start)
                                                      : -static_cast<long double>(start - rawNs)) -
                                      fraction;
            phaseErrorNs = error;
            reset = !start || start >= UINT64_MAX - 10000000000ULL || std::abs(error) > 2000000.0L;
            if (!reset) {
                // Acquire device drift quickly after an explicit boundary,
                // then reject timestamp-observer jitter with a slow alpha-beta
                // clock servo. Only sample spacing changes, never block phase.
                const auto coefficient =
                    acquisitionFrames < static_cast<uint64_t>(hz) ? 1.0L / 16 : 1.0L / 8192;
                const auto alpha = std::min(.25L, coefficient * frames / 256);
                const auto beta = alpha * alpha / 4;
                const auto minimum = 1e9L / (hz * 1.005);
                const auto maximum = 1e9L / (hz * .995);
                basePeriodNs = std::clamp(basePeriodNs + beta * error / frames, minimum, maximum);
                const auto period =
                    std::clamp(basePeriodNs + alpha * error / frames, minimum, maximum);
                selectedHz = std::clamp(static_cast<double>(1e9L / period), hz * .995, hz * 1.005);
            }
        }
        if (reset) {
            start = rawNs;
            basePeriodNs = 1e9L / hz;
            phaseErrorNs = 0;
            fraction = 0;
            acquisitionFrames = 0;
        }
        if (acquisitionFrames < static_cast<uint64_t>(hz))
            acquisitionFrames += frames;
        projectedNs = start;
        projectedFractionNs = fraction;
        previousHz = selectedHz;
        nominalHz = hz;
        previousFrame = frame;
        previousRawNs = rawNs;
        previousFrames = frames;
        return {static_cast<uint64_t>(start), selectedHz};
    }
};
} // namespace loopz::runtime::detail
