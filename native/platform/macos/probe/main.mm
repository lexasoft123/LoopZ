#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <loopz/macos/driver_protocol.hpp>
#include <loopz/probe_signal.hpp>
#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <mach/mach_time.h>

namespace {
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
    loopz::probe::StreamCheck checker;
    double streamEnergy[2]{};
    std::atomic<uint64_t> quietFrames{0}, nonfiniteSamples{0};
    uint64_t warmupFrames{48000};
    int64_t minimumReceiptOffsetNs{INT64_MAX}, maximumReceiptOffsetNs{INT64_MIN};
    uint32_t minimumCallbackFrames{UINT32_MAX}, maximumCallbackFrames{0};
    std::atomic<uint64_t> position{0}, failures{0};
    ~Recorder() {
        if (unit) {
            AudioOutputUnitStop(unit);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
        }
    }
    static OSStatus render(void* context, AudioUnitRenderActionFlags* flags,
                           const AudioTimeStamp* time, UInt32 bus, UInt32 frames,
                           AudioBufferList*) {
        auto& self = *static_cast<Recorder*>(context);
        if (frames > self.left.size()) {
            self.failures.fetch_add(1);
            return noErr;
        }
        self.list.buffers[0].mDataByteSize = self.list.buffers[1].mDataByteSize =
            frames * sizeof(float);
        auto status = AudioUnitRender(self.unit, flags, time, bus, frames,
                                      reinterpret_cast<AudioBufferList*>(&self.list));
        if (status) {
            self.failures.fetch_add(1);
            return status;
        }
        if (self.streaming) {
            const bool valid = time && (time->mFlags & kAudioTimeStampHostTimeValid) &&
                               (time->mFlags & kAudioTimeStampSampleTimeValid);
            const uint64_t ns =
                valid ? time->mHostTime / timebase.denom * timebase.numer +
                            time->mHostTime % timebase.denom * timebase.numer / timebase.denom
                      : 0;
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
int main(int argc, char** argv) {
    @autoreleasepool {
        double seconds = 5, requestedRate = 0;
        std::string expect = "any", report;
        bool sendTone = false, streaming = false, timestamped = false;
        double frequencies[2]{440, 880};
        bool timingRequested = false;
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
            else if (arg == "--report" && i + 1 < argc)
                report = argv[++i];
            else if (arg == "--set-rate" && i + 1 < argc)
                requestedRate = std::strtod(argv[++i], nullptr);
            else if (arg == "--send-tone")
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
        if (sendTone && deviceUID != loopz::kDeviceUID) {
            std::fprintf(stderr, "The test source only plays to LoopZ Audio\n");
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
        if (requestedRate) {
            if (requestedRate != 44100 && requestedRate != 48000)
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
        recorder.checker.frequencies[0] = frequencies[0];
        recorder.checker.frequencies[1] = frequencies[1];
        recorder.streaming = streaming;
        recorder.warmupFrames = static_cast<uint64_t>(rate);
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
        if (!status)
            status = AudioOutputUnitStart(recorder.unit);
        if (status) {
            std::fprintf(stderr, "Virtual-device capture failed: %d\n", static_cast<int>(status));
            return 1;
        }
        if (sendTone) {
            std::printf("{\"event\":\"source-started\",\"rate\":%.0f}\n", rate);
            std::fflush(stdout);
        }
        if (streaming) {
            const auto started = std::chrono::steady_clock::now();
            uint64_t nextReport = 60;
            while (
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() <
                seconds) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                recorder.checker.consume();
                const auto elapsed = static_cast<uint64_t>(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                        .count());
                if (elapsed >= nextReport) {
                    const auto& check = recorder.checker;
                    std::printf(
                        "{\"event\":\"progress\",\"elapsedSeconds\":%llu,\"frames\":%llu,"
                        "\"quietFrames\":%llu,\"callbackFailures\":%llu,\"nonfiniteSamples\":%llu,"
                        "\"windows\":%llu,\"badWindows\":%llu,\"timestampErrors\":%llu,"
                        "\"queueOverruns\":%llu,\"delayMs\":[%.0f,%.0f]}\n",
                        elapsed, recorder.position.load(), recorder.quietFrames.load(),
                        recorder.failures.load(), recorder.nonfiniteSamples.load(), check.windows,
                        check.badWindows, check.timestampErrors.load(), check.queueOverruns.load(),
                        check.minimumDelayMs, check.maximumDelayMs);
                    std::fflush(stdout);
                    nextReport += 60;
                }
            }
        } else
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
        AudioOutputUnitStop(recorder.unit);
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
            const bool signalPass = check.windows >= static_cast<uint64_t>(seconds) - 2 &&
                                    !check.badWindows && !check.timestampErrors.load() &&
                                    !check.queueOverruns.load() &&
                                    check.maximumDelayMs - check.minimumDelayMs <= 2;
            const bool pass = signalPass && frames >= seconds * rate - 8192 &&
                              !recorder.failures.load() && !recorder.nonfiniteSamples.load() &&
                              !recorder.quietFrames.load() && left > 0.03 && right > 0.03;
            std::printf(
                "{\"event\":\"complete\",\"passed\":%s,\"frames\":%llu,\"rate\":%.0f,\"rms\":[%.8f,"
                "%.8f],\"quietFrames\":%llu,\"callbackFailures\":%llu,\"nonfiniteSamples\":%llu,"
                "\"windows\":%llu,\"badWindows\":%llu,\"timestampErrors\":%llu,\"queueOverruns\":%"
                "llu,\"minimumAmplitude\":%.6f,\"maximumCross\":%.6f,\"minimumMarkerCorrelation\":%"
                ".6f,\"delayMs\":[%.0f,%.0f],\"delayResolutionMs\":1,\"callbackReceiptOffsetMs\":[%"
                ".6f,%.6f],\"callbackFrames\":[%u,%u]}\n",
                pass ? "true" : "false", frames, rate, left, right, recorder.quietFrames.load(),
                recorder.failures.load(), recorder.nonfiniteSamples.load(), check.windows,
                check.badWindows, check.timestampErrors.load(), check.queueOverruns.load(),
                check.minimumAmplitude, check.maximumCross, check.minimumCorrelation,
                check.minimumDelayMs, check.maximumDelayMs, recorder.minimumReceiptOffsetNs / 1e6,
                recorder.maximumReceiptOffsetNs / 1e6, recorder.minimumCallbackFrames,
                recorder.maximumCallbackFrames);
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
