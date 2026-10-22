#include <loopz/audio_meter.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace loopz {
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
        float peak = 0;
        double energy = 0;
        if (samples)
            for (uint32_t frame = 0; frame < frames; ++frame) {
                const float value = std::isfinite(samples[frame]) ? samples[frame] : 0;
                peak = std::max(peak, std::fabs(value));
                energy += static_cast<double>(value) * value;
            }
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
