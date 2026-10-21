#include <loopz/macos/application_routes.hpp>
#include <loopz/macos/application_ownership.hpp>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace loopz {
namespace {
AudioObjectPropertyAddress
address(AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template <typename T>
bool get(AudioObjectID object, AudioObjectPropertySelector selector, T& value) {
    const auto property = address(selector);
    UInt32 bytes = sizeof(value);
    return !AudioObjectGetPropertyData(object, &property, 0, nullptr, &bytes, &value) &&
           bytes == sizeof(value);
}
bool objects(AudioObjectID object, AudioObjectPropertySelector selector,
             AudioObjectPropertyScope scope, std::vector<AudioObjectID>& result) {
    const auto property = address(selector, scope);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(object, &property, 0, nullptr, &bytes) ||
        bytes > 1024 * 1024 || bytes % sizeof(AudioObjectID))
        return false;
    result.resize(bytes / sizeof(AudioObjectID));
    if (!bytes)
        return true;
    const auto capacity = bytes;
    if (AudioObjectGetPropertyData(object, &property, 0, nullptr, &bytes, result.data()) ||
        bytes > capacity || bytes % sizeof(AudioObjectID))
        return false;
    result.resize(bytes / sizeof(AudioObjectID));
    return true;
}
std::string deviceUID(AudioDeviceID device) {
    CFStringRef value = nullptr;
    if (!get(device, kAudioDevicePropertyDeviceUID, value) || !value)
        return {};
    std::string result;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        NSString* uid = (__bridge NSString*)value;
        const auto* text = uid.UTF8String;
        if (text && uid.length && uid.length <= 4096)
            result = text;
    }
    CFRelease(value);
    return result;
}
} // namespace

bool macos::selectApplicationAudio(const std::string& bundle, ApplicationAudioSelection& selected,
                                   std::string& error) {
    @autoreleasepool {
        if (bundle.empty() || bundle.size() > 4096 || bundle.find('\0') != std::string::npos ||
            ![[NSString alloc] initWithBytes:bundle.data()
                                      length:bundle.size()
                                    encoding:NSUTF8StringEncoding]) {
            selected = {};
            error = "Invalid application identity";
            return false;
        }
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
            ApplicationAudioSelection next;
            ApplicationOwnership ownership(bundle);
            std::vector<AudioProcessAttribution> attributed;
            std::vector<AudioObjectID> all;
            if (!objects(kAudioObjectSystemObject, kAudioHardwarePropertyProcessObjectList,
                         kAudioObjectPropertyScopeGlobal, all) ||
                all.size() > 4096) {
                error = "Cannot enumerate Core Audio processes";
                continue;
            }
            bool valid = true;
            for (auto process : all) {
                CFStringRef identity = nullptr;
                if (!get(process, kAudioProcessPropertyBundleID, identity)) {
                    if (identity)
                        CFRelease(identity);
                    error = "Cannot inspect bundle identity of Core Audio process " +
                            std::to_string(process);
                    valid = false;
                    break;
                }
                std::string application;
                if (identity) {
                    const auto bytes =
                        CFGetTypeID(identity) == CFStringGetTypeID()
                            ? [(__bridge NSString*)identity dataUsingEncoding:NSUTF8StringEncoding]
                            : nil;
                    if (!bytes || bytes.length > 4096) {
                        CFRelease(identity);
                        error = "Invalid Core Audio application identity";
                        valid = false;
                        break;
                    }
                    if (bytes.length)
                        application.assign(static_cast<const char*>(bytes.bytes), bytes.length);
                }
                if (identity)
                    CFRelease(identity);
                pid_t pid = 0;
                if (!get(process, kAudioProcessPropertyPID, pid)) {
                    error = "Cannot inspect the identity of an audio process";
                    valid = false;
                    break;
                }
                if (const auto value = ownership.inspect(process, pid, std::move(application)))
                    attributed.push_back(*value);
            }
            std::vector<uint64_t> matches;
            if (valid) {
                try {
                    matches = selectApplicationProcesses(bundle, ownership.roots(), attributed);
                } catch (const std::exception& failure) {
                    error = failure.what();
                    valid = false;
                }
            }
            std::vector<AudioProcessAttribution> included;
            for (const auto matched : matches) {
                const auto process = static_cast<AudioObjectID>(matched);
                std::vector<AudioObjectID> devices;
                UInt32 running = 0;
                if (!objects(process, kAudioProcessPropertyDevices, kAudioObjectPropertyScopeOutput,
                             devices) ||
                    !get(process, kAudioProcessPropertyIsRunningOutput, running)) {
                    error = "Cannot inspect output routes/activity of Core Audio process " +
                            std::to_string(process);
                    valid = false;
                    break;
                }
                next.processes.push_back(process);
                next.outputs.insert(devices.begin(), devices.end());
                next.producing = next.producing || running;
                const auto found =
                    std::find_if(attributed.begin(), attributed.end(), [&](const auto& item) {
                        return item.object == matched;
                    });
                next.identities.push_back({process, found->process});
                included.push_back(*found);
            }
            if (valid && !ownership.coherent(included)) {
                error = "Application process ownership changed during inspection";
                valid = false;
            }
            if (valid) {
                std::sort(next.processes.begin(), next.processes.end());
                selected = std::move(next);
                error.clear();
                return true;
            }
        }
        selected = {};
        return false;
    }
}

