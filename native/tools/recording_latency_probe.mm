#include "recording_latency_analysis.hpp"
#include <zcore/device/audio_input_callback_gate.h>

#import <AVFoundation/AVFoundation.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kRate = 48000;
constexpr uint32_t kMaximumBlock = 4096;
volatile sig_atomic_t canceled = 0;
mach_timebase_info_data_t timebase{};

void cancel(int) {
    canceled = 1;
}
uint64_t nanoseconds(uint64_t ticks) noexcept {
    return ticks / timebase.denom * timebase.numer +
           ticks % timebase.denom * timebase.numer / timebase.denom;
}
void check(OSStatus status, const char* operation) {
    if (status)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}
AudioObjectPropertyAddress
property(AudioObjectPropertySelector selector,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template <class T>
T get(AudioDeviceID device, AudioObjectPropertySelector selector,
      AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    T value{};
    UInt32 bytes = sizeof(value);
    const auto address = property(selector, scope);
    check(AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &value),
          "Read device property");
    if (bytes != sizeof(value))
        throw std::runtime_error("Unexpected device property size");
    return value;
}
struct InputHardwareLatency {
    uint32_t deviceFrames, streamFrames, safetyFrames;
    AudioStreamID stream;
    double rate;
    bool operator==(const InputHardwareLatency&) const = default;
};
InputHardwareLatency inputHardwareLatency(AudioDeviceID device, uint32_t channel) {
    InputHardwareLatency result{
        get<UInt32>(device, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeInput), 0,
        get<UInt32>(device, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeInput), 0,
        get<Float64>(device, kAudioDevicePropertyNominalSampleRate)};
    if (result.rate != kRate)
        throw std::runtime_error("Physical input rate changed");
    auto address = property(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput);
    UInt32 bytes = 0;
    check(AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &bytes),
          "Read input stream count");
    if (!bytes || bytes % sizeof(AudioStreamID) || bytes > 4096)
        throw std::runtime_error("Invalid input stream list");
    std::vector<AudioStreamID> streams(bytes / sizeof(AudioStreamID));
    const auto expected = bytes;
    check(AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, streams.data()),
          "Read input streams");
    if (bytes != expected)
        throw std::runtime_error("Input streams changed while reading latency");
    for (const auto stream : streams) {
        const auto first = get<UInt32>(stream, kAudioStreamPropertyStartingChannel);
        const auto format =
            get<AudioStreamBasicDescription>(stream, kAudioStreamPropertyVirtualFormat);
        if (first && channel >= first - 1 &&
            uint64_t(channel) < uint64_t(first - 1) + format.mChannelsPerFrame) {
            if (result.stream)
                throw std::runtime_error("Ambiguous input channel latency");
            result.stream = stream;
            result.streamFrames = get<UInt32>(stream, kAudioStreamPropertyLatency);
        }
    }
    if (!result.stream)
        throw std::runtime_error("Selected input stream latency is unavailable");
    return result;
}
AudioDeviceID findDevice(const char* text, bool requireVirtual) {
    CFStringRef uid = CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8);
    AudioDeviceID device = 0;
    UInt32 bytes = sizeof(device);
    const auto address = property(kAudioHardwarePropertyTranslateUIDToDevice);
    const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(uid),
                                                   &uid, &bytes, &device);
    if (uid)
        CFRelease(uid);
    check(status, "Find capture device");
    if (!device || !get<UInt32>(device, kAudioDevicePropertyDeviceIsAlive) ||
        get<Float64>(device, kAudioDevicePropertyNominalSampleRate) != kRate ||
        (get<UInt32>(device, kAudioDevicePropertyTransportType) ==
         kAudioDeviceTransportTypeVirtual) != requireVirtual)
        throw std::runtime_error("Expected an alive 48 kHz physical/virtual input pair");
    return device;
}

