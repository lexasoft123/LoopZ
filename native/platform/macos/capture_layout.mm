#include <loopz/macos/capture_layout.hpp>
#include <AudioToolbox/AudioFormat.h>

#include <algorithm>
#include <cmath>

namespace loopz::macos {
namespace {
constexpr auto headerBytes = offsetof(AudioChannelLayout, mChannelDescriptions);
bool validLayout(const AudioChannelLayout& layout, uint32_t bytes) noexcept {
    if (bytes < headerBytes || bytes > 65536)
        return false;
    return layout.mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions ||
           (layout.mNumberChannelDescriptions <= kMaximumRoutingChannels &&
            layout.mNumberChannelDescriptions <=
                (bytes - headerBytes) / sizeof(AudioChannelDescription));
}
} // namespace

std::optional<CaptureChannelPlan> stereoCapturePlan(const AudioChannelLayout& layout,
                                                    uint32_t bytes, uint32_t channels,
                                                    std::string& error) {
    error.clear();
    if (channels == 0 || channels > kMaximumRoutingChannels || !validLayout(layout, bytes)) {
        error = "Invalid application output channel layout";
        return std::nullopt;
    }
    if (discreteCaptureLayout(layout, bytes, channels))
        return CaptureChannelPlan{discreteStereoConnections(channels), true};
    UInt32 count = 0, countBytes = sizeof(count);
    auto status = AudioFormatGetProperty(kAudioFormatProperty_NumberOfChannelsForLayout, bytes,
                                         &layout, &countBytes, &count);
    if (status || countBytes != sizeof(count) || count != channels) {
        error = "Application output layout does not match its stream";
        return std::nullopt;
    }
    // Application sources duplicate mono at unity. Audio Format Services uses
    // constant-power mono panning instead, which would reduce that source level.
    if (channels == 1)
        return CaptureChannelPlan{discreteStereoConnections(1), false};
    AudioChannelLayout stereo{};
    stereo.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
    const AudioChannelLayout* layouts[]{&layout, &stereo};
    std::vector<Float32> matrix(channels * 2);
    UInt32 matrixBytes = static_cast<UInt32>(matrix.size() * sizeof(Float32));
    status = AudioFormatGetProperty(kAudioFormatProperty_MatrixMixMap, sizeof(layouts), layouts,
                                    &matrixBytes, matrix.data());
    if (status || matrixBytes != matrix.size() * sizeof(Float32)) {
        error = "No standard stereo mix for application output layout (status " +
                std::to_string(status) + ")";
        return std::nullopt;
    }
    CaptureChannelPlan result;
    result.connections.reserve(matrix.size());
    for (uint32_t source = 0; source < channels; ++source) {
        for (uint32_t output = 0; output < 2; ++output) {
            // Audio Format Services returns input-major, output-minor rows.
            const auto gain = matrix[source * 2 + output];
            if (!std::isfinite(gain) || gain < 0 || gain > 1) {
                error = "Application stereo mix needs unsupported channel coefficients";
                return std::nullopt;
            }
            if (gain != 0)
                result.connections.push_back({source, output, gain});
        }
    }
    if (result.connections.empty()) {
        error = "Application output layout has no stereo destinations";
        return std::nullopt;
    }
    return result;
}

std::optional<CaptureChannelPlan> deviceCapturePlan(AudioDeviceID device, uint32_t channels,
                                                    std::string& error) {
    error.clear();
    if (channels == 0 || channels > kMaximumRoutingChannels) {
        error = "Invalid application output channel count";
        return std::nullopt;
    }
    const AudioObjectPropertyAddress address{kAudioDevicePropertyPreferredChannelLayout,
                                             kAudioObjectPropertyScopeOutput,
                                             kAudioObjectPropertyElementMain};
    if (!AudioObjectHasProperty(device, &address))
        return CaptureChannelPlan{discreteStereoConnections(channels), true};
    UInt32 bytes = 0;
    auto status = AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &bytes);
    if (status || bytes < headerBytes || bytes > 65536) {
        error = "Cannot inspect application output channel layout";
        return std::nullopt;
    }
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const auto capacity = bytes;
    status = AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, storage.data());
    if (status || bytes > capacity || bytes < headerBytes) {
        error = "Application output channel layout changed during inspection";
        return std::nullopt;
    }
    return stereoCapturePlan(*reinterpret_cast<const AudioChannelLayout*>(storage.data()), bytes,
                             channels, error);
}

std::optional<CaptureChannelPlan> deviceStreamCapturePlan(AudioDeviceID device,
                                                          uint32_t firstChannel, uint32_t channels,
                                                          uint32_t deviceChannels,
                                                          std::string& error) {
    if (!channels || firstChannel >= deviceChannels || channels > deviceChannels - firstChannel) {
        error = "Application output stream lies outside its device channel layout";
        return std::nullopt;
    }
    auto full = deviceCapturePlan(device, deviceChannels, error);
    if (!full)
        return std::nullopt;
    CaptureChannelPlan result;
    result.discrete = full->discrete;
    for (const auto& wire : full->connections)
        if (wire.sourceChannel >= firstChannel && wire.sourceChannel - firstChannel < channels)
            result.connections.push_back(
                {wire.sourceChannel - firstChannel, wire.outputChannel, wire.gain});
    return result;
}

bool sameCapturePlan(const CaptureChannelPlan& a, const CaptureChannelPlan& b) noexcept {
    return a.discrete == b.discrete && a.connections.size() == b.connections.size() &&
           std::equal(a.connections.begin(), a.connections.end(), b.connections.begin(),
                      [](const auto& left, const auto& right) {
                          return left.sourceChannel == right.sourceChannel &&
                                 left.outputChannel == right.outputChannel &&
                                 left.gain == right.gain;
                      });
}
bool identityStereoPlan(const CaptureChannelPlan& plan) noexcept {
    return plan.connections.size() == 2 && plan.connections[0].sourceChannel == 0 &&
           plan.connections[0].outputChannel == 0 && plan.connections[0].gain == 1 &&
           plan.connections[1].sourceChannel == 1 && plan.connections[1].outputChannel == 1 &&
           plan.connections[1].gain == 1;
}
} // namespace loopz::macos
