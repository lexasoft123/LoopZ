#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include <AudioUnit/AudioUnit.h>
#include <loopz/macos/capture.hpp>
#include <zcore/device/audio_host.h>
#include <mach/mach_time.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <set>
#include <chrono>
#include <thread>

namespace loopz {
namespace {
mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t result;
    mach_timebase_info(&result);
    return result;
}();
AudioObjectPropertyAddress
property(AudioObjectPropertySelector selector,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template <class T>
bool get(AudioObjectID id, AudioObjectPropertySelector selector, T& value,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
         OSStatus* failure = nullptr) {
    auto address = property(selector, scope);
    UInt32 bytes = sizeof(T);
    const auto status = AudioObjectGetPropertyData(id, &address, 0, nullptr, &bytes, &value);
    if (failure) {
        *failure = status;
        if (!status && bytes != sizeof(T))
            *failure = kAudioHardwareBadPropertySizeError;
    }
    return status == noErr && bytes == sizeof(T);
}
bool objectsChecked(AudioObjectID id, AudioObjectPropertySelector selector,
                    std::vector<AudioObjectID>& result,
                    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                    OSStatus* failure = nullptr) {
    auto address = property(selector, scope);
    UInt32 bytes = 0;
    auto status = AudioObjectGetPropertyDataSize(id, &address, 0, nullptr, &bytes);
    if (failure)
        *failure = status;
    if (status)
        return false;
    if (bytes > 1024 * 1024 || bytes % sizeof(AudioObjectID)) {
        if (failure)
            *failure = kAudioHardwareBadPropertySizeError;
        return false;
    }
    result.resize(bytes / sizeof(AudioObjectID));
    if (!bytes)
        return true;
    const auto capacity = bytes;
    status = AudioObjectGetPropertyData(id, &address, 0, nullptr, &bytes, result.data());
    if (failure)
        *failure = status;
    if (status)
        return false;
    if (bytes > capacity || bytes % sizeof(AudioObjectID)) {
        if (failure)
            *failure = kAudioHardwareBadPropertySizeError;
        return false;
    }
    result.resize(bytes / sizeof(AudioObjectID));
    return true;
}
std::vector<AudioObjectID>
objects(AudioObjectID id, AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    std::vector<AudioObjectID> result;
    if (!objectsChecked(id, selector, result, scope))
        result.clear();
    return result;
}
bool appSelection(const std::string& bundle, NSMutableArray<NSNumber*>* matches,
                  std::set<AudioDeviceID>& outputs, std::string& error) {
    // Process objects may disappear between enumeration and a property read.
    // Retry whole snapshots on the control thread. Brief backoff lets HAL remove
    // exiting clients; an unknown output never becomes an empty output.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
        [matches removeAllObjects];
        outputs.clear();
        std::vector<AudioObjectID> processes;
        bool valid = true;
        OSStatus status = noErr;
        if (!objectsChecked(kAudioObjectSystemObject, kAudioHardwarePropertyProcessObjectList,
                            processes, kAudioObjectPropertyScopeGlobal, &status)) {
            error = "Cannot enumerate Core Audio processes (prs#, status " +
                    std::to_string(status) + ")";
            continue;
        }
        for (auto process : processes) {
            CFStringRef name = nullptr;
            if (!get(process, kAudioProcessPropertyBundleID, name, kAudioObjectPropertyScopeGlobal,
                     &status)) {
                error = "Cannot inspect bundle identity of Core Audio process " +
                        std::to_string(process) + " (pbid, status " + std::to_string(status) + ")";
                valid = false;
                break;
            }
            const bool match = name && [(__bridge NSString*)name isEqualToString:@(bundle.c_str())];
            if (name)
                CFRelease(name);
            if (!match)
                continue;
            [matches addObject:@(process)];
            std::vector<AudioObjectID> devices;
            if (!objectsChecked(process, kAudioProcessPropertyDevices, devices,
                                kAudioObjectPropertyScopeOutput, &status)) {
                error = "Cannot inspect output routes of Core Audio process " +
                        std::to_string(process) + " (pdv#, status " + std::to_string(status) + ")";
                valid = false;
                break;
            }
            outputs.insert(devices.begin(), devices.end());
        }
        if (valid) {
            error.clear();
            return true;
        }
    }
    return false;
}
bool stereoFloat(const AudioStreamBasicDescription& format) {
    return format.mFormatID == kAudioFormatLinearPCM &&
           (format.mFormatFlags & kAudioFormatFlagIsFloat) &&
           (format.mFormatFlags & kAudioFormatFlagIsBigEndian) == kAudioFormatFlagsNativeEndian &&
           format.mChannelsPerFrame == 2 && format.mBitsPerChannel == 32 &&
           format.mFramesPerPacket == 1 &&
           format.mBytesPerFrame ==
               ((format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 4 : 8) &&
           format.mBytesPerPacket == format.mBytesPerFrame;
}
bool sameFormat(const AudioStreamBasicDescription& a, const AudioStreamBasicDescription& b) {
    return a.mSampleRate == b.mSampleRate && a.mFormatID == b.mFormatID &&
           a.mFormatFlags == b.mFormatFlags && a.mBytesPerPacket == b.mBytesPerPacket &&
           a.mFramesPerPacket == b.mFramesPerPacket && a.mBytesPerFrame == b.mBytesPerFrame &&
           a.mChannelsPerFrame == b.mChannelsPerFrame && a.mBitsPerChannel == b.mBitsPerChannel;
}
AudioDeviceID deviceForUID(const std::string& uid) {
    if (uid.empty()) {
        AudioDeviceID id = 0;
        get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, id);
        return id;
    }
    for (auto id : objects(kAudioObjectSystemObject, kAudioHardwarePropertyDevices)) {
        CFStringRef value = nullptr;
        if (get(id, kAudioDevicePropertyDeviceUID, value) && value) {
            bool match = [(__bridge NSString*)value isEqualToString:@(uid.c_str())];
            CFRelease(value);
            if (match)
                return id;
        }
    }
    return 0;
}
} // namespace
uint64_t ticksToNs(uint64_t ticks) noexcept {
    return ticks / timebase.denom * timebase.numer +
           ticks % timebase.denom * timebase.numer / timebase.denom;
}
uint64_t nsToTicks(uint64_t ns) noexcept {
    return ns / timebase.numer * timebase.denom +
           ns % timebase.numer * timebase.denom / timebase.numer;
}
uint64_t hostNowNs() noexcept {
    return ticksToNs(mach_absolute_time());
}
std::vector<AppInfo> applications() {
    std::vector<AppInfo> result;
    for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
        if (app.bundleIdentifier && app.processIdentifier != getpid())
            result.push_back({app.bundleIdentifier.UTF8String,
                              (app.localizedName ?: app.bundleIdentifier).UTF8String,
                              app.processIdentifier});
    }
    return result;
}
std::string inventoryJSON() {
    singz::AudioHost host;
    auto inventory = host.enumerate();
    NSMutableArray* devices = [NSMutableArray array];
    for (const auto& device : inventory.devices)
        [devices addObject:@{
            @"uid" : @(device.uid.c_str()),
            @"name" : @(device.label.c_str()),
            @"inputChannels" : @(device.inputChannels),
            @"outputChannels" : @(device.outputChannels),
            @"rate" : @(device.nominalSampleRate),
            @"defaultInput" : @(device.defaultInput),
            @"defaultOutput" : @(device.defaultOutput)
        }];
    NSMutableArray* apps = [NSMutableArray array];
    for (const auto& app : applications())
        [apps addObject:@{
            @"bundle" : @(app.bundle.c_str()),
            @"name" : @(app.name.c_str()),
            @"pid" : @(app.pid)
        }];
    NSData* json =
        [NSJSONSerialization dataWithJSONObject:@{@"devices" : devices, @"applications" : apps}
                                        options:NSJSONWritingPrettyPrinted
                                          error:nil];
    return std::string(static_cast<const char*>(json.bytes), json.length);
}
struct Capture::Impl {
    CaptureBuffer application, microphone;
    AudioObjectID tap{0};
    AudioDeviceID aggregate{0};
    AudioDeviceIOProcID tapIO{nullptr};
    AudioUnit micUnit{nullptr};
    AudioDeviceID micDevice{0};
    double appRate{0}, micRate{0};
    AudioDeviceID appDevice{0};
    AudioObjectID appStream{0};
    std::string appBundle;
    AudioStreamBasicDescription appFormat{}, streamFormat{};
    std::array<float, 4096> appL{}, appR{}, micL{}, micR{};
    struct StereoList {
        UInt32 count;
        AudioBuffer buffers[2];
    } micList{2, {{1, 4096 * sizeof(float), micL.data()}, {1, 4096 * sizeof(float), micR.data()}}};
    ~Impl() {
        stop();
    }
    void stop() {
        if (micUnit) {
            AudioOutputUnitStop(micUnit);
            AudioUnitUninitialize(micUnit);
            AudioComponentInstanceDispose(micUnit);
            micUnit = nullptr;
        }
        if (tapIO) {
            AudioDeviceStop(aggregate, tapIO);
            AudioDeviceDestroyIOProcID(aggregate, tapIO);
            tapIO = nullptr;
        }
        if (aggregate) {
            AudioHardwareDestroyAggregateDevice(aggregate);
            aggregate = 0;
        }
        if (tap) {
            AudioHardwareDestroyProcessTap(tap);
            tap = 0;
        }
    }
    static OSStatus tapCallback(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                                const AudioTimeStamp* time, AudioBufferList*, const AudioTimeStamp*,
                                void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (!time || !input || !input->mNumberBuffers || !input->mBuffers[0].mNumberChannels ||
            !(time->mFlags & kAudioTimeStampHostTimeValid))
            return noErr;
        uint32_t frames =
            input->mBuffers[0].mDataByteSize / sizeof(float) / input->mBuffers[0].mNumberChannels;
        if (!frames || frames > self.appL.size())
            return noErr;
        if (input->mNumberBuffers >= 2 && input->mBuffers[0].mNumberChannels == 1 &&
            input->mBuffers[1].mNumberChannels == 1) {
            if (!input->mBuffers[0].mData || !input->mBuffers[1].mData ||
                input->mBuffers[1].mDataByteSize < frames * sizeof(float))
                return noErr;
            self.application.append(
                static_cast<const float*>(input->mBuffers[0].mData),
                static_cast<const float*>(input->mBuffers[1].mData), frames,
                ticksToNs(time->mHostTime), self.appRate,
                (time->mFlags & kAudioTimeStampSampleTimeValid) ? time->mSampleTime : NAN);
        } else if (input->mNumberBuffers == 1 && input->mBuffers[0].mNumberChannels == 2 &&
                   input->mBuffers[0].mData) {
            auto* samples = static_cast<const float*>(input->mBuffers[0].mData);
            for (uint32_t i = 0; i < frames; ++i) {
                self.appL[i] = samples[2 * i];
                self.appR[i] = samples[2 * i + 1];
            }
            self.application.append(
                self.appL.data(), self.appR.data(), frames, ticksToNs(time->mHostTime),
                self.appRate,
                (time->mFlags & kAudioTimeStampSampleTimeValid) ? time->mSampleTime : NAN);
        }
        return noErr;
    }
    static OSStatus micCallback(void* context, AudioUnitRenderActionFlags* flags,
                                const AudioTimeStamp* time, UInt32 bus, UInt32 frames,
                                AudioBufferList*) {
        auto& self = *static_cast<Impl*>(context);
        if (!time || frames > self.micL.size() || !(time->mFlags & kAudioTimeStampHostTimeValid))
            return noErr;
        self.micList.buffers[0].mDataByteSize = self.micList.buffers[1].mDataByteSize =
            frames * sizeof(float);
        auto status = AudioUnitRender(self.micUnit, flags, time, bus, frames,
                                      reinterpret_cast<AudioBufferList*>(&self.micList));
        if (!status)
            self.microphone.append(
                self.micL.data(), self.micR.data(), frames, ticksToNs(time->mHostTime),
                self.micRate,
                (time->mFlags & kAudioTimeStampSampleTimeValid) ? time->mSampleTime : NAN);
        return status;
    }
};
Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() = default;
CaptureBuffer& Capture::app() {
    return impl_->application;
}
CaptureBuffer& Capture::mic() {
    return impl_->microphone;
}
void Capture::stop() {
    impl_->stop();
}
bool Capture::healthy(std::string& error) const {
    if (impl_->application.discontinuities.load() || impl_->microphone.discontinuities.load()) {
        error = "Capture timestamp/sample generation changed; restart this feasibility run";
        return false;
    }
    if (impl_->micUnit) {
        double rate = 0;
        UInt32 alive = 0;
        if (!get(impl_->micDevice, kAudioDevicePropertyDeviceIsAlive, alive) || !alive ||
            !get(impl_->micDevice, kAudioDevicePropertyNominalSampleRate, rate) ||
            rate != impl_->micRate) {
            error = "Microphone route or rate changed; restart this feasibility run";
            return false;
        }
    }
    if (impl_->tap) {
        if (impl_->appDevice) {
            std::set<AudioDeviceID> outputs;
            if (!appSelection(impl_->appBundle, [NSMutableArray array], outputs, error)) {
                error = "Application route inspection failed: " + error;
                return false;
            }
            std::vector<AudioObjectID> streams;
            AudioStreamBasicDescription native{};
            if (!objectsChecked(impl_->appDevice, kAudioDevicePropertyStreams, streams,
                                kAudioObjectPropertyScopeOutput) ||
                streams.size() != 1 || streams[0] != impl_->appStream ||
                !get(impl_->appStream, kAudioStreamPropertyVirtualFormat, native) ||
                !stereoFloat(native) || !sameFormat(native, impl_->streamFormat)) {
                error = "Application output stream format changed; restart this feasibility run";
                return false;
            }
            // An app may stop IO during silence. Nonempty changes require a fresh
            // tap rather than silently capturing its former output device.
            if (!outputs.empty() && (outputs.size() != 1 || *outputs.begin() != impl_->appDevice)) {
                error = "Application output route changed; restart this feasibility run";
                return false;
            }
        }
        AudioStreamBasicDescription format{};
        if (!get(impl_->tap, kAudioTapPropertyFormat, format) || !stereoFloat(format) ||
            !sameFormat(format, impl_->appFormat)) {
            error = "Application capture format changed; restart this feasibility run";
            return false;
        }
    }
    return true;
}
bool Capture::startApplication(const std::string& bundle, bool mute, std::string& error,
                               const ApplicationCaptureConfig& config) {
    if ((config.requestedBufferFrames != 64 && config.requestedBufferFrames != 128) ||
        (config.requireDeviceStream && !config.preferDeviceStream)) {
        error = "Invalid application capture configuration";
        return false;
    }
    NSMutableArray<NSNumber*>* processes = [NSMutableArray array];
    std::set<AudioDeviceID> outputs;
    if (!appSelection(bundle, processes, outputs, error)) {
        return false;
    }
    if (processes.count == 0) {
        error = "No Core Audio process for " + bundle + "; launch the app and start audio first";
        return false;
    }
    CATapDescription* description = nil;
    NSString* selectedUID = nil;
    if (config.preferDeviceStream) {
        if (outputs.size() == 1) {
            const auto candidate = *outputs.begin();
            const auto streams =
                objects(candidate, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
            AudioStreamBasicDescription native{};
            CFStringRef uid = nullptr;
            if (streams.size() == 1 && get(streams[0], kAudioStreamPropertyVirtualFormat, native) &&
                stereoFloat(native) && get(candidate, kAudioDevicePropertyDeviceUID, uid) && uid) {
                selectedUID = CFBridgingRelease(uid);
                description = [[CATapDescription alloc] initWithProcesses:processes
                                                             andDeviceUID:selectedUID
                                                               withStream:0];
                if (description) {
                    impl_->appDevice = candidate;
                    impl_->appStream = streams[0];
                    impl_->streamFormat = native;
                    impl_->appBundle = bundle;
                } else
                    selectedUID = nil;
            } else if (uid)
                CFRelease(uid);
        }
    }
    if (!description && config.requireDeviceStream) {
        error = "Small-block capture requires one eligible stereo application output stream";
        return false;
    }
    if (!description)
        description = [[CATapDescription alloc] initStereoMixdownOfProcesses:processes];
    description.name = @"LoopZ application source";
    description.privateTap = YES;
    description.muteBehavior = mute ? CATapMutedWhenTapped : CATapUnmuted;
    if (@available(macOS 26.0, *)) {
        description.bundleIDs = @[ @(bundle.c_str()) ];
        description.processRestoreEnabled = YES;
    }
    OSStatus status = AudioHardwareCreateProcessTap(description, &impl_->tap);
    AudioStreamBasicDescription format{};
    if (!status && !get(impl_->tap, kAudioTapPropertyFormat, format))
        status = kAudioHardwareUnspecifiedError;
    if (!status && !stereoFloat(format))
        status = kAudioHardwareUnsupportedOperationError;
    if (!status) {
        impl_->appRate = format.mSampleRate;
        impl_->appFormat = format;
        NSDictionary* aggregate = @{
            @kAudioAggregateDeviceNameKey : @"LoopZ private capture",
            @kAudioAggregateDeviceUIDKey : NSUUID.UUID.UUIDString,
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceTapAutoStartKey : @NO,
            @kAudioAggregateDeviceTapListKey : @[ @{
                @kAudioSubTapUIDKey : description.UUID.UUIDString,
                @kAudioSubTapDriftCompensationKey : @NO
            } ]
        };
        status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate,
                                                    &impl_->aggregate);
    }
    if (!status) {
        // This aggregate is private to LoopZ. Do not alter a physical device's
        // shared buffer size just to reduce application-capture latency.
        auto address = property(kAudioDevicePropertyBufferFrameSize);
        UInt32 frames = config.requestedBufferFrames;
        status = AudioObjectSetPropertyData(impl_->aggregate, &address, 0, nullptr, sizeof(frames),
                                            &frames);
        if (!status &&
            (!get(impl_->aggregate, kAudioDevicePropertyBufferFrameSize, frames) || frames > 512))
            status = kAudioHardwareUnsupportedOperationError;
        if (!status) {
            UInt32 latency = 0, safety = 0;
            const bool latencyKnown = get(impl_->aggregate, kAudioDevicePropertyLatency, latency,
                                          kAudioObjectPropertyScopeInput);
            const bool safetyKnown = get(impl_->aggregate, kAudioDevicePropertySafetyOffset, safety,
                                         kAudioObjectPropertyScopeInput);
            NSDictionary* diagnostics = @{
                @"event" : @"capture-config",
                @"appBufferFrames" : @(frames),
                @"mode" : selectedUID ? @"device-stream" : @"mixdown",
                @"deviceUID" : selectedUID ?: @"",
                @"aggregateLatencyFrames" : latencyKnown ? @(latency) : NSNull.null,
                @"aggregateSafetyFrames" : safetyKnown ? @(safety) : NSNull.null,
                @"aggregateLatencyKnown" : @(latencyKnown),
                @"aggregateSafetyKnown" : @(safetyKnown)
            };
            NSData* json = [NSJSONSerialization dataWithJSONObject:diagnostics options:0 error:nil];
            if (!json) {
                error = "Capture diagnostics serialization failed";
                impl_->stop();
                return false;
            }
            std::fwrite(json.bytes, 1, json.length, stdout);
            std::puts("");
            std::fflush(stdout);
        }
    }
    if (!status)
        status = AudioDeviceCreateIOProcID(impl_->aggregate, Impl::tapCallback, impl_.get(),
                                           &impl_->tapIO);
    if (!status)
        status = AudioDeviceStart(impl_->aggregate, impl_->tapIO);
    if (status) {
        error = "Application capture failed with Core Audio status " + std::to_string(status);
        impl_->stop();
        return false;
    }
    return true;
}
bool Capture::startMicrophone(const std::string& uid, std::string& error) {
    AudioDeviceID device = deviceForUID(uid);
    if (!device) {
        error = "Microphone device not found";
        return false;
    }
    // Explicitly reject our own virtual input as a capture source.
    CFStringRef actualUID = nullptr;
    const bool selfCapture = get(device, kAudioDevicePropertyDeviceUID, actualUID) && actualUID &&
                             [(__bridge NSString*)actualUID isEqualToString:@(kDeviceUID)];
    if (actualUID)
        CFRelease(actualUID);
    if (selfCapture) {
        error = "LoopZ cannot capture its own virtual output";
        return false;
    }
    impl_->micDevice = device;
    AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                          kAudioUnitManufacturer_Apple, 0, 0};
    auto component = AudioComponentFindNext(nullptr, &description);
    OSStatus status = component ? AudioComponentInstanceNew(component, &impl_->micUnit)
                                : kAudioHardwareUnspecifiedError;
    UInt32 enabled = 1, disabled = 0;
    if (!status)
        status = AudioUnitSetProperty(impl_->micUnit, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Input, 1, &enabled, sizeof(enabled));
    if (!status)
        status = AudioUnitSetProperty(impl_->micUnit, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Output, 0, &disabled, sizeof(disabled));
    if (!status)
        status = AudioUnitSetProperty(impl_->micUnit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, 0, &device, sizeof(device));
    AudioStreamBasicDescription native{};
    UInt32 bytes = sizeof(native);
    if (!status)
        status = AudioUnitGetProperty(impl_->micUnit, kAudioUnitProperty_StreamFormat,
                                      kAudioUnitScope_Input, 1, &native, &bytes);
    impl_->micRate = native.mSampleRate;
    AudioStreamBasicDescription format{native.mSampleRate,
                                       kAudioFormatLinearPCM,
                                       static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) |
                                           static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved),
                                       4,
                                       1,
                                       4,
                                       2,
                                       32,
                                       0};
    if (!status)
        status = AudioUnitSetProperty(impl_->micUnit, kAudioUnitProperty_StreamFormat,
                                      kAudioUnitScope_Output, 1, &format, sizeof(format));
    // Duplicate a mono microphone into stereo; select the first stereo pair of
    // a multichannel interface. More channel selectors belong to the next phase.
    SInt32 channelMap[2]{0, native.mChannelsPerFrame > 1 ? 1 : 0};
    if (!status)
        status = AudioUnitSetProperty(impl_->micUnit, kAudioOutputUnitProperty_ChannelMap,
                                      kAudioUnitScope_Output, 1, channelMap, sizeof(channelMap));
    UInt32 maximum = 4096;
    if (!status)
        status = AudioUnitSetProperty(impl_->micUnit, kAudioUnitProperty_MaximumFramesPerSlice,
                                      kAudioUnitScope_Global, 0, &maximum, sizeof(maximum));
    AURenderCallbackStruct callback{Impl::micCallback, impl_.get()};
    if (!status)
        status = AudioUnitSetProperty(impl_->micUnit, kAudioOutputUnitProperty_SetInputCallback,
                                      kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    if (!status)
        status = AudioUnitInitialize(impl_->micUnit);
    if (!status)
        status = AudioOutputUnitStart(impl_->micUnit);
    if (status) {
        error = "Microphone capture failed with Core Audio status " + std::to_string(status);
        impl_->stop();
        return false;
    }
    return true;
}
} // namespace loopz
