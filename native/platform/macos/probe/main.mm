#include <loopz/sample_rates.hpp>
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <loopz/macos/driver_protocol.hpp>
#include <loopz/probe_signal.hpp>
#include <loopz/probe_capture_trace.hpp>
#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <string_view>
#include <stdexcept>
#include <csignal>
#include <mach/mach_time.h>
#include "multi_output_source.hpp"
#include "input_capture_control.hpp"
#include "input_capture_trace_json.hpp"
#include <zcore/device/audio_input_callback_gate.h>
#if defined(LOOPZ_EMBEDDED_PROBE)
#include "virtual_input_check.hpp"
#include <loopz/macos/permissions.hpp>
#include <map>
#include <charconv>
#endif

namespace {
volatile std::sig_atomic_t probeStopping = 0;
#if defined(LOOPZ_EMBEDDED_PROBE)
class ProbeSignals {
    using Handler = void (*)(int);
    Handler interrupt_{SIG_DFL}, terminate_{SIG_DFL};
    static void stop(int) {
        probeStopping = 1;
    }

  public:
    ProbeSignals() {
        probeStopping = 0;
        interrupt_ = std::signal(SIGINT, stop);
        if (interrupt_ == SIG_ERR)
            throw std::runtime_error("Cannot observe virtual input cancellation");
        terminate_ = std::signal(SIGTERM, stop);
        if (terminate_ == SIG_ERR) {
            std::signal(SIGINT, interrupt_);
            throw std::runtime_error("Cannot observe virtual input cancellation");
        }
    }
    ~ProbeSignals() {
        std::signal(SIGINT, interrupt_);
        std::signal(SIGTERM, terminate_);
    }
};
#endif
bool loopzRoutingDevice(AudioDeviceID device, const std::string& uid) {
    if (!uid.starts_with("com.lexasoft.loopz.device."))
        return false;
    CFStringRef model = nullptr;
    UInt32 bytes = sizeof(model), transport = 0;
    AudioObjectPropertyAddress address{kAudioDevicePropertyModelUID,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &model) ||
        bytes != sizeof(model) || !model)
        return false;
    const bool match = CFGetTypeID(model) == CFStringGetTypeID() &&
                       CFEqual(model, CFSTR("com.lexasoft.loopz.device.model.v2"));
    CFRelease(model);
    if (!match)
        return false;
    address.mSelector = kAudioDevicePropertyTransportType;
    bytes = sizeof(transport);
    return !AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &transport) &&
           bytes == sizeof(transport) && transport == kAudioDeviceTransportTypeVirtual;
}
const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    return info;
}();
uint64_t nowNs() {
    auto ticks = mach_absolute_time();
    return ticks / timebase.denom * timebase.numer +
           ticks % timebase.denom * timebase.numer / timebase.denom;
}
AudioDeviceID virtualInputDevice(const std::string& uid) {
    CFStringRef text = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    if (!text)
        throw std::invalid_argument("Invalid virtual input UID");
    AudioObjectID device = 0;
    UInt32 bytes = sizeof(device);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal, 0};
    const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(text),
                                                   &text, &bytes, &device);
    CFRelease(text);
    if (status || bytes != sizeof(device) || !device)
        throw std::invalid_argument("The requested virtual input is unavailable");
    UInt32 transport = 0;
    bytes = sizeof(transport);
    address.mSelector = kAudioDevicePropertyTransportType;
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &transport) ||
        bytes != sizeof(transport) || transport != kAudioDeviceTransportTypeVirtual)
        throw std::invalid_argument("Input comparison requires a virtual device");
    address = {kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, 0};
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &bytes) ||
        bytes < offsetof(AudioBufferList, mBuffers) || bytes > 1024 * 1024)
        throw std::invalid_argument("Virtual input channels are unavailable");
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const auto capacity = bytes;
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, storage.data()) ||
        bytes > capacity || bytes < offsetof(AudioBufferList, mBuffers))
        throw std::invalid_argument("Virtual input channel data changed");
    const auto* list = reinterpret_cast<const AudioBufferList*>(storage.data());
    if (list->mNumberBuffers > (bytes - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer))
        throw std::invalid_argument("Invalid virtual input channel data");
    uint64_t channels = 0;
    for (UInt32 index = 0; index < list->mNumberBuffers; ++index)
        channels += list->mBuffers[index].mNumberChannels;
    if (channels < 2 || channels > 4096)
        throw std::invalid_argument("Virtual input comparison requires at least two channels");
    return device;
}
struct BufferPeriod {
    AudioDeviceID device{0};
    std::string uid;
    UInt32 original{0}, requested{0};
    bool changed{false};
    bool read(UInt32& frames) const noexcept {
        AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                           kAudioObjectPropertyScopeGlobal, 0};
        UInt32 bytes = sizeof(frames);
        return !AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &frames) &&
               bytes == sizeof(frames);
    }
    bool write(UInt32 frames) const noexcept {
        AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                           kAudioObjectPropertyScopeGlobal, 0};
        return !AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(frames), &frames);
    }
    void prepare(const std::string& selected, UInt32 frames) {
        uid = selected;
        device = virtualInputDevice(uid);
        requested = frames;
        if (!read(original) || !original)
            throw std::runtime_error("Cannot preserve the virtual input buffer period");
        if (original != requested) {
            changed = true;
            UInt32 actual = 0;
            if (!write(requested) || !read(actual) || actual != requested)
                throw std::runtime_error("Virtual input did not accept the requested period");
        }
    }
    bool restore() noexcept {
        if (!changed)
            return true;
        try {
            UInt32 current = 0;
            if (virtualInputDevice(uid) != device || !read(current))
                return false;
            if (current == original) {
                changed = false;
                return true;
            }
            // Never overwrite a concurrent client's different selection.
            if (current != requested || !write(original) || !read(current) || current != original)
                return false;
            changed = false;
            return true;
        } catch (...) {
            return false;
        }
    }
    ~BufferPeriod() {
        if (!restore())
            std::fprintf(stderr, "Virtual input buffer period could not be restored\n");
    }
};
struct Observation {
    uint64_t receivedNs{0}, firstFrame{0};
    uint32_t frames{0};
    float peak[2]{};
};
struct ToneWriter {
    double rate;
    double frequencies[2]{440, 880};
    uint64_t frame{0};
    bool timestamped{false};
    uint64_t timestampErrors{0};
    static OSStatus render(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp* time,
                           UInt32, UInt32 frames, AudioBufferList* output) {
        auto& self = *static_cast<ToneWriter*>(context);
        if (!output || output->mNumberBuffers != 2)
            return kAudio_ParamError;
        const bool valid = time && (time->mFlags & kAudioTimeStampHostTimeValid);
        const uint64_t ns =
            valid ? time->mHostTime / timebase.denom * timebase.numer +
                        time->mHostTime % timebase.denom * timebase.numer / timebase.denom
                  : 0;
        if (self.timestamped && !valid)
            ++self.timestampErrors;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            auto& buffer = output->mBuffers[channel];
            if (!buffer.mData || buffer.mDataByteSize < frames * sizeof(float))
                return kAudio_ParamError;
            auto* samples = static_cast<float*>(buffer.mData);
            for (uint32_t i = 0; i < frames; ++i)
                samples[i] =
                    0.1f * std::sin(2 * M_PI * self.frequencies[channel] * (self.frame + i) /
                                    self.rate) +
                    (self.timestamped && valid
                         ? loopz::probe::marker(
                               ns + static_cast<uint64_t>((i + 0.5) * 1e9 / self.rate), channel)
                         : 0);
        }
        self.frame += frames;
        return noErr;
    }
};
struct Recorder {
    AudioUnit unit{nullptr};
    std::array<float, 4096> left{}, right{};
    struct List {
        UInt32 count;
        AudioBuffer buffers[2];
    } list{2, {{1, 4096 * sizeof(float), left.data()}, {1, 4096 * sizeof(float), right.data()}}};
    std::vector<float> samples;
    std::vector<Observation> observations;
    uint64_t observationCount{0};
    bool streaming{false};
    std::atomic<bool> measuring{true};
    std::atomic<uint64_t> preparationCallbacks{0}, preparationFailures{0};
    bool coordinatedCapture{false};
    loopz::probe::CaptureTrace trace;
    loopz::probe::StreamCheck checker;
    double streamEnergy[2]{};
    std::atomic<uint64_t> quietFrames{0}, nonfiniteSamples{0};
    uint64_t warmupFrames{48000};
    int64_t minimumReceiptOffsetNs{INT64_MAX}, maximumReceiptOffsetNs{INT64_MIN};
    uint32_t minimumCallbackFrames{UINT32_MAX}, maximumCallbackFrames{0};
    std::atomic<uint64_t> position{0}, failures{0};
    uint64_t zeroSamples{0};
    singz::AudioInputCallbackGate callbacks;
    void stop() noexcept {
        callbacks.beginClose();
        if (unit)
            AudioOutputUnitStop(unit);
        while (callbacks.inFlight())
            std::this_thread::yield();
    }
    ~Recorder() {
        if (unit) {
            stop();
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
        }
    }
    static OSStatus render(void* context, AudioUnitRenderActionFlags* flags,
                           const AudioTimeStamp* time, UInt32 bus, UInt32 frames,
                           AudioBufferList*) {
        auto& self = *static_cast<Recorder*>(context);
        singz::AudioInputCallbackScope admission(self.callbacks);
        if (!admission)
            return noErr;
        const bool measuring = self.measuring.load(std::memory_order_acquire);
        if (!frames || frames > self.left.size()) {
            (measuring ? self.failures : self.preparationFailures).fetch_add(1);
            return noErr;
        }
        self.list.buffers[0].mDataByteSize = self.list.buffers[1].mDataByteSize =
            frames * sizeof(float);
        auto status = AudioUnitRender(self.unit, flags, time, bus, frames,
                                      reinterpret_cast<AudioBufferList*>(&self.list));
        if (status) {
            (measuring ? self.failures : self.preparationFailures).fetch_add(1);
            return status;
        }
        if (!measuring) {
            self.preparationCallbacks.fetch_add(1);
            return noErr;
        }
        if (self.streaming) {
            const bool valid = time && (time->mFlags & kAudioTimeStampHostTimeValid) &&
                               (time->mFlags & kAudioTimeStampSampleTimeValid);
            const uint64_t ns =
                valid ? time->mHostTime / timebase.denom * timebase.numer +
                            time->mHostTime % timebase.denom * timebase.numer / timebase.denom
                      : 0;
            if (self.coordinatedCapture)
                self.trace.observe({self.left.data(), frames}, {self.right.data(), frames},
                                   valid ? time->mSampleTime : 0, ns, nowNs(), self.checker.rate,
                                   valid, flags ? *flags : 0);
            self.checker.append(self.left.data(), self.right.data(), frames, ns,
                                valid ? time->mSampleTime : 0, valid);
        }
        auto first = self.position.load();
        if (first >= self.warmupFrames && time && (time->mFlags & kAudioTimeStampHostTimeValid)) {
            const auto logical = time->mHostTime / timebase.denom * timebase.numer +
                                 time->mHostTime % timebase.denom * timebase.numer / timebase.denom;
            const auto received = nowNs();
            const auto offset = received >= logical ? static_cast<int64_t>(received - logical)
                                                    : -static_cast<int64_t>(logical - received);
            self.minimumReceiptOffsetNs = std::min(self.minimumReceiptOffsetNs, offset);
            self.maximumReceiptOffsetNs = std::max(self.maximumReceiptOffsetNs, offset);
            self.minimumCallbackFrames = std::min(self.minimumCallbackFrames, frames);
            self.maximumCallbackFrames = std::max(self.maximumCallbackFrames, frames);
        }
        auto capacity = self.samples.size() / 2;
        const auto copy = self.streaming     ? frames
                          : first < capacity ? std::min<uint64_t>(frames, capacity - first)
                                             : 0;
        Observation observation{nowNs(), first, frames, {0, 0}};
        if (!self.streaming)
            for (uint64_t i = 0; i < copy; ++i) {
                self.samples[2 * (first + i)] = self.left[i];
                self.samples[2 * (first + i) + 1] = self.right[i];
            }
        for (uint64_t i = 0; i < copy; ++i) {
            observation.peak[0] = std::max(observation.peak[0], std::fabs(self.left[i]));
            observation.peak[1] = std::max(observation.peak[1], std::fabs(self.right[i]));
            if (self.streaming)
                for (uint32_t channel = 0; channel < 2; ++channel) {
                    const double sample = channel ? self.right[i] : self.left[i];
                    if (!std::isfinite(sample))
                        self.nonfiniteSamples.fetch_add(1);
                    else
                        self.streamEnergy[channel] += sample * sample;
                    if (first + i >= self.warmupFrames && sample == 0)
                        ++self.zeroSamples;
                }
        }
        if (self.observationCount < self.observations.size())
            self.observations[self.observationCount++] = observation;
        if (self.streaming && first >= self.warmupFrames &&
            (observation.peak[0] < 0.02f || observation.peak[1] < 0.02f))
            self.quietFrames.fetch_add(copy);
        self.position.store(first + copy);
        return noErr;
    }
};
} // namespace
static int runProbe(int argc, char** argv, bool virtualInputOnly = false, uint32_t bufferFrames = 0,
                    bool requireMarker = true, bool coordinatedCapture = false) {
    if (argc > 1 && std::string(argv[1]) == "--multi-output-source")
        return loopz::macos::multiOutputSource(argc - 1, argv + 1);
    @autoreleasepool {
        double seconds = 5, requestedRate = 0;
        std::string expect = "any", report;
        bool sendTone = false, streaming = false, timestamped = false, rateRequested = false;
        double frequencies[2]{440, 880};
        bool timingRequested = false;
        uint32_t startupDelayMs = 0;
        std::string timingConfig;
        std::string deviceUID = loopz::kDeviceUID;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--driver-timing")
                timingRequested = true;
            else if (arg == "--set-timing" && i + 1 < argc) {
                timingRequested = true;
                timingConfig = argv[++i];
            } else if (arg == "--device-uid" && i + 1 < argc)
                deviceUID = argv[++i];
            else if (arg == "--seconds" && i + 1 < argc)
                seconds = std::strtod(argv[++i], nullptr);
            else if (arg == "--startup-delay-ms" && i + 1 < argc) {
                const std::string value = argv[++i];
                size_t end = 0;
                try {
                    const auto parsed = std::stoul(value, &end);
                    if (end != value.size() || parsed > 5000)
                        return 2;
                    startupDelayMs = static_cast<uint32_t>(parsed);
                } catch (const std::exception&) {
                    return 2;
                }
            } else if (arg == "--report" && i + 1 < argc)
                report = argv[++i];
            else if (arg == "--set-rate" && i + 1 < argc) {
                requestedRate = std::strtod(argv[++i], nullptr);
                rateRequested = true;
            } else if (arg == "--send-tone")
                sendTone = true;
            else if (arg == "--streaming")
                streaming = true;
            else if (arg == "--timestamped")
                timestamped = true;
            else if (arg == "--frequency-left" && i + 1 < argc)
                frequencies[0] = std::strtod(argv[++i], nullptr);
            else if (arg == "--frequency-right" && i + 1 < argc)
                frequencies[1] = std::strtod(argv[++i], nullptr);
            else if (arg == "--expect-tone")
                expect = "tone";
            else if (arg == "--expect-silence")
                expect = "silence";
            else if (arg == "--expect-audio")
                expect = "audio";
            else {
                std::fprintf(stderr, "Unknown probe argument\n");
                return 2;
            }
        }
        if (!std::isfinite(seconds) || seconds < 1 ||
            seconds > ((sendTone || streaming) ? 86400 : 120))
            return 2;
        if (streaming && (sendTone || !report.empty() || (expect != "any" && expect != "audio")))
            return 2;
        if (startupDelayMs && !sendTone)
            return 2;
        AudioDeviceID device = 0;
        auto uid = CFStringCreateWithCString(nullptr, deviceUID.c_str(), kCFStringEncodingUTF8);
        AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        UInt32 bytes = sizeof(device);
        auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(uid),
                                                 &uid, &bytes, &device);
        CFRelease(uid);
        if (status || !device) {
            std::fprintf(stderr, "Requested audio device is not visible\n");
            return 1;
        }
        BufferPeriod period;
        if (virtualInputOnly) {
            period.prepare(deviceUID, bufferFrames);
            device = period.device;
        }
        if ((sendTone || rateRequested) && deviceUID != loopz::kDeviceUID &&
            !loopzRoutingDevice(device, deviceUID)) {
            std::fprintf(stderr,
                         "Playback and rate tests require a verified LoopZ virtual device\n");
            return 2;
        }
        if (timingRequested) {
            if (deviceUID != loopz::kDeviceUID)
                return 2;
            address.mSelector = 0x4c5a746d;
            unsigned period = 0, queue = 0, source = 0;
            char extra = 0;
            if (!timingConfig.empty()) {
                if (std::sscanf(timingConfig.c_str(), "%u,%u,%u%c", &period, &queue, &source,
                                &extra) != 3)
                    return 2;
                NSDictionary* config = @{
                    @"periodFrames" : @(period),
                    @"queueFrames" : @(queue),
                    @"sourceFrames" : @(source)
                };
                CFPropertyListRef data = (__bridge CFPropertyListRef)config;
                status =
                    AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(data), &data);
                if (status) {
                    std::fprintf(stderr, "Driver timing request failed (%d)\n",
                                 static_cast<int>(status));
                    return 1;
                }
            }
            for (unsigned attempt = 0; attempt < 150; ++attempt) {
                CFPropertyListRef data = nullptr;
                bytes = sizeof(data);
                status = AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &data);
                if (status || !data) {
                    std::fprintf(stderr, "Driver timing property unavailable (%d)\n",
                                 static_cast<int>(status));
                    return 1;
                }
                id object = CFBridgingRelease(data);
                if (![object isKindOfClass:NSDictionary.class])
                    return 1;
                NSDictionary* config = object;
                if (timingConfig.empty() ||
                    ([config[@"periodFrames"] unsignedIntValue] == period &&
                     [config[@"queueFrames"] unsignedIntValue] == queue &&
                     [config[@"sourceFrames"] unsignedIntValue] == source)) {
                    NSData* json = [NSJSONSerialization dataWithJSONObject:config
                                                                   options:0
                                                                     error:nil];
                    if (!json)
                        return 1;
                    std::fwrite(json.bytes, 1, json.length, stdout);
                    std::puts("");
                    return 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::fprintf(stderr, "Timing configuration was not applied\n");
            return 1;
        }
        if (rateRequested) {
            if (!loopz::supportedVirtualSampleRate(requestedRate))
                return 2;
            address.mSelector = kAudioDevicePropertyNominalSampleRate;
            status = AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(requestedRate),
                                                &requestedRate);
            if (status) {
                std::fprintf(stderr, "Rate change failed: %d\n", static_cast<int>(status));
                return 1;
            }
            for (uint32_t retry = 0; retry < 100; ++retry) {
                double actual = 0;
                bytes = sizeof(actual);
                if (!AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &actual) &&
                    actual == requestedRate) {
                    std::printf("{\"rate\":%.0f,\"changed\":true}\n", actual);
                    return 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::fprintf(stderr, "Rate change was not applied within two seconds\n");
            return 1;
        }
        double rate = 0;
        address.mSelector = kAudioDevicePropertyNominalSampleRate;
        bytes = sizeof(rate);
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &rate) ||
            rate < 8000 || rate > 192000)
            return 1;
        for (auto frequency : frequencies)
            if (!std::isfinite(frequency) || frequency <= 0 || frequency >= rate / 2)
                return 2;
        ToneWriter writer{rate, {frequencies[0], frequencies[1]}};
        writer.timestamped = timestamped;
        Recorder recorder;
        recorder.checker.rate = rate;
        recorder.checker.requireMarker = requireMarker;
        recorder.checker.frequencies[0] = frequencies[0];
        recorder.checker.frequencies[1] = frequencies[1];
        recorder.streaming = streaming;
        recorder.coordinatedCapture = coordinatedCapture;
        recorder.measuring.store(!coordinatedCapture);
        recorder.warmupFrames = coordinatedCapture ? 0 : static_cast<uint64_t>(rate);
        recorder.checker.skipWarmup = coordinatedCapture;
        if (!sendTone && !streaming) {
            recorder.samples.resize(static_cast<size_t>((seconds + 1) * rate) * 2);
            recorder.observations.resize(static_cast<size_t>((seconds + 1) * rate / 16) + 1024);
        }
        AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                              kAudioUnitManufacturer_Apple, 0, 0};
        auto component = AudioComponentFindNext(nullptr, &description);
        status = component ? AudioComponentInstanceNew(component, &recorder.unit) : -1;
        UInt32 inputEnabled = sendTone ? 0 : 1, outputEnabled = sendTone ? 1 : 0;
        if (!status)
            status =
                AudioUnitSetProperty(recorder.unit, kAudioOutputUnitProperty_EnableIO,
                                     kAudioUnitScope_Input, 1, &inputEnabled, sizeof(inputEnabled));
        if (!status)
            status = AudioUnitSetProperty(recorder.unit, kAudioOutputUnitProperty_EnableIO,
                                          kAudioUnitScope_Output, 0, &outputEnabled,
                                          sizeof(outputEnabled));
        if (!status)
            status = AudioUnitSetProperty(recorder.unit, kAudioOutputUnitProperty_CurrentDevice,
                                          kAudioUnitScope_Global, 0, &device, sizeof(device));
        AudioStreamBasicDescription format{
            rate,
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
            status = AudioUnitSetProperty(recorder.unit, kAudioUnitProperty_StreamFormat,
                                          sendTone ? kAudioUnitScope_Input : kAudioUnitScope_Output,
                                          sendTone ? 0 : 1, &format, sizeof(format));
        AURenderCallbackStruct callback{sendTone ? ToneWriter::render : Recorder::render,
                                        sendTone ? static_cast<void*>(&writer)
                                                 : static_cast<void*>(&recorder)};
        if (!status)
            status = AudioUnitSetProperty(
                recorder.unit,
                sendTone ? static_cast<UInt32>(kAudioUnitProperty_SetRenderCallback)
                         : static_cast<UInt32>(kAudioOutputUnitProperty_SetInputCallback),
                sendTone ? kAudioUnitScope_Input : kAudioUnitScope_Global, 0, &callback,
                sizeof(callback));
        if (!status)
            status = AudioUnitInitialize(recorder.unit);
        if (!status && startupDelayMs) {
            std::printf("{\"event\":\"source-waiting\",\"startupDelayMs\":%u}\n", startupDelayMs);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(startupDelayMs));
        }
        if (!status) {
            recorder.callbacks.open();
            status = AudioOutputUnitStart(recorder.unit);
        }
        if (status) {
            std::fprintf(stderr, "Virtual-device capture failed: %d\n", static_cast<int>(status));
            return 1;
        }
        if (sendTone) {
            std::printf("{\"event\":\"source-started\",\"rate\":%.0f}\n", rate);
            std::fflush(stdout);
        }
        if (streaming) {
            if (coordinatedCapture) {
                const auto readyUntil = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (!recorder.preparationCallbacks.load() && !probeStopping &&
                       std::chrono::steady_clock::now() < readyUntil)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!recorder.preparationCallbacks.load())
                    throw std::runtime_error(
                        "Input-only client did not receive preparation callbacks");
                loopz::macos::diagnostic::inputCaptureEvent("prepared");
                loopz::macos::diagnostic::awaitInputCapture("begin-capture", probeStopping);
                recorder.measuring.store(true, std::memory_order_release);
            }
            const auto started = std::chrono::steady_clock::now();
            uint64_t nextReport = 60;
            while (
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() <
                    seconds &&
                !probeStopping) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                recorder.checker.consume();
                const auto elapsed = static_cast<uint64_t>(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                        .count());
                if (elapsed >= nextReport) {
                    const auto& check = recorder.checker;
                    char delay[64] = "null";
                    if (requireMarker)
                        std::snprintf(delay, sizeof(delay), "[%.0f,%.0f]", check.minimumDelayMs,
                                      check.maximumDelayMs);
                    std::printf(
                        "{\"event\":\"progress\",\"elapsedSeconds\":%llu,\"frames\":%llu,"
                        "\"quietFrames\":%llu,\"callbackFailures\":%llu,\"nonfiniteSamples\":%llu,"
                        "\"windows\":%llu,\"badWindows\":%llu,\"timestampErrors\":%llu,"
                        "\"queueOverruns\":%llu,\"delayMs\":%s,\"markerChecked\":%s}\n",
                        elapsed, recorder.position.load(), recorder.quietFrames.load(),
                        recorder.failures.load(), recorder.nonfiniteSamples.load(), check.windows,
                        check.badWindows, check.timestampErrors.load(), check.queueOverruns.load(),
                        delay, requireMarker ? "true" : "false");
                    std::fflush(stdout);
                    nextReport += 60;
                }
            }
        } else
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
        recorder.stop();
        if (coordinatedCapture) {
            loopz::macos::diagnostic::inputCaptureEvent("captured");
            loopz::macos::diagnostic::awaitInputCapture("release-input", probeStopping);
        }
        if (sendTone) {
            std::printf("{\"sentFrames\":%llu,\"rate\":%.0f,\"target\":\"LoopZ Audio\"}\n",
                        writer.frame, rate);
            return writer.frame && !writer.timestampErrors ? 0 : 1;
        }
        const auto frames = recorder.position.load();
        if (streaming) {
            const double left = frames ? std::sqrt(recorder.streamEnergy[0] / frames) : 0;
            const double right = frames ? std::sqrt(recorder.streamEnergy[1] / frames) : 0;
            recorder.checker.consume();
            const auto& check = recorder.checker;
            const bool signalPass =
                check.windows >= static_cast<uint64_t>(seconds) - 2 && !check.badWindows &&
                !check.timestampErrors.load() && !check.queueOverruns.load() &&
                (!requireMarker || check.maximumDelayMs - check.minimumDelayMs <= 2);
            const bool restored = period.restore();
            const bool pass =
                signalPass && frames >= seconds * rate - 8192 && restored && !probeStopping &&
                !recorder.failures.load() && !recorder.nonfiniteSamples.load() &&
                !recorder.quietFrames.load() && left > 0.03 && right > 0.03 &&
                (!virtualInputOnly || rate != 48000 ||
                 (requireMarker ? !recorder.zeroSamples : check.maximumZeroRun.load() <= 2)) &&
                (!coordinatedCapture ||
                 (!recorder.trace.overflows && !recorder.trace.invalidBlocks &&
                  recorder.trace.gaps().empty()));
            char delay[64] = "null", correlation[64] = "null";
            if (requireMarker) {
                std::snprintf(delay, sizeof(delay), "[%.0f,%.0f]", check.minimumDelayMs,
                              check.maximumDelayMs);
                std::snprintf(correlation, sizeof(correlation), "%.6f", check.minimumCorrelation);
            }
            std::printf(
                "{\"event\":\"complete\",\"passed\":%s,\"frames\":%llu,\"rate\":%.0f,\"rms\":[%.8f,"
                "%.8f],\"quietFrames\":%llu,\"callbackFailures\":%llu,\"nonfiniteSamples\":%llu,"
                "\"windows\":%llu,\"badWindows\":%llu,\"timestampErrors\":%llu,\"queueOverruns\":%"
                "llu,\"minimumAmplitude\":%.6f,\"maximumCross\":%.6f,\"minimumMarkerCorrelation\":%"
                "s,"
                "\"delayMs\":%s,\"delayResolutionMs\":%s,\"markerChecked\":%s,\"maximumZeroRun\":%"
                "llu,\"callbackReceiptOffsetMs\":[%"
                ".6f,%.6f],\"callbackFrames\":[%u,%u],\"zeroSamples\":%llu,"
                "\"invalidTimestamps\":%llu,\"forwardTimestampJumps\":%llu,"
                "\"backwardTimestampJumps\":%llu,\"maximumForwardTimestampFrames\":%.3f,"
                "\"maximumBackwardTimestampFrames\":%.3f,"
                "\"virtualInputOnly\":%s,\"requestedBufferFrames\":%u,\"periodRestored\":%s,"
                "\"cancelled\":%s,\"coordinatedCapture\":%s,\"preparationCallbacks\":%llu,"
                "\"preparationFailures\":%llu,\"captureTrace\":%s}\n",
                pass ? "true" : "false", frames, rate, left, right, recorder.quietFrames.load(),
                recorder.failures.load(), recorder.nonfiniteSamples.load(), check.windows,
                check.badWindows, check.timestampErrors.load(), check.queueOverruns.load(),
                check.minimumAmplitude, check.maximumCross, correlation, delay,
                requireMarker ? "1" : "null", requireMarker ? "true" : "false",
                check.maximumZeroRun.load(), recorder.minimumReceiptOffsetNs / 1e6,
                recorder.maximumReceiptOffsetNs / 1e6, recorder.minimumCallbackFrames,
                recorder.maximumCallbackFrames, recorder.zeroSamples,
                check.invalidTimestamps.load(), check.forwardTimestampJumps.load(),
                check.backwardTimestampJumps.load(), check.maximumForwardTimestampFrames.load(),
                check.maximumBackwardTimestampFrames.load(), virtualInputOnly ? "true" : "false",
                bufferFrames, restored ? "true" : "false", probeStopping ? "true" : "false",
                coordinatedCapture ? "true" : "false", recorder.preparationCallbacks.load(),
                recorder.preparationFailures.load(),
                coordinatedCapture ? captureTraceJSON(recorder.trace).c_str() : "null");
            return pass ? 0 : 1;
        }
        double energy[2]{}, tone[2]{}, cross[2]{};
        const auto skip = static_cast<uint64_t>(rate / 2);
        if (frames <= skip) {
            std::fprintf(stderr, "No audio callbacks received\n");
            return 1;
        }
        for (uint32_t channel = 0; channel < 2; ++channel) {
            double real = 0, imag = 0, cr = 0, ci = 0;
            for (uint64_t i = skip; i < frames; ++i) {
                double sample = recorder.samples[2 * i + channel];
                energy[channel] += sample * sample;
                double angle = 2 * M_PI * frequencies[channel] * i / rate;
                real += sample * std::cos(angle);
                imag += sample * std::sin(angle);
                double other = 2 * M_PI * frequencies[1 - channel] * i / rate;
                cr += sample * std::cos(other);
                ci += sample * std::sin(other);
            }
            energy[channel] = std::sqrt(energy[channel] / (frames - skip));
            tone[channel] = 2 * std::hypot(real, imag) / (frames - skip);
            cross[channel] = 2 * std::hypot(cr, ci) / (frames - skip);
        }
        bool pass = recorder.failures.load() == 0;
        if (expect == "tone")
            pass &= tone[0] > 0.07 && tone[1] > 0.07 && cross[0] < 0.01 && cross[1] < 0.01;
        if (expect == "silence")
            pass &= energy[0] < 1e-6 && energy[1] < 1e-6;
        if (expect == "audio")
            pass &= energy[0] > 1e-5 || energy[1] > 1e-5;
        if (!report.empty()) {
            NSMutableArray* blocks = [NSMutableArray arrayWithCapacity:recorder.observationCount];
            for (uint64_t i = 0; i < recorder.observationCount; ++i) {
                const auto& o = recorder.observations[i];
                [blocks addObject:@{
                    @"receivedNs" : @(o.receivedNs),
                    @"firstFrame" : @(o.firstFrame),
                    @"frames" : @(o.frames),
                    @"peak" : @[ @(o.peak[0]), @(o.peak[1]) ]
                }];
            }
            NSData* data = [NSJSONSerialization dataWithJSONObject:@{
                @"rate" : @(rate),
                @"frames" : @(frames),
                @"blocks" : blocks
            }
                                                           options:0
                                                             error:nil];
            if (![data writeToFile:@(report.c_str()) atomically:YES]) {
                std::fprintf(stderr, "Cannot write observation report\n");
                return 1;
            }
        }
        std::printf(
            "{\"passed\":%s,\"expect\":\"%s\",\"frames\":%llu,\"rate\":%.0f,\"rms\":[%.8f,%.8f],"
            "\"toneAmplitude\":[%.8f,%.8f],\"crosstalk\":[%.8f,%.8f],\"callbackFailures\":%llu}\n",
            pass ? "true" : "false", expect.c_str(), frames, rate, energy[0], energy[1], tone[0],
            tone[1], cross[0], cross[1], recorder.failures.load());
        return pass ? 0 : 1;
    }
}