bool macos::applicationOutputStreams(AudioDeviceID device,
                                     std::vector<ApplicationOutputStream>& result,
                                     uint32_t& channels, std::string& error) {
    result.clear();
    channels = 0;
    std::vector<AudioObjectID> streams;
    if (!objects(device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, streams) ||
        streams.empty() || streams.size() > kMaximumRoutingSources) {
        error = "Cannot inspect bounded application output streams";
        return false;
    }
    std::array<bool, kMaximumRoutingChannels> occupied{};
    for (auto stream : streams) {
        AudioStreamBasicDescription format{};
        UInt32 first = 0;
        if (!get(stream, kAudioStreamPropertyVirtualFormat, format) ||
            !get(stream, kAudioStreamPropertyStartingChannel, first) || first == 0 ||
            first > kMaximumRoutingChannels || format.mChannelsPerFrame == 0 ||
            format.mChannelsPerFrame > kMaximumRoutingChannels - (first - 1)) {
            error = "Application output stream has an invalid channel range";
            return false;
        }
        --first;
        for (uint32_t channel = first; channel < first + format.mChannelsPerFrame; ++channel) {
            if (occupied[channel]) {
                error = "Application output streams overlap their device channels";
                return false;
            }
            occupied[channel] = true;
        }
        channels = std::max(channels, first + format.mChannelsPerFrame);
        result.push_back({stream, format, first});
    }
    error.clear();
    return true;
}

std::vector<ApplicationCaptureRoute> applicationCaptureRoutes(const std::string& bundle,
                                                              std::string& error) {
    macos::ApplicationAudioSelection selected;
    if (!macos::selectApplicationAudio(bundle, selected, error))
        return {};
    if (selected.processes.empty() || !selected.producing || selected.outputs.empty()) {
        error = "Application has no running output; waiting for audio";
        return {};
    }
    return macos::captureRoutes(selected, error);
}
std::vector<ApplicationCaptureRoute> macos::captureRoutes(const ApplicationAudioSelection& selected,
                                                          std::string& error) {
    error.clear();
    std::vector<ApplicationCaptureRoute> result;
    for (auto device : selected.outputs) {
        const auto uid = deviceUID(device);
        std::vector<macos::ApplicationOutputStream> streams;
        uint32_t channels = 0;
        if (uid.empty() || !macos::applicationOutputStreams(device, streams, channels, error)) {
            if (error.empty())
                error = "Application output device has no stable identity";
            return {};
        }
        if (result.size() + streams.size() > kMaximumRoutingSources) {
            error = "Application has too many simultaneous output streams";
            return {};
        }
        for (uint32_t index = 0; index < streams.size(); ++index) {
            const auto& stream = streams[index];
            result.push_back({uid, index, stream.format.mChannelsPerFrame, stream.firstChannel,
                              channels, stream.format.mSampleRate});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.deviceUID == b.deviceUID ? a.streamIndex < b.streamIndex
                                          : a.deviceUID < b.deviceUID;
    });
    return result;
}
} // namespace loopz