class PeriodLease {
  public:
    PeriodLease(AudioDeviceID device, UInt32 requested)
        : device_(device), original_(get<UInt32>(device, kAudioDevicePropertyBufferFrameSize)),
          requested_(requested) {
        const auto address = property(kAudioDevicePropertyBufferFrameSize);
        check(AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(requested_),
                                         &requested_),
              "Set recording client period");
        changed_ = true;
        try {
            if (get<UInt32>(device_, kAudioDevicePropertyBufferFrameSize) != requested_)
                throw std::runtime_error("Recording client period was not applied");
        } catch (...) {
            restore();
            throw;
        }
    }
    ~PeriodLease() {
        try {
            restore();
        } catch (...) {
            std::fputs("Recording client period restoration failed\n", stderr);
        }
    }
    void restore() {
        if (!changed_)
            return;
        const auto current = get<UInt32>(device_, kAudioDevicePropertyBufferFrameSize);
        if (current != requested_ && current != original_)
            throw std::runtime_error("External buffer change prevents restoration");
        if (current != original_) {
            const auto address = property(kAudioDevicePropertyBufferFrameSize);
            check(AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(original_),
                                             &original_),
                  "Restore recording client period");
        }
        if (get<UInt32>(device_, kAudioDevicePropertyBufferFrameSize) != original_)
            throw std::runtime_error("Recording client period restoration was not applied");
        changed_ = false;
    }

  private:
    AudioDeviceID device_;
    UInt32 original_, requested_;
    bool changed_{false};
};

class Capture {
  public:
    std::vector<float> samples;
    std::vector<loopz::probe::RecordingBlock> blocks;
    uint64_t position{0}, blockCount{0}, failures{0}, timestampErrors{0}, invalidBlocks{0};
    uint64_t nonfinite{0}, zeroFrames{0};
    uint64_t silentBlocks{0}, maximumZeroRun{0}, zeroRun{0};
    std::atomic<bool> done{false};

    explicit Capture(uint32_t seconds)
        : samples(size_t(kRate * seconds)), blocks(samples.size() / 14 + 1024) {}

