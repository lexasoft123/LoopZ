#pragma once

#include <loopz/probe_signal.hpp>
#include <zcore/device/audio_host.h>
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>

namespace loopz::macos::diagnostic {
inline AudioObjectID requireRoutingDevice(const std::string& uid) {
    if (!uid.starts_with("com.lexasoft.loopz.device."))
        throw std::invalid_argument("IO checks require a LoopZ routing device");
    CFStringRef text = (__bridge CFStringRef) @(uid.c_str());
    AudioObjectID device = 0;
    UInt32 bytes = sizeof(device);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(text), &text, &bytes,
                                   &device) ||
        !device)
        throw std::invalid_argument("The LoopZ routing device is unavailable");
    CFStringRef model = nullptr;
    bytes = sizeof(model);
    address.mSelector = kAudioDevicePropertyModelUID;
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &model) || !model)
        throw std::invalid_argument("The routing device identity could not be verified");
    const bool matched = CFEqual(model, CFSTR("com.lexasoft.loopz.device.model.v2"));
    CFRelease(model);
    UInt32 transport = 0;
    bytes = sizeof(transport);
    address.mSelector = kAudioDevicePropertyTransportType;
    if (!matched || AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &transport) ||
        transport != kAudioDeviceTransportTypeVirtual)
        throw std::invalid_argument("IO checks cannot open this device");
    return device;
}
struct Writer {
    uint32_t channels;
    uint64_t timestampErrors{0};
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Writer*>(context);
        if (!block.output || block.outputChannels != self.channels)
            return false;
        if (!block.outputTimestampValid || !block.outputTimestampHardware)
            ++self.timestampErrors;
        for (uint32_t channel = 0; channel < self.channels; ++channel)
            for (uint32_t frame = 0; frame < block.frames; ++frame) {
                const auto time = block.outputHostTimeNs +
                                  static_cast<uint64_t>((frame + 0.5) * 1e9 / block.sampleRate);
                block.output[channel][frame] = static_cast<float>(probe::marker(time, channel));
            }
        return true;
    }
};
inline singz::AudioHostConfig config(const std::string& uid, uint32_t channels, bool input,
                                     uint32_t bufferFrames) {
    singz::AudioHostConfig result;
    result.outputDeviceUid = uid;
    result.requestedBufferFrames = bufferFrames;
    if (input)
        result.inputDeviceUid = uid;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        result.outputChannels.push_back(channel);
        if (input)
            result.inputChannels.push_back(channel);
    }
    return result;
}
inline void requireOK(const singz::AudioHostResult& result) {
    if (!result.ok)
        throw std::runtime_error(result.message);
}
} // namespace loopz::macos::diagnostic
