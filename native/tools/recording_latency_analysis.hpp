#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <span>
#include <stdexcept>
#include <vector>
#include <numbers>

namespace loopz::probe {

// Offline, centered FIR applied identically to both captures. Matching the
// common voice band avoids near-Nyquist quantization noise dominating the
// fractional-phase fit. Equal centered filters add no relative sample delay.
inline std::vector<float> matchingBand(std::span<const float> input) {
    constexpr int radius = 24;
    constexpr double cutoff = 0.125;
    std::array<double, 2 * radius + 1> weights{};
    double total = 0;
    for (int tap = -radius; tap <= radius; ++tap) {
        const auto x = 2 * std::numbers::pi * cutoff * tap;
        const auto sinc = tap ? std::sin(x) / x : 1;
        const auto window = 0.5 + 0.5 * std::cos(std::numbers::pi * tap / radius);
        weights[tap + radius] = sinc * window;
        total += sinc * window;
    }
    std::vector<float> result(input.size());
    for (size_t i = radius; i + radius < input.size(); ++i) {
        double sum = 0;
        for (int tap = -radius; tap <= radius; ++tap)
            sum += input[size_t(int64_t(i) + tap)] * weights[tap + radius];
        result[i] = float(sum / total);
    }
    return result;
}

struct WaveformMatch {
    double lagFrames;
    double correlation;
};

// Different capture clients can start at unrelated sample indices. Find their
// correspondence from signal content; their advertised timestamps are not used.
inline WaveformMatch matchWaveforms(std::span<const float> reference,
                                    std::span<const float> recording, int first, int count,
                                    int center, int radius) {
    if (first < 16 || count < 64 || first + count > int(reference.size()) || radius < 8)
        throw std::invalid_argument("Invalid waveform matching interval");
    const auto correlate = [&](int lag, int stride) {
        if (first + lag < 16 || first + count + lag > int(recording.size()))
            return 0.0;
        double xy = 0, xx = 0, yy = 0;
        for (int i = first; i < first + count; i += stride) {
            double x = 0, y = 0;
            for (int offset = 0; offset < stride; ++offset) {
                x += reference[i - offset] - 0.97 * reference[i - offset - stride];
                y += recording[i + lag - offset] - 0.97 * recording[i + lag - offset - stride];
            }
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0.0;
    };
    // Decimation can rank a narrow-band alias above the correct lag. Keep
    // several coarse hypotheses and choose using full-resolution correlation;
    // a single coarse winner can reject two otherwise identical recordings.
    std::array<WaveformMatch, 32> candidates{};
    for (int lag = center - radius; lag <= center + radius; lag += 8) {
        const auto value = correlate(lag, 8);
        if (std::abs(value) <= std::abs(candidates.back().correlation))
            continue;
        size_t position = candidates.size() - 1;
        while (position && std::abs(value) > std::abs(candidates[position - 1].correlation)) {
            candidates[position] = candidates[position - 1];
            --position;
        }
        candidates[position] = {double(lag), value};
    }
    WaveformMatch result{double(center), 0};
    std::array<WaveformMatch, candidates.size()> refined{};
    for (size_t index = 0; index < candidates.size(); ++index) {
        const auto candidate = candidates[index];
        if (!candidate.correlation)
            continue;
        const auto coarse = int(candidate.lagFrames);
        for (int lag = coarse - 8; lag <= coarse + 8; ++lag) {
            const auto value = correlate(lag, 1);
            if (std::abs(value) > std::abs(refined[index].correlation))
                refined[index] = {double(lag), value};
        }
    }
    // A hardware-clock source is resampled onto the virtual clock. Refine the
    // fractional phase rather than rejecting valid audio between integer lags.
    const auto interpolate = [&](double position) {
        const auto index = int(std::floor(position));
        const auto t = position - index;
        const double a = recording[index - 1], b = recording[index];
        const double c = recording[index + 1], d = recording[index + 2];
        return b + 0.5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
    };
    for (size_t index = 0; index < refined.size(); ++index) {
        if (!refined[index].correlation ||
            std::any_of(refined.begin(), refined.begin() + index, [&](const auto& previous) {
                return previous.correlation && previous.lagFrames == refined[index].lagFrames;
            }))
            continue;
        const auto integer = refined[index].lagFrames;
        for (int phase = -16; phase <= 16; ++phase) {
            const auto lag = integer + phase / 32.0;
            if (first + lag < 17 || first + count + lag + 2 >= recording.size())
                continue;
            double xy = 0, xx = 0, yy = 0;
            for (int i = first; i < first + count; ++i) {
                const auto x = reference[i] - 0.97 * reference[i - 1];
                const auto y = interpolate(i + lag) - 0.97 * interpolate(i + lag - 1);
                xy += x * y;
                xx += x * x;
                yy += y * y;
            }
            const auto value = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
            if (std::abs(value) > std::abs(result.correlation))
                result = {lag, value};
        }
    }
    return result;
}

struct RecordingBlock {
    uint64_t firstFrame, acquiredNs, receivedNs;
    uint32_t frames;
};

inline double acquisitionTime(std::span<const RecordingBlock> blocks, double frame, double rate) {
    const auto found = std::upper_bound(blocks.begin(), blocks.end(), frame,
                                        [](double position, const auto& block) {
                                            return position < block.firstFrame;
                                        });
    if (found == blocks.begin())
        throw std::out_of_range("Reference frame predates capture");
    const auto& block = *std::prev(found);
    if (frame >= block.firstFrame + block.frames || !block.acquiredNs)
        throw std::out_of_range("Reference frame is outside valid capture");
    return block.acquiredNs + (frame - block.firstFrame) * 1e9 / rate;
}

// Reference acquisition is the driver-provided timestamp, before correcting
// any device/stream hardware delay. A block becomes available at receivedNs. All its
// samples share that receipt time: do not manufacture earlier per-sample arrivals.
inline double deliveredAgeNs(const RecordingBlock& recording, uint32_t offset, double lag,
                             std::span<const RecordingBlock> reference, double rate) {
    if (offset >= recording.frames)
        throw std::out_of_range("Recording offset is outside its block");
    const auto frame = double(recording.firstFrame) + offset - lag;
    return recording.receivedNs - acquisitionTime(reference, frame, rate);
}

// The driver timestamp describes transfer from hardware into its buffer.
// Device + selected-stream latency locates the earlier physical input event.
// Safety offsets and IO periods are already represented by receipt minus
// timestamp; adding them here would count software buffering twice.
inline double hardwareAdjustedDeliveryAgeNs(double driverAgeNs, uint32_t deviceFrames,
                                            uint32_t streamFrames, double rate) {
    if (!std::isfinite(driverAgeNs) || !std::isfinite(rate) || rate < 8000 || rate > 192000)
        throw std::invalid_argument("Invalid hardware latency correction");
    const auto frames = uint64_t(deviceFrames) + streamFrames;
    return driverAgeNs + frames * 1e9 / rate;
}

} // namespace loopz::probe