    void open(AudioDeviceID device, SInt32 channel, bool normalizeDisabledOutput = false) {
        AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                              kAudioUnitManufacturer_Apple, 0, 0};
        const auto component = AudioComponentFindNext(nullptr, &description);
        if (!component)
            throw std::runtime_error("AUHAL is unavailable");
        check(AudioComponentInstanceNew(component, &unit_), "Create input unit");
        UInt32 yes = 1, no = 0;
        set(kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, yes);
        set(kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, no);
        set(kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, device);
        if (normalizeDisabledOutput) {
            // AUHAL's global latency can include a converter on its disabled
            // output bus, whose default client format is 44.1 kHz. Match only
            // that private client format to hardware; playback stays disabled.
            const auto hardwareOutput = unitFormat(kAudioUnitScope_Output, 0);
            if (hardwareOutput.mSampleRate == kRate && hardwareOutput.mChannelsPerFrame &&
                hardwareOutput.mFormatID == kAudioFormatLinearPCM) {
                auto clientOutput = hardwareOutput;
                set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, clientOutput);
                disabledOutputNormalized_ = true;
            }
        }
        UInt32 maximum = kMaximumBlock;
        set(kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, maximum);
        AudioStreamBasicDescription format{kRate,
                                           kAudioFormatLinearPCM,
                                           UInt32(kAudioFormatFlagsNativeFloatPacked) |
                                               UInt32(kAudioFormatFlagIsNonInterleaved),
                                           sizeof(float),
                                           1,
                                           sizeof(float),
                                           1,
                                           32,
                                           0};
        set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, format);
        set(kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1, channel);
        AURenderCallbackStruct callback{render, this};
        set(kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, callback);
        check(AudioUnitInitialize(unit_), "Initialize input unit");
        if (unitFormat(kAudioUnitScope_Input, 1).mSampleRate != kRate ||
            unitFormat(kAudioUnitScope_Output, 1).mSampleRate != kRate)
            throw std::runtime_error("Unexpected reference input conversion rate");
    }
    bool disabledOutputNormalized() const noexcept {
        return disabledOutputNormalized_;
    }
    void start() {
        callbacks_.open();
        check(AudioOutputUnitStart(unit_), "Start input unit");
    }
    double unitLatencySeconds() const {
        Float64 latency = 0;
        UInt32 bytes = sizeof(latency);
        check(AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
                                   &latency, &bytes),
              "Read reference Audio Unit latency");
        if (bytes != sizeof(latency) || !std::isfinite(latency) || latency < 0)
            throw std::runtime_error("Invalid reference Audio Unit latency");
        return latency;
    }
    void stop() noexcept {
        callbacks_.beginClose();
        if (unit_)
            AudioOutputUnitStop(unit_);
        while (callbacks_.inFlight())
            std::this_thread::yield();
    }
    ~Capture() {
        if (unit_) {
            stop();
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
        }
    }
    std::span<const loopz::probe::RecordingBlock> observations() const {
        return {blocks.data(), size_t(blockCount)};
    }
    NSDictionary* summary() const {
        uint32_t minimum = UINT32_MAX, maximum = 0;
        double firstAge = INFINITY, lastAge = -INFINITY;
        for (const auto& block : observations()) {
            if (block.firstFrame < uint64_t(kRate))
                continue;
            minimum = std::min(minimum, block.frames);
            maximum = std::max(maximum, block.frames);
            const auto age = double(block.receivedNs) - block.acquiredNs;
            firstAge = std::min(firstAge, age);
            lastAge = std::max(lastAge, age);
        }
        return @{
            @"frames" : @(position),
            @"callbacks" : @(blockCount),
            @"failures" : @(failures),
            @"timestampErrors" : @(timestampErrors),
            @"invalidBlocks" : @(invalidBlocks),
            @"nonfiniteSamples" : @(nonfinite),
            @"zeroFramesAfterWarmup" : @(zeroFrames),
            @"silentBlocksAfterWarmup" : @(silentBlocks),
            @"maximumZeroRunAfterWarmup" : @(maximumZeroRun),
            @"callbackFrames" : @[ @(minimum), @(maximum) ],
            @"declaredFirstSampleReceiptAgeMs" : @[ @(firstAge / 1e6), @(lastAge / 1e6) ]
        };
    }

  private:
    AudioUnit unit_{nullptr};
    std::array<float, kMaximumBlock> scratch_{};
    singz::AudioInputCallbackGate callbacks_;
    double expected_{0};
    bool previous_{false};
    bool disabledOutputNormalized_{false};
    AudioStreamBasicDescription unitFormat(AudioUnitScope scope, AudioUnitElement element) const {
        AudioStreamBasicDescription result{};
        UInt32 bytes = sizeof(result);
        check(AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat, scope, element, &result,
                                   &bytes),
              "Read input unit format");
        if (bytes != sizeof(result))
            throw std::runtime_error("Unexpected input unit format size");
        return result;
    }
    template <class T>
    void set(AudioUnitPropertyID key, AudioUnitScope scope, AudioUnitElement element, T& value) {
        check(AudioUnitSetProperty(unit_, key, scope, element, &value, sizeof(value)),
              "Configure input unit");
    }
    static OSStatus render(void* context, AudioUnitRenderActionFlags* flags,
                           const AudioTimeStamp* time, UInt32 bus, UInt32 frames,
                           AudioBufferList*) {
        auto& self = *static_cast<Capture*>(context);
        singz::AudioInputCallbackScope admission(self.callbacks_);
        if (!admission || self.done.load(std::memory_order_acquire))
            return noErr;
        if (!frames || frames > self.scratch_.size() || self.blockCount == self.blocks.size()) {
            ++self.failures;
            self.done.store(true, std::memory_order_release);
            return noErr;
        }
        AudioBufferList list{1, {{1, UInt32(frames * sizeof(float)), self.scratch_.data()}}};
        const auto status = AudioUnitRender(self.unit_, flags, time, bus, frames, &list);
        const auto received = nanoseconds(mach_absolute_time());
        if (status) {
            ++self.failures;
            return status;
        }
        const bool valid = time && (time->mFlags & kAudioTimeStampHostTimeValid) &&
                           (time->mFlags & kAudioTimeStampSampleTimeValid) &&
                           std::isfinite(time->mSampleTime);
        if (self.position >= uint64_t(kRate / 2)) {
            if (!valid)
                ++self.invalidBlocks;
            else if (self.previous_ && std::abs(time->mSampleTime - self.expected_) > 0.01)
                ++self.timestampErrors;
        }
        self.previous_ = valid;
        self.expected_ = valid ? time->mSampleTime + frames : 0;
        const auto count = std::min<uint64_t>(frames, self.samples.size() - self.position);
        self.blocks[self.blockCount++] = {self.position, valid ? nanoseconds(time->mHostTime) : 0,
                                          received, uint32_t(count)};
        bool allZero = true;
        for (uint64_t i = 0; i < count; ++i) {
            const auto sample = self.scratch_[i];
            if (!std::isfinite(sample))
                ++self.nonfinite;
            allZero &= sample == 0;
            if (self.position + i >= uint64_t(kRate)) {
                if (sample == 0) {
                    ++self.zeroFrames;
                    self.maximumZeroRun = std::max(self.maximumZeroRun, ++self.zeroRun);
                } else {
                    self.zeroRun = 0;
                }
            }
            self.samples[self.position + i] = std::isfinite(sample) ? sample : 0;
        }
        if (self.position >= uint64_t(kRate) && allZero)
            ++self.silentBlocks;
        self.position += count;
        if (self.position == self.samples.size())
            self.done.store(true, std::memory_order_release);
        return noErr;
    }
};

