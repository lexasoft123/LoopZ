#pragma once

#include <CoreAudio/CoreAudio.h>
#include <cstddef>
#include <cstdint>
#include <loopz/routing.hpp>
#include <optional>

namespace loopz::macos {
struct CaptureChannelPlan {
    std::vector<ChannelConnection> connections;
    bool discrete{false};
};
// Control-domain queries only. The returned portable matrix is prepared in
// SingZ's graph before any capture callback starts; no format APIs run in IO.
std::optional<CaptureChannelPlan> stereoCapturePlan(const AudioChannelLayout& layout,
                                                    uint32_t bytes, uint32_t channels,
                                                    std::string& error);
std::optional<CaptureChannelPlan> deviceCapturePlan(AudioDeviceID device, uint32_t channels,
                                                    std::string& error);
std::optional<CaptureChannelPlan> deviceStreamCapturePlan(AudioDeviceID device,
                                                          uint32_t firstChannel, uint32_t channels,
                                                          uint32_t deviceChannels,
                                                          std::string& error);
bool sameCapturePlan(const CaptureChannelPlan& a, const CaptureChannelPlan& b) noexcept;
bool identityStereoPlan(const CaptureChannelPlan& plan) noexcept;

// HAL can synthesize an all-unknown description array for a numbered interface
// that publishes no speaker layout. Treat that as discrete, without treating an
// explicit surround speaker assignment as alternating stereo pairs.
inline bool discreteCaptureLayout(const AudioChannelLayout& layout, uint32_t bytes,
                                  uint32_t channels) noexcept {
    constexpr auto headerBytes = offsetof(AudioChannelLayout, mChannelDescriptions);
    if (channels == 0 || channels > 64 || bytes < headerBytes)
        return false;
    if (layout.mChannelLayoutTag == (kAudioChannelLayoutTag_DiscreteInOrder | channels) ||
        layout.mChannelLayoutTag == (kAudioChannelLayoutTag_Unknown | channels))
        return true;
    if (layout.mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions ||
        layout.mNumberChannelDescriptions != channels ||
        bytes - headerBytes < channels * sizeof(AudioChannelDescription))
        return false;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const auto& description = layout.mChannelDescriptions[channel];
        if ((description.mChannelLabel != kAudioChannelLabel_Unknown &&
             description.mChannelLabel != (kAudioChannelLabel_Discrete_0 | channel)) ||
            description.mChannelFlags != kAudioChannelFlags_AllOff)
            return false;
    }
    return true;
}
} // namespace loopz::macos