#if defined(LOOPZ_EMBEDDED_PROBE)
int loopz::macos::virtualInputCheck(int argc, char** argv) {
    try {
        std::map<std::string, std::string> options;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 == argc)
                throw std::invalid_argument("Virtual input arguments require values");
            const std::string key = argv[index];
            if (key != "--read-device" && key != "--seconds" && key != "--buffer-frames" &&
                key != "--frequency-left" && key != "--frequency-right" && key != "--signal" &&
                key != "--capture-protocol")
                throw std::invalid_argument("Unknown virtual input argument");
            if (!options.emplace(key, argv[index + 1]).second)
                throw std::invalid_argument("Duplicate virtual input argument");
        }
        const auto number = [&](const char* key, uint32_t fallback) {
            if (!options.contains(key))
                return fallback;
            const auto& text = options.at(key);
            uint32_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw std::invalid_argument("Invalid virtual input integer");
            return value;
        };
        const auto seconds = number("--seconds", 20), frames = number("--buffer-frames", 32);
        const auto signal = options.contains("--signal") ? options.at("--signal") : "marker";
        const bool coordinatedCapture = options.contains("--capture-protocol");
        if (coordinatedCapture && options.at("--capture-protocol") != "stdio-v1")
            throw std::invalid_argument("Virtual input capture protocol must be stdio-v1");
        if (signal != "marker" && signal != "sine")
            throw std::invalid_argument("Virtual input signal must be marker or sine");
        if (seconds < 3 || seconds > 120 || frames < 32 || frames > 2048 || (frames & (frames - 1)))
            throw std::invalid_argument("Virtual input requires 3–120 seconds and 32–2048 frames");
        for (const auto* key : {"--frequency-left", "--frequency-right"}) {
            if (!options.contains(key))
                options[key] = key == std::string_view("--frequency-left") ? "1100" : "2200";
            size_t used = 0;
            const auto value = std::stod(options.at(key), &used);
            if (used != options.at(key).size() || !std::isfinite(value) || value < 40 ||
                value > 20000)
                throw std::invalid_argument("Invalid virtual input frequency");
        }
        if (std::stod(options.at("--frequency-left")) == std::stod(options.at("--frequency-right")))
            throw std::invalid_argument("Virtual input frequencies must differ");
        if (!options.contains("--read-device") || options.at("--read-device").empty())
            throw std::invalid_argument("A virtual input UID is required");
        virtualInputDevice(options.at("--read-device"));
        if (microphonePermission() != MicrophonePermission::Authorized) {
            std::puts(
                "{\"passed\":false,\"permissionRequired\":true,\"permissionRequested\":false}");
            return 77;
        }
        std::vector<std::string> arguments{"virtual-input-check",
                                           "--device-uid",
                                           options.at("--read-device"),
                                           "--streaming",
                                           "--expect-audio",
                                           "--seconds",
                                           std::to_string(seconds),
                                           "--frequency-left",
                                           options.at("--frequency-left"),
                                           "--frequency-right",
                                           options.at("--frequency-right")};
        std::vector<char*> pointers;
        for (auto& value : arguments)
            pointers.push_back(value.data());
        ProbeSignals signals;
        return runProbe(static_cast<int>(pointers.size()), pointers.data(), true, frames,
                        signal == "marker", coordinatedCapture);
    } catch (const std::invalid_argument& error) {
        std::fprintf(stderr, "Virtual input check: %s\n", error.what());
        return 2;
    } catch (const std::out_of_range&) {
        std::fprintf(stderr, "Virtual input check: Numeric argument is out of range\n");
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Virtual input check: %s\n", error.what());
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    return runProbe(argc, argv);
}
#endif
