#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include <AudioUnit/AudioUnit.h>
#include <loopz/macos/capture.hpp>
#include <loopz/audio_delivery_timing.hpp>
#include <loopz/macos/capture_layout.hpp>
#include <loopz/macos/application_routes.hpp>
#include <loopz/routing.hpp>
#include <zcore/device/audio_host.h>
#include <zcore/device/audio_input_callback_gate.h>
#include <mach/mach_time.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <set>
#include <chrono>
#include <thread>
#include <limits>

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
                  std::set<AudioDeviceID>& outputs,
                  std::vector<macos::ApplicationAudioProcessIdentity>& identities,
                  std::string& error) {
    macos::ApplicationAudioSelection selection;
    if (!macos::selectApplicationAudio(bundle, selection, error))
        return false;
    [matches removeAllObjects];
    for (auto process : selection.processes)
        [matches addObject:@(process)];
    outputs = std::move(selection.outputs);
    identities = std::move(selection.identities);
    return true;
}
bool boundedFloat(const AudioStreamBasicDescription& format) {
    return format.mFormatID == kAudioFormatLinearPCM &&
           (format.mFormatFlags & kAudioFormatFlagIsFloat) &&
           (format.mFormatFlags & kAudioFormatFlagIsBigEndian) == kAudioFormatFlagsNativeEndian &&
           format.mChannelsPerFrame > 0 && format.mChannelsPerFrame <= kMaximumRoutingChannels &&
           format.mBitsPerChannel == 32 && format.mFramesPerPacket == 1 &&
           format.mBytesPerFrame == ((format.mFormatFlags & kAudioFormatFlagIsNonInterleaved)
                                         ? 4
                                         : sizeof(float) * format.mChannelsPerFrame) &&
           format.mBytesPerPacket == format.mBytesPerFrame && std::isfinite(format.mSampleRate) &&
           format.mSampleRate >= 8000 && format.mSampleRate <= 192000;
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
            @"bufferFrames" : @(device.bufferFrames.preferredFrames),
            @"transport" : @(static_cast<uint32_t>(device.transport)),
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
    singz::AudioInputCallbackGate appCallbacks, micCallbacks;
    AudioObjectID tap{0};
    AudioObjectID muteTap{0};
    AudioDeviceID muteAggregate{0};
    AudioDeviceIOProcID muteIO{nullptr};
    bool muteRunning{false}; // Control owner only; the mute reader retains no audio.
    AudioDeviceID aggregate{0};
    AudioDeviceIOProcID tapIO{nullptr};
    AudioUnit micUnit{nullptr};
    AudioDeviceID micDevice{0};
    double appRate{0}, micRate{0};
    AudioDeviceID appDevice{0};
    AudioObjectID appStream{0};
    uint32_t appStreamIndex{0}, appFirstChannel{0}, appDeviceChannels{0};
    bool scopedStream{false};
    std::string appBundle, microphoneUID;
    std::vector<AudioObjectID> appProcesses;
    std::vector<macos::ApplicationAudioProcessIdentity> appIdentities;
    uint64_t appStartedNs{0};
    AudioStreamBasicDescription appFormat{}, streamFormat{};
    std::array<float, 4096> appL{}, appR{}, micL{}, micR{};
    std::unique_ptr<RoutingGraph> appMixer;
    macos::CaptureChannelPlan appChannels;
    std::array<std::array<float, kBlock>, kMaximumRoutingChannels> appPlanes{};
    uint64_t appMixPosition{0};
    AudioDeliveryTiming appDelivery;
    bool deliveryEnabled{false}; // Immutable while callback admission is open.
    uint32_t deliveryRequestedFrames{0};
    std::string deliveryDeviceUID;
    uint64_t deliveryHistoryRejected{0};
    std::atomic<uint64_t> appFirstReadyNs{0}, maximumAppReadyEndAgeNs{0};
    std::atomic<uint32_t> maximumAppCallbackFrames{0};
    void applicationPublished(const AudioTimeStamp& time, uint32_t frames,
                              uint64_t enteredNs) noexcept {
        const auto ready = hostNowNs();
        auto first = appFirstReadyNs.load();
        if (!first) {
            first = ready;
            appFirstReadyNs.store(first);
        }
        if (frames > maximumAppCallbackFrames.load())
            maximumAppCallbackFrames.store(frames);
        const auto host = ticksToNs(time.mHostTime);
        const auto duration = frameNs(frames, appRate);
        if (ready >= first && ready - first >= 1000000000ULL && host <= UINT64_MAX - duration &&
            ready > host + duration) {
            const auto late = ready - host - duration;
            if (late > maximumAppReadyEndAgeNs.load())
                maximumAppReadyEndAgeNs.store(late);
        }
        if (deliveryEnabled)
            appDelivery.observe(host, frames, appRate, enteredNs, ready, time.mSampleTime,
                                time.mFlags & kAudioTimeStampSampleTimeValid);
    }
    struct StereoList {
        UInt32 count;
        AudioBuffer buffers[2];
    } micList{2, {{1, 4096 * sizeof(float), micL.data()}, {1, 4096 * sizeof(float), micR.data()}}};
    ~Impl() {
        stop();
    }
    void stopMicrophone() {
        micCallbacks.beginClose();
        if (micUnit) {
            AudioOutputUnitStop(micUnit);
            while (micCallbacks.inFlight())
                std::this_thread::yield();
            AudioUnitUninitialize(micUnit);
            AudioComponentInstanceDispose(micUnit);
            micUnit = nullptr;
        }
        microphone.stop();
    }
    void stopApplication() {
        // Original playback is independent of the capture aggregate's IO lifetime.
        if (muteIO) {
            AudioDeviceStop(muteAggregate, muteIO);
            AudioDeviceDestroyIOProcID(muteAggregate, muteIO);
            muteIO = nullptr;
        }
        if (muteAggregate) {
            AudioHardwareDestroyAggregateDevice(muteAggregate);
            muteAggregate = 0;
        }
        if (muteTap) {
            AudioHardwareDestroyProcessTap(muteTap);
            muteTap = 0;
        }
        muteRunning = false;
        appCallbacks.beginClose();
        if (tapIO) {
            AudioDeviceStop(aggregate, tapIO);
            while (appCallbacks.inFlight())
                std::this_thread::yield();
            AudioDeviceDestroyIOProcID(aggregate, tapIO);
            tapIO = nullptr;
        }
        // The callback has joined: this summary needs neither atomic counters
        // nor allocation/logging on the audio thread. Repeated stop is silent.
        const auto delivery = appDelivery.snapshot();
        if (deliveryEnabled && (delivery.callbacks || delivery.warmupCallbacks ||
                                delivery.rejected || deliveryHistoryRejected)) {
            NSDictionary* report = @{
                @"event" : @"capture-delivery",
                @"bundle" : @(appBundle.c_str()),
                @"deviceUID" : @(deliveryDeviceUID.c_str()),
                @"streamIndex" : @(appStreamIndex),
                @"sampleRate" : @(appRate),
                @"requestedBufferFrames" : @(deliveryRequestedFrames),
                @"warmupNs" : @1000000000ULL,
                @"callbacks" : @(delivery.callbacks),
                @"frames" : @(delivery.frames),
                @"warmupCallbacks" : @(delivery.warmupCallbacks),
                @"rejected" : @(delivery.rejected),
                @"historyRejectedCallbacks" : @(deliveryHistoryRejected),
                @"callbackFrames" : @[ @(delivery.minimumFrames), @(delivery.maximumFrames) ],
                @"entryFirstSampleAgeNs" :
                    @[ @(delivery.minimumEntryAgeNs), @(delivery.maximumEntryAgeNs) ],
                @"readyExclusiveEndAgeNs" :
                    @[ @(delivery.minimumReadyEndAgeNs), @(delivery.maximumReadyEndAgeNs) ],
                @"callbackGapNs" : @[ @(delivery.minimumGapNs), @(delivery.maximumGapNs) ],
                @"maximumProcessingNs" : @(delivery.maximumProcessingNs),
                @"maximumAnchorErrorNs" : @(delivery.maximumAnchorErrorNs),
                @"unsupportedAnchorIntervals" : @(delivery.unsupportedAnchorIntervals),
                @"sourceFrameDiscontinuities" : @(delivery.sourceFrameDiscontinuities),
                @"timingMaximumCallbackFrames" : @(maximumAppCallbackFrames.load()),
                @"timingMaximumReadyEndAgeNs" : @(maximumAppReadyEndAgeNs.load()),
                @"audioSamplesRecorded" : @NO
            };
            const auto bytes = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
            if (bytes) {
                std::fwrite(bytes.bytes, 1, bytes.length, stdout);
                std::puts("");
                std::fflush(stdout);
            }
        }
        appDelivery.reset();
        deliveryHistoryRejected = 0;
        deliveryEnabled = false;
        deliveryDeviceUID.clear();
        appFirstReadyNs.store(0);
        maximumAppReadyEndAgeNs.store(0);
        maximumAppCallbackFrames.store(0);
        if (aggregate) {
            AudioHardwareDestroyAggregateDevice(aggregate);
            aggregate = 0;
        }
        if (tap) {
            AudioHardwareDestroyProcessTap(tap);
            tap = 0;
        }
        application.stop();
        appMixer.reset();
        appMixPosition = 0;
    }
    void stop() {
        stopMicrophone();
        stopApplication();
    }
    static OSStatus tapCallback(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                                const AudioTimeStamp* time, AudioBufferList*, const AudioTimeStamp*,
                                void* context) {
        auto& self = *static_cast<Impl*>(context);
        singz::AudioInputCallbackScope callback(self.appCallbacks);
        if (!callback)
            return noErr;
        const auto enteredNs = self.deliveryEnabled ? hostNowNs() : 0;
        if (!time || !input || !input->mNumberBuffers || !input->mBuffers[0].mNumberChannels ||
            !(time->mFlags & kAudioTimeStampHostTimeValid))
            return noErr;
        uint32_t frames =
            input->mBuffers[0].mDataByteSize / sizeof(float) / input->mBuffers[0].mNumberChannels;
        if (!frames || frames > self.appL.size())
            return noErr;
        const auto channels = self.appFormat.mChannelsPerFrame;
        const bool planar = self.appFormat.mFormatFlags & kAudioFormatFlagIsNonInterleaved;
        if (input->mNumberBuffers != (planar ? channels : 1))
            return noErr;
        for (uint32_t buffer = 0; buffer < input->mNumberBuffers; ++buffer) {
            const auto& value = input->mBuffers[buffer];
            const auto bufferChannels = planar ? 1 : channels;
            if (!value.mData || value.mNumberChannels != bufferChannels ||
                value.mDataByteSize != frames * bufferChannels * sizeof(float))
                return noErr;
        }
        if (planar && channels == 2 && !self.appMixer) {
            const auto published = self.application.append(
                static_cast<const float*>(input->mBuffers[0].mData),
                static_cast<const float*>(input->mBuffers[1].mData), frames,
                ticksToNs(time->mHostTime), self.appRate,
                (time->mFlags & kAudioTimeStampSampleTimeValid) ? time->mSampleTime : NAN);
            if (published)
                self.applicationPublished(*time, frames, enteredNs);
            else if (self.deliveryEnabled)
                ++self.deliveryHistoryRejected;
            return noErr;
        }
        for (uint32_t offset = 0; offset < frames; offset += kBlock) {
            const auto count = std::min(kBlock, frames - offset);
            std::array<const float*, kMaximumRoutingChannels> planes{};
            for (uint32_t channel = 0; channel < channels; ++channel) {
                if (planar) {
                    planes[channel] =
                        static_cast<const float*>(input->mBuffers[channel].mData) + offset;
                } else {
                    const auto* samples = static_cast<const float*>(input->mBuffers[0].mData);
                    for (uint32_t frame = 0; frame < count; ++frame)
                        self.appPlanes[channel][frame] =
                            samples[(offset + frame) * channels + channel];
                    planes[channel] = self.appPlanes[channel].data();
                }
            }
            if (self.appMixer) {
                const std::array<RoutingInput, 1> source{{{{planes.data(), channels}, count}}};
                float* output[]{self.appL.data() + offset, self.appR.data() + offset};
                if (!self.appMixer->render(source, {output, count}, count, self.appMixPosition))
                    return noErr;
            } else {
                std::copy_n(planes[0], count, self.appL.data() + offset);
                std::copy_n(planes[1], count, self.appR.data() + offset);
            }
            self.appMixPosition += count;
        }
        const auto published = self.application.append(
            self.appL.data(), self.appR.data(), frames, ticksToNs(time->mHostTime), self.appRate,
            (time->mFlags & kAudioTimeStampSampleTimeValid) ? time->mSampleTime : NAN);
        if (published)
            self.applicationPublished(*time, frames, enteredNs);
        else if (self.deliveryEnabled)
            ++self.deliveryHistoryRejected;
        return noErr;
    }
    static OSStatus micCallback(void* context, AudioUnitRenderActionFlags* flags,
                                const AudioTimeStamp* time, UInt32 bus, UInt32 frames,
                                AudioBufferList*) {
        auto& self = *static_cast<Impl*>(context);
        singz::AudioInputCallbackScope callback(self.micCallbacks);
        if (!callback)
            return noErr;
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
std::optional<InputTimingFacts> Capture::applicationInputTiming(bool requireDelivery) const {
    const auto first = impl_->appFirstReadyNs.load();
    const auto now = hostNowNs();
    if (!impl_->tapIO || !impl_->aggregate ||
        (requireDelivery && (!first || now < first || now - first < 1000000000ULL ||
                             !impl_->application.clock().anchorNs)))
        return {};
    UInt32 alive = 0, period = 0, latency = 0, safety = 0;
    Float64 rate = 0;
    if (!get(impl_->aggregate, kAudioDevicePropertyDeviceIsAlive, alive) || !alive ||
        !get(impl_->aggregate, kAudioDevicePropertyBufferFrameSize, period) || !period ||
        !get(impl_->aggregate, kAudioDevicePropertyNominalSampleRate, rate) ||
        !std::isfinite(rate) || rate < 8000 || rate > 192000 || rate != impl_->appRate ||
        !get(impl_->aggregate, kAudioDevicePropertyLatency, latency,
             kAudioDevicePropertyScopeInput) ||
        !get(impl_->aggregate, kAudioDevicePropertySafetyOffset, safety,
             kAudioDevicePropertyScopeInput))
        return {};
    const auto address = property(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(impl_->aggregate, &address, 0, nullptr, &bytes) || !bytes ||
        bytes % sizeof(AudioStreamID) || bytes > 64 * sizeof(AudioStreamID))
        return {};
    std::array<AudioStreamID, 64> streams{};
    const auto expected = bytes;
    if (AudioObjectGetPropertyData(impl_->aggregate, &address, 0, nullptr, &bytes,
                                   streams.data()) ||
        bytes != expected)
        return {};
    UInt32 streamLatency = 0;
    for (uint32_t i = 0; i < bytes / sizeof(AudioStreamID); ++i) {
        UInt32 value = 0;
        if (!get(streams[i], kAudioStreamPropertyLatency, value))
            return {};
        streamLatency = std::max(streamLatency, value);
    }
    const auto observed =
        std::ceil(static_cast<long double>(impl_->maximumAppReadyEndAgeNs.load()) * rate / 1e9L);
    const auto maximum = uint64_t(std::numeric_limits<uint32_t>::max());
    const auto extra = observed >= maximum ? maximum : static_cast<uint64_t>(observed);
    const auto total = uint64_t(latency) + safety + streamLatency + extra;
    UInt32 currentPeriod = 0;
    Float64 currentRate = 0;
    if (!get(impl_->aggregate, kAudioDevicePropertyBufferFrameSize, currentPeriod) ||
        currentPeriod != period ||
        !get(impl_->aggregate, kAudioDevicePropertyNominalSampleRate, currentRate) ||
        currentRate != rate)
        return {};
    return InputTimingFacts{rate, std::max(period, impl_->maximumAppCallbackFrames.load()),
                            static_cast<uint32_t>(std::min(total, maximum))};
}
void Capture::stop() {
    impl_->stop();
}
bool Capture::healthy(std::string& error) const {
    macos::ApplicationAudioSelection selected;
    if (impl_->tap && !macos::selectApplicationAudio(impl_->appBundle, selected, error)) {
        error = "Application route inspection failed: " + error;
        return false;
    }
    return healthy(selected, error);
}
bool Capture::healthy(const macos::ApplicationAudioSelection& selected, std::string& error) const {
    if (impl_->micUnit) {
        double rate = 0;
        UInt32 alive = 0;
        if (deviceForUID(impl_->microphoneUID) != impl_->micDevice ||
            !get(impl_->micDevice, kAudioDevicePropertyDeviceIsAlive, alive) || !alive ||
            !get(impl_->micDevice, kAudioDevicePropertyNominalSampleRate, rate) ||
            rate != impl_->micRate) {
            error = "Microphone route or rate changed";
            return false;
        }
    }
    if (impl_->tap) {
        const auto& current = selected.processes;
        const auto& outputs = selected.outputs;
        if (current.empty() || current != impl_->appProcesses ||
            selected.identities != impl_->appIdentities) {
            error = "Application exited or its audio processes changed";
            return false;
        }
        const bool producing = selected.producing;
        // Stopped apps can legitimately have no callbacks. A process that is
        // actively producing output must not leave a dead tap marked healthy.
        const auto now = hostNowNs();
        const auto last = std::max(impl_->application.clock().anchorNs, impl_->appStartedNs);
        if (producing && now >= last && now - last > 2000000000ULL) {
            error = "Application capture callbacks stalled";
            return false;
        }
        if (impl_->appDevice) {
            std::vector<macos::ApplicationOutputStream> streams;
            uint32_t channels = 0;
            if (!macos::applicationOutputStreams(impl_->appDevice, streams, channels, error) ||
                impl_->appStreamIndex >= streams.size() ||
                (!impl_->scopedStream && streams.size() != 1) ||
                streams[impl_->appStreamIndex].object != impl_->appStream ||
                streams[impl_->appStreamIndex].firstChannel != impl_->appFirstChannel ||
                channels != impl_->appDeviceChannels ||
                !sameFormat(streams[impl_->appStreamIndex].format, impl_->streamFormat)) {
                error = "Application output stream format changed";
                return false;
            }
            const auto plan = macos::deviceStreamCapturePlan(
                impl_->appDevice, impl_->appFirstChannel, impl_->streamFormat.mChannelsPerFrame,
                impl_->appDeviceChannels, error);
            if (!plan || !macos::sameCapturePlan(*plan, impl_->appChannels)) {
                error = error.empty() ? "Application output channel mapping changed"
                                      : "Application output channel mapping changed: " + error;
                return false;
            }
            // An app may stop IO during silence. Nonempty changes require a fresh
            // tap rather than silently capturing its former output device.
            if (!outputs.empty() && (!outputs.contains(impl_->appDevice) ||
                                     (!impl_->scopedStream && outputs.size() != 1))) {
                error = "Application output route changed";
                return false;
            }
        }
        AudioStreamBasicDescription format{};
        if (!get(impl_->tap, kAudioTapPropertyFormat, format) || !boundedFloat(format) ||
            !sameFormat(format, impl_->appFormat)) {
            error = "Application capture format changed";
            return false;
        }
    }
    return true;
}
bool Capture::startApplication(const std::string& bundle, bool mute, std::string& error,
                               const ApplicationCaptureConfig& config) {
    impl_->stopApplication();
    impl_->appDevice = 0;
    impl_->appStream = 0;
    impl_->scopedStream = !config.deviceUID.empty();
    impl_->appStreamIndex = config.streamIndex;
    impl_->appBundle = bundle;
    impl_->appProcesses.clear();
    impl_->appIdentities.clear();
    if ((config.requestedBufferFrames != 32 && config.requestedBufferFrames != 64 &&
         config.requestedBufferFrames != 128) ||
        (config.requireDeviceStream && !config.preferDeviceStream) ||
        (impl_->scopedStream && (!config.requireDeviceStream || !config.preferDeviceStream)) ||
        config.streamIndex >= kMaximumRoutingSources ||
        (!impl_->scopedStream && config.streamIndex)) {
        error = "Invalid application capture configuration";
        return false;
    }
    NSMutableArray<NSNumber*>* processes = [NSMutableArray array];
    std::set<AudioDeviceID> outputs;
    if (!appSelection(bundle, processes, outputs, impl_->appIdentities, error)) {
        return false;
    }
    if (processes.count == 0) {
        error = "No Core Audio process for " + bundle + "; launch the app and start audio first";
        return false;
    }
    bool producing = false;
    for (NSNumber* process in processes) {
        UInt32 running = 0;
        if (!get(process.unsignedIntValue, kAudioProcessPropertyIsRunningOutput, running)) {
            error = "Application output activity is not ready";
            return false;
        }
        producing = producing || running;
    }
    // A newly launched app may already have a process object but no running
    // output clock. Starting a mixdown tap then can block aggregate startup
    // until the acquisition expires. Retry discovery before creating any tap.
    if (!producing || outputs.empty()) {
        error = "Application has not started an output stream; waiting for audio";
        return false;
    }
    for (NSNumber* process in processes)
        impl_->appProcesses.push_back(process.unsignedIntValue);
    std::sort(impl_->appProcesses.begin(), impl_->appProcesses.end());
    CATapDescription* description = nil;
    NSString* selectedUID = nil;
    if (config.preferDeviceStream) {
        const auto candidate = impl_->scopedStream   ? deviceForUID(config.deviceUID)
                               : outputs.size() == 1 ? *outputs.begin()
                                                     : 0;
        if (candidate && outputs.contains(candidate)) {
            std::vector<macos::ApplicationOutputStream> streams;
            uint32_t deviceChannels = 0;
            CFStringRef uid = nullptr;
            std::optional<macos::CaptureChannelPlan> plan;
            const auto index = config.streamIndex;
            if (macos::applicationOutputStreams(candidate, streams, deviceChannels, error) &&
                index < streams.size() && (impl_->scopedStream || streams.size() == 1) &&
                boundedFloat(streams[index].format))
                plan = macos::deviceStreamCapturePlan(candidate, streams[index].firstChannel,
                                                      streams[index].format.mChannelsPerFrame,
                                                      deviceChannels, error);
            if (plan && get(candidate, kAudioDevicePropertyDeviceUID, uid) && uid) {
                selectedUID = CFBridgingRelease(uid);
                description = [[CATapDescription alloc] initWithProcesses:processes
                                                             andDeviceUID:selectedUID
                                                               withStream:index];
                if (description) {
                    impl_->appDevice = candidate;
                    impl_->appStream = streams[index].object;
                    impl_->streamFormat = streams[index].format;
                    impl_->appFirstChannel = streams[index].firstChannel;
                    impl_->appDeviceChannels = deviceChannels;
                    impl_->appChannels = std::move(*plan);
                    impl_->appBundle = bundle;
                } else
                    selectedUID = nil;
            } else if (uid)
                CFRelease(uid);
        }
    }
    if (!description && config.requireDeviceStream) {
        error = "Small-block capture requires one eligible application output stream: " + error;
        return false;
    }
    if (!description) {
        description = [[CATapDescription alloc] initStereoMixdownOfProcesses:processes];
        impl_->appChannels = {discreteStereoConnections(2), true};
    }
    error.clear();
    description.name = @"LoopZ application source";
    description.privateTap = YES;
    // The recording tap's description stays unchanged for its entire IO lifetime.
    // Muting its live description can cause the OS to interrupt capture delivery.
    description.muteBehavior = CATapUnmuted;
    if (@available(macOS 26.0, *)) {
        description.bundleIDs = @[ @(bundle.c_str()) ];
        description.processRestoreEnabled = YES;
    }
    OSStatus status = AudioHardwareCreateProcessTap(description, &impl_->tap);
    AudioStreamBasicDescription format{};
    if (!status && !get(impl_->tap, kAudioTapPropertyFormat, format))
        status = kAudioHardwareUnspecifiedError;
    if (!status && (!boundedFloat(format) || (!selectedUID && format.mChannelsPerFrame != 2)))
        status = kAudioHardwareUnsupportedOperationError;
    if (!status && selectedUID &&
        (format.mChannelsPerFrame != impl_->streamFormat.mChannelsPerFrame ||
         format.mSampleRate != impl_->streamFormat.mSampleRate))
        status = kAudioHardwareUnsupportedOperationError;
    if (!status) {
        impl_->appRate = format.mSampleRate;
        impl_->appFormat = format;
        if (format.mChannelsPerFrame != 2 || !macos::identityStereoPlan(impl_->appChannels)) {
            RoutingConfiguration mix;
            mix.safetyLimiter = false;
            mix.sources = {{"application-stream", format.mChannelsPerFrame, 1, true, false,
                            impl_->appChannels.connections}};
            try {
                impl_->appMixer = std::make_unique<RoutingGraph>(format.mSampleRate, mix);
            } catch (const std::exception& failure) {
                error = "Cannot prepare application stereo fold: " + std::string(failure.what());
                impl_->stopApplication();
                return false;
            }
        }
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
                @"tapChannels" : @(format.mChannelsPerFrame),
                @"tapRate" : @(format.mSampleRate),
                @"streamIndex" : @(impl_->appStreamIndex),
                @"streamFirstChannel" : @(impl_->appFirstChannel),
                @"channelMix" : selectedUID
                    ? (impl_->appChannels.discrete ? @"discrete-pairs" : @"speaker-layout")
                    : @"system-mixdown",
                @"aggregateLatencyFrames" : latencyKnown ? @(latency) : NSNull.null,
                @"aggregateSafetyFrames" : safetyKnown ? @(safety) : NSNull.null,
                @"aggregateLatencyKnown" : @(latencyKnown),
                @"aggregateSafetyKnown" : @(safetyKnown)
            };
            NSData* json = [NSJSONSerialization dataWithJSONObject:diagnostics options:0 error:nil];
            if (!json) {
                error = "Capture diagnostics serialization failed";
                impl_->stopApplication();
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
    if (!status) {
        impl_->deliveryEnabled = config.deliveryDiagnostics;
        impl_->deliveryRequestedFrames = config.requestedBufferFrames;
        impl_->deliveryDeviceUID = selectedUID ? selectedUID.UTF8String : "";
        impl_->appCallbacks.open();
        status = AudioDeviceStart(impl_->aggregate, impl_->tapIO);
    }
    if (status) {
        error = "Application capture failed with Core Audio status " + std::to_string(status);
        impl_->stopApplication();
        return false;
    }
    CATapDescription* muteDescription =
        selectedUID ? [[CATapDescription alloc] initWithProcesses:processes
                                                     andDeviceUID:selectedUID
                                                       withStream:impl_->appStreamIndex]
                    : [[CATapDescription alloc] initStereoMixdownOfProcesses:processes];
    muteDescription.name = @"LoopZ original playback policy";
    muteDescription.privateTap = YES;
    muteDescription.muteBehavior = CATapMutedWhenTapped;
    if (@available(macOS 26.0, *)) {
        muteDescription.bundleIDs = @[ @(bundle.c_str()) ];
        muteDescription.processRestoreEnabled = YES;
    }
    status = AudioHardwareCreateProcessTap(muteDescription, &impl_->muteTap);
    if (!status) {
        NSDictionary* control = @{
            @kAudioAggregateDeviceNameKey : @"LoopZ private playback policy",
            @kAudioAggregateDeviceUIDKey : NSUUID.UUID.UUIDString,
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceTapAutoStartKey : @NO,
            @kAudioAggregateDeviceTapListKey : @[ @{
                @kAudioSubTapUIDKey : muteDescription.UUID.UUIDString,
                @kAudioSubTapDriftCompensationKey : @NO
            } ]
        };
        status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)control,
                                                    &impl_->muteAggregate);
    }
    if (!status) {
        auto address = property(kAudioDevicePropertyBufferFrameSize);
        const UInt32 frames = config.requestedBufferFrames;
        status = AudioObjectSetPropertyData(impl_->muteAggregate, &address, 0, nullptr,
                                            sizeof(frames), &frames);
    }
    if (!status)
        status = AudioDeviceCreateIOProcID(
            impl_->muteAggregate,
            [](AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
               AudioBufferList*, const AudioTimeStamp*, void*) -> OSStatus {
                return noErr;
            },
            nullptr, &impl_->muteIO);
    if (status || !setApplicationMute(mute)) {
        error = "Cannot apply original playback policy";
        impl_->stopApplication();
        return false;
    }
    impl_->appStartedNs = hostNowNs();
    return true;
}
bool Capture::setApplicationMute(bool mute) noexcept {
    if (!impl_->tap || !impl_->tapIO || !impl_->muteTap || !impl_->muteIO)
        return false;
    if (impl_->muteRunning == mute)
        return true;
    // CATapMutedWhenTapped follows this separate reader's IO lifetime. Neither
    // tap description nor the recording aggregate changes during a policy edit.
    const auto status = mute ? AudioDeviceStart(impl_->muteAggregate, impl_->muteIO)
                             : AudioDeviceStop(impl_->muteAggregate, impl_->muteIO);
    if (status)
        return false;
    impl_->muteRunning = mute;
    return true;
}
bool Capture::startMicrophone(const std::string& uid, std::string& error) {
    impl_->stopMicrophone();
    impl_->microphoneUID = uid;
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
    if (!status) {
        impl_->micCallbacks.open();
        status = AudioOutputUnitStart(impl_->micUnit);
    }
    if (status) {
        error = "Microphone capture failed with Core Audio status " + std::to_string(status);
        impl_->stopMicrophone();
        return false;
    }
    return true;
}
} // namespace loopz
