#include <loopz/capture_mix.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace loopz {
uint32_t sampleCaptureMix(std::span<const CaptureBuffer* const> captures, uint64_t firstHostNs,
                          double rate, float* left, float* right, uint32_t frames,
                          CaptureMixReader& reader) noexcept {
    if (left)
        std::fill_n(left, std::min(frames, kBlock), 0.0f);
    if (right)
        std::fill_n(right, std::min(frames, kBlock), 0.0f);
    if (!left || !right || !frames || frames > kBlock || captures.empty() ||
        captures.size() > kMaximumRoutingSources ||
        std::any_of(captures.begin(), captures.end(), [](const auto* capture) {
            return !capture;
        }))
        return frames;
    if (captures.size() == 1)
        return captures.front()->sample(firstHostNs, rate, left, right, frames, &reader.streams[0]);
    std::array<double, kBlock> sumL{}, sumR{};
    std::array<float, kBlock> inputL{}, inputR{};
    std::array<uint8_t, kBlock> incomplete{};
    for (size_t source = 0; source < captures.size(); ++source) {
        std::array<uint8_t, kBlock> available{};
        captures[source]->sample(firstHostNs, rate, inputL.data(), inputR.data(), frames,
                                 &reader.streams[source], {available.data(), frames});
        for (uint32_t frame = 0; frame < frames; ++frame) {
            sumL[frame] += inputL[frame];
            sumR[frame] += inputR[frame];
            incomplete[frame] |= !available[frame];
        }
    }
    uint32_t missing = 0;
    const auto finite = [](double value) {
        return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
                   ? static_cast<float>(value)
                   : 0.0f;
    };
    for (uint32_t frame = 0; frame < frames; ++frame) {
        left[frame] = finite(sumL[frame]);
        right[frame] = finite(sumR[frame]);
        missing += incomplete[frame];
    }
    return missing;
}
AudioReadResult sampleAudioHistoryMix(std::span<const AudioHistory* const> captures,
                                      uint64_t firstHostNs, double rate, float* left, float* right,
                                      uint32_t frames, AudioHistoryMixReader& reader) noexcept {
    if (left)
        std::fill_n(left, std::min(frames, kBlock), 0.0f);
    if (right)
        std::fill_n(right, std::min(frames, kBlock), 0.0f);
    if (!left || !right || !frames || frames > kBlock || captures.empty() ||
        captures.size() > kMaximumRoutingSources ||
        std::any_of(captures.begin(), captures.end(), [](const auto* capture) {
            return !capture || capture->channels() != 2;
        }))
        return {frames, false};
    const std::array<float*, 2> output{left, right};
    if (captures.size() == 1)
        return captures.front()->read(firstHostNs, rate, {output, frames}, frames,
                                      reader.streams[0]);
    std::array<double, kBlock> sumL{}, sumR{};
    std::array<float, kBlock> inputL{}, inputR{};
    std::array<uint8_t, kBlock> incomplete{};
    bool online = false;
    for (size_t source = 0; source < captures.size(); ++source) {
        std::array<uint8_t, kBlock> available{};
        const std::array<float*, 2> planes{inputL.data(), inputR.data()};
        const auto result =
            captures[source]->readWithCoverage(firstHostNs, rate, {planes, frames}, frames,
                                               reader.streams[source], {available.data(), frames});
        online = online || result.online;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            sumL[frame] += inputL[frame];
            sumR[frame] += inputR[frame];
            incomplete[frame] |= !available[frame];
        }
    }
    uint32_t missing = 0;
    const auto finite = [](double value) {
        return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
                   ? static_cast<float>(value)
                   : 0.0f;
    };
    for (uint32_t frame = 0; frame < frames; ++frame) {
        left[frame] = finite(sumL[frame]);
        right[frame] = finite(sumR[frame]);
        missing += incomplete[frame];
    }
    return {missing, online};
}
} // namespace loopz
