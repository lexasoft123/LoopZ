#include <loopz/audio_meter.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace loopz {
namespace {
struct Levels {
    float peak;
    double energy;
};
Levels measure(const float* samples, uint32_t frames) noexcept {
    if (!samples)
        return {};
    Levels result{};
    uint32_t frame = 0;
#if defined(__SSE2__) || defined(_M_X64)
    // x64 requires only SSE2. Keep squared samples in double precision so
    // finite float extremes cannot overflow the meter's energy accumulator.
    auto peak = _mm_setzero_ps();
    auto lowEnergy = _mm_setzero_pd(), highEnergy = _mm_setzero_pd();
    const auto absoluteMask = _mm_set1_epi32(0x7fffffff);
    const auto infinityBits = _mm_set1_epi32(0x7f800000);
    for (; frames - frame >= 4; frame += 4) {
        const auto raw = _mm_loadu_ps(samples + frame);
        const auto magnitude = _mm_and_si128(_mm_castps_si128(raw), absoluteMask);
        const auto finite = _mm_cmpgt_epi32(infinityBits, magnitude);
        const auto value = _mm_and_ps(raw, _mm_castsi128_ps(finite));
        peak = _mm_max_ps(peak, _mm_castsi128_ps(_mm_and_si128(magnitude, finite)));
        const auto low = _mm_cvtps_pd(value);
        const auto high = _mm_cvtps_pd(_mm_movehl_ps(value, value));
        lowEnergy = _mm_add_pd(lowEnergy, _mm_mul_pd(low, low));
        highEnergy = _mm_add_pd(highEnergy, _mm_mul_pd(high, high));
    }
    std::array<float, 4> peaks{};
    std::array<double, 2> low{}, high{};
    _mm_storeu_ps(peaks.data(), peak);
    _mm_storeu_pd(low.data(), lowEnergy);
    _mm_storeu_pd(high.data(), highEnergy);
    for (float value : peaks)
        result.peak = std::max(result.peak, value);
    result.energy = (low[0] + low[1]) + (high[0] + high[1]);
#endif
    for (; frame < frames; ++frame) {
        const float value = std::isfinite(samples[frame]) ? samples[frame] : 0;
        result.peak = std::max(result.peak, std::fabs(value));
        result.energy += static_cast<double>(value) * value;
    }
    return result;
}
} // namespace

AudioMeter::AudioMeter(uint32_t channels) : channels_(channels) {
    if (!channels || channels > kMaximumRoutingChannels)
        throw std::invalid_argument("Invalid audio meter channel count");
}
void AudioMeter::publish(RoutingInput input, uint32_t frames) noexcept {
    if (input.channels.size() != channels_) {
        clear();
        return;
    }
    publishPartial(input, frames);
}
void AudioMeter::publishPartial(RoutingInput input, uint32_t frames) noexcept {
    if (!frames || frames > input.capacityFrames || input.channels.empty() ||
        input.channels.size() > channels_ ||
        std::any_of(input.channels.begin(), input.channels.end(), [](auto* p) {
            return !p;
        })) {
        clear();
        return;
    }
    version_.fetch_add(1);
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        const float* samples = channel < input.channels.size() ? input.channels[channel] : nullptr;
        const auto [peak, energy] = measure(samples, frames);
        peak_[channel].store(std::bit_cast<uint32_t>(peak));
        rms_[channel].store(
            std::bit_cast<uint32_t>(static_cast<float>(std::sqrt(energy / frames))));
    }
    frames_.store(frames);
    version_.fetch_add(1);
}
void AudioMeter::publishSnapshot(const AudioMeterSnapshot& snapshot) noexcept {
    if (!snapshot.frames || snapshot.channels < channels_ ||
        snapshot.channels > kMaximumRoutingChannels) {
        clear();
        return;
    }
    version_.fetch_add(1);
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        const auto peak = snapshot.peak[channel], rms = snapshot.rms[channel];
        peak_[channel].store(
            std::bit_cast<uint32_t>(std::isfinite(peak) && peak >= 0 ? peak : 0.0f));
        rms_[channel].store(std::bit_cast<uint32_t>(std::isfinite(rms) && rms >= 0 ? rms : 0.0f));
    }
    frames_.store(snapshot.frames);
    version_.fetch_add(1);
}
void AudioMeter::publishSilence(uint32_t frames) noexcept {
    version_.fetch_add(1);
    for (uint32_t channel = 0; channel < channels_; ++channel) {
        peak_[channel].store(0);
        rms_[channel].store(0);
    }
    frames_.store(frames);
    version_.fetch_add(1);
}
void AudioMeter::clear() noexcept {
    publishSilence(0);
}
bool AudioMeter::read(AudioMeterSnapshot& result) const noexcept {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto version = version_.load();
        if (!version || version & 1)
            continue;
        AudioMeterSnapshot snapshot;
        snapshot.channels = channels_;
        snapshot.frames = frames_.load();
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            snapshot.peak[channel] = std::bit_cast<float>(peak_[channel].load());
            snapshot.rms[channel] = std::bit_cast<float>(rms_[channel].load());
        }
        if (version_.load() == version) {
            result = snapshot;
            return true;
        }
    }
    return false;
}
} // namespace loopz