NSDictionary* analyze(const Capture& reference, const Capture& recording, uint32_t period,
                      uint32_t seconds, InputHardwareLatency hardware, double unitLatencySeconds) {
    const auto referenceBand = loopz::probe::matchingBand(reference.samples);
    const auto recordingBand = loopz::probe::matchingBand(recording.samples);
    const auto center = int(std::llround(
        (double(reference.blocks[0].receivedNs) - recording.blocks[0].receivedNs) * kRate / 1e9));
    auto* windows = [NSMutableArray array];
    bool passed = true;
    const auto positions =
        seconds == 6
            ? std::array<int, 4>{1, 2, 3, 4}
            : std::array<int, 4>{1, int(seconds / 3), int(seconds * 2 / 3), int(seconds - 2)};
    for (int second : positions) {
        const int first = int(kRate * second), count = int(kRate / 2);
        const auto match = loopz::probe::matchWaveforms(referenceBand, recordingBand, first, count,
                                                        center, int(kRate / 4));
        std::vector<double> ages;
        for (const auto& block : recording.observations()) {
            const auto referenceFirst = int64_t(block.firstFrame) - match.lagFrames;
            if (referenceFirst < first || referenceFirst + block.frames > first + count)
                continue;
            for (uint32_t offset = 0; offset < block.frames; ++offset)
                ages.push_back(loopz::probe::deliveredAgeNs(block, offset, match.lagFrames,
                                                            reference.observations(), kRate));
        }
        std::sort(ages.begin(), ages.end());
        if (ages.empty())
            throw std::runtime_error("No matched recording blocks");
        const auto quantile = [&](double fraction) {
            return ages[size_t(fraction * (ages.size() - 1))] / 1e6;
        };
        const bool matched = match.correlation > 0.9 && ages.front() >= 0;
        passed &= matched;
        NSDictionary* delivery = @{
            @"minimum" : @(quantile(0)),
            @"median" : @(quantile(0.5)),
            @"p95" : @(quantile(0.95)),
            @"maximum" : @(quantile(1))
        };
        const auto corrected = [&](double fraction) {
            return loopz::probe::hardwareAdjustedDeliveryAgeNs(quantile(fraction) * 1e6,
                                                               hardware.deviceFrames,
                                                               hardware.streamFrames, kRate) /
                   1e6;
        };
        NSDictionary* hardwareDelivery = @{
            @"minimum" : @(corrected(0)),
            @"median" : @(corrected(.5)),
            @"p95" : @(corrected(.95)),
            @"maximum" : @(corrected(1))
        };
        [windows addObject:@{
            @"referenceStartSeconds" : @(second),
            @"windowSeconds" : @0.5,
            @"indexLagFrames" : @(match.lagFrames),
            @"correlation" : @(match.correlation),
            @"signalMatched" : @(matched),
            @"deliveredSampleAgeMs" : matched ? (id)delivery : (id)NSNull.null,
            @"driverTimestampToDeliveryMs" : matched ? (id)delivery : (id)NSNull.null,
            @"hardwareAdjustedDeliveryAgeMs" : matched && unitLatencySeconds == 0
                ? (id)hardwareDelivery
                : (id)NSNull.null
        }];
    }
    passed &= !reference.failures && !reference.timestampErrors && !reference.invalidBlocks &&
              !reference.nonfinite && !recording.failures && !recording.timestampErrors &&
              !recording.invalidBlocks && !recording.nonfinite && !recording.silentBlocks;
    return @{
        @"passed" : @(passed),
        @"sampleRate" : @(kRate),
        @"observationSeconds" : @(seconds),
        @"waveformMatchingLowpassHz" : @(kRate / 8),
        @"requestedClientFrames" : @(period),
        @"reference" : reference.summary(),
        @"recording" : recording.summary(),
        @"windows" : windows,
        @"hardwareLatency" : @{
            @"deviceFrames" : @(hardware.deviceFrames),
            @"selectedStreamFrames" : @(hardware.streamFrames),
            @"selectedStream" : @(hardware.stream),
            @"safetyOffsetFrames" : @(hardware.safetyFrames),
            @"addedDelayMs" : @(loopz::probe::hardwareAdjustedDeliveryAgeNs(
                                    0, hardware.deviceFrames, hardware.streamFrames, kRate) /
                                1e6),
            @"safetyAndBufferAddedAgain" : @NO,
            @"referenceAudioUnitLatencySeconds" : @(unitLatencySeconds),
            @"referenceDisabledOutputFormatMatched" : @(reference.disabledOutputNormalized()),
            @"estimateValid" : @(unitLatencySeconds == 0)
        },
        @"audioSaved" : @NO,
        @"physicalOutputOpened" : @NO,
        @"permissionRequested" : @NO,
        @"method" :
            @"Passive microphone waveform correspondence; actual recording callback "
            @"receipt minus physical reference driver timestamp. Hardware-adjusted age "
            @"also adds device plus selected-stream input latency. Safety offsets and "
            @"IO periods are not added again. Nonzero reference AU latency requires review. "
            @"Hardware-adjusted values depend on the driver's reports, not analog calibration."
    };
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if (argc != 7 && argc != 8)
                throw std::invalid_argument("usage: probe physical-uid channel virtual-uid "
                                            "channel client-frames report.json [seconds:6..90]");
            if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] !=
                AVAuthorizationStatusAuthorized)
                throw std::runtime_error("Existing microphone authorization is required");
            const auto parse = [](const char* text, int maximum) {
                size_t end = 0;
                const auto value = std::stoi(text, &end);
                if (text[end] || value < 1 || value > maximum)
                    throw std::invalid_argument("Invalid probe option");
                return value;
            };
            const auto period = uint32_t(parse(argv[5], 512));
            const auto seconds = uint32_t(argc == 8 ? parse(argv[7], 90) : 6);
            if (seconds < 6)
                throw std::invalid_argument("Observation must last 6 to 90 seconds");
            if (period != 32 && period != 64 && period != 128 && period != 512)
                throw std::invalid_argument("Client frames must be 32, 64, 128 or 512");
            const auto physical = findDevice(argv[1], false),
                       virtualDevice = findDevice(argv[3], true);
            mach_timebase_info(&timebase);
            std::signal(SIGINT, cancel);
            std::signal(SIGTERM, cancel);
            PeriodLease physicalPeriod(physical, period), virtualPeriod(virtualDevice, period);
            Capture reference(seconds), recording(seconds);
            const auto physicalChannel = parse(argv[2], 64) - 1;
            reference.open(physical, physicalChannel, true);
            recording.open(virtualDevice, parse(argv[4], 64) - 1);
            const auto hardware = inputHardwareLatency(physical, physicalChannel);
            const auto unitLatency = reference.unitLatencySeconds();
            reference.start();
            recording.start();
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 4);
            while (!canceled && (!reference.done.load() || !recording.done.load()) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            reference.stop();
            recording.stop();
            if (inputHardwareLatency(physical, physicalChannel) != hardware ||
                reference.unitLatencySeconds() != unitLatency)
                throw std::runtime_error("Input latency changed during capture");
            virtualPeriod.restore();
            physicalPeriod.restore();
            if (canceled || reference.position != reference.samples.size() ||
                recording.position != recording.samples.size())
                throw std::runtime_error("Bounded recording did not finish");
            NSMutableDictionary* result =
                [analyze(reference, recording, period, seconds, hardware, unitLatency) mutableCopy];
            result[@"clientPeriodsRestored"] = @YES;
            NSData* data = [NSJSONSerialization dataWithJSONObject:result
                                                           options:NSJSONWritingPrettyPrinted
                                                             error:nil];
            if (!data || ![data writeToFile:@(argv[6]) atomically:YES])
                throw std::runtime_error("Cannot save recording latency report");
            std::puts([result[@"passed"] boolValue] ? "Recording path passed"
                                                    : "Recording path failed validation");
            return [result[@"passed"] boolValue] ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
