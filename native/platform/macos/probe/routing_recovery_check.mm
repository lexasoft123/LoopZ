#include "routing_recovery_check.hpp"
#include "routing_io_support.hpp"
#include "recovery_clock_lease.hpp"
#include <loopz/probe_recovery.hpp>
#include <mach/mach_time.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

namespace loopz::macos {
namespace {
using namespace diagnostic;
using Phase = probe::RecoveryPhase;
using Clock = std::chrono::steady_clock;

void awaitCommand(const std::string& expected, std::chrono::seconds timeout,
                  const std::function<void()>& check) {
    const auto until = Clock::now() + timeout;
    std::string value;
    while (Clock::now() < until) {
        check();
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 20);
        if (ready < 0)
            throw std::runtime_error("Recovery control polling failed");
        if (!ready)
            continue;
        char byte;
        if (::read(STDIN_FILENO, &byte, 1) != 1)
            throw std::runtime_error("Recovery controller closed before completion");
        if (byte == '\n') {
            if (value == expected)
                return;
            throw std::invalid_argument("Unexpected recovery control command");
        }
        if (value.size() >= 32)
            throw std::invalid_argument("Recovery control command is too long");
        value += byte;
    }
    throw std::runtime_error("Recovery control deadline expired");
}

void emit(NSDictionary* report) {
    NSError* error = nil;
    const auto data = [NSJSONSerialization dataWithJSONObject:report options:0 error:&error];
    if (!data)
        throw std::runtime_error("Recovery result could not be serialized");
    if (std::fwrite(data.bytes, 1, data.length, stdout) != data.length ||
        std::putchar('\n') == EOF || std::fflush(stdout))
        throw std::runtime_error("Recovery observer disconnected");
}

template <typename T> T property(AudioObjectID object, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    T value{};
    UInt32 bytes = sizeof(value);
    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, &value) ||
        bytes != sizeof(value))
        throw std::runtime_error("Recovery device property is unavailable");
    return value;
}
std::vector<AudioObjectID> streams(AudioObjectID object, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreams, scope,
                                       kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &bytes) ||
        bytes > 128 * sizeof(AudioObjectID) || bytes % sizeof(AudioObjectID))
        throw std::runtime_error("Recovery stream catalog is unavailable");
    std::vector<AudioObjectID> values(bytes / sizeof(AudioObjectID));
    const auto expected = bytes;
    if (bytes && (AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, values.data()) ||
                  bytes != expected))
        throw std::runtime_error("Recovery stream catalog changed while reading");
    return values;
}

struct DeviceClock {
    std::string uid;
    AudioObjectID object;
    std::vector<AudioObjectID> inputs, outputs;
    mach_timebase_info_data_t timebase{};
    long double origin{0};
    std::unique_ptr<RecoveryClockLease> lease;
    explicit DeviceClock(std::string id)
        : uid(std::move(id)), object(requireRoutingDevice(uid)),
          inputs(streams(object, kAudioObjectPropertyScopeInput)),
          outputs(streams(object, kAudioObjectPropertyScopeOutput)) {
        if (mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.denom)
            throw std::runtime_error("Recovery clock conversion is unavailable");
        lease = std::make_unique<RecoveryClockLease>(
            RecoveryClockOperations{[selected = uid, expected = object] {
                                        return requireRoutingDevice(selected) == expected;
                                    },
                                    [expected = object] {
                                        return AudioDeviceStart(expected, nullptr);
                                    },
                                    [expected = object] {
                                        return AudioDeviceStop(expected, nullptr);
                                    }});
        origin = clockOrigin(std::chrono::seconds(2));
    }
    long double clockOrigin(std::chrono::milliseconds startupAllowance = {}) const {
        const auto deadline = Clock::now() + startupAllowance;
        for (;;) {
            AudioTimeStamp time{};
            time.mFlags = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
            const auto status = AudioDeviceGetCurrentTime(object, &time);
            // A running AUHAL callback can precede the first public zero-stamp
            // clock update. Permit only this specific startup status to settle.
            // Post-baseline observations have no allowance and fail immediately.
            if (status == kAudioHardwareNotRunningError && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (status ||
                (time.mFlags & (kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid)) !=
                    (kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid) ||
                !std::isfinite(time.mSampleTime))
                throw std::runtime_error(
                    "The existing client lost its device clock (phase " +
                    std::string(startupAllowance.count() ? "startup" : "live") + ", status " +
                    std::to_string(status) + ", flags " + std::to_string(time.mFlags) +
                    ", object " + std::to_string(object) + ", running " +
                    std::to_string(property<UInt32>(object, kAudioDevicePropertyDeviceIsRunning)) +
                    ", running somewhere " +
                    std::to_string(
                        property<UInt32>(object, kAudioDevicePropertyDeviceIsRunningSomewhere)) +
                    ", uid " + uid + ")");
            return static_cast<long double>(time.mHostTime) * timebase.numer / timebase.denom -
                   static_cast<long double>(time.mSampleTime) * 1e9L / 48000;
        }
    }
    void check() const {
        if (requireRoutingDevice(uid) != object ||
            property<UInt32>(object, kAudioDevicePropertyDeviceIsAlive) != 1 ||
            property<Float64>(object, kAudioDevicePropertyNominalSampleRate) != 48000 ||
            streams(object, kAudioObjectPropertyScopeInput) != inputs ||
            streams(object, kAudioObjectPropertyScopeOutput) != outputs)
            throw std::runtime_error("Recovery replaced the device or its public streams");
        if (std::abs(clockOrigin() - origin) > 2e9L / 48000)
            throw std::runtime_error("Recovery reset the existing client's clock origin");
    }
};

void waitForCallbacks(singz::AudioHost& input, singz::AudioHost& output) {
    const auto began = Clock::now();
    const auto deadline = began + std::chrono::seconds(2);
    const auto warmupEnd = began + std::chrono::seconds(1);
    for (;;) {
        const auto captured = input.status(), written = output.status();
        if (captured.state != singz::AudioHostState::Running ||
            written.state != singz::AudioHostState::Running || captured.renderFailures ||
            written.renderFailures || captured.invalidCallbacks || written.invalidCallbacks)
            throw std::runtime_error("Recovery clients could not start healthy IO");
        if (captured.callbacks && written.callbacks && Clock::now() >= warmupEnd)
            return;
        if (Clock::now() >= deadline)
            throw std::runtime_error("Recovery clients did not start callbacks within two seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct Capture {
    uint32_t channels;
    probe::RecoveryCapture samples;
    explicit Capture(uint32_t count) : channels(count), samples(count, 48000) {}
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Capture*>(context);
        if (!block.input || !block.output || block.inputChannels != self.channels ||
            block.outputChannels != self.channels || block.sampleRate != 48000)
            return false;
        for (uint32_t channel = 0; channel < self.channels; ++channel)
            std::fill_n(block.output[channel], block.frames, 0);
        self.samples.append(block.input, block.frames, block.inputSampleHostTimeNs,
                            block.inputTimestampValid && block.inputTimestampHardware);
        return true;
    }
};

class Observer {
  public:
    Observer(singz::AudioHost& input, singz::AudioHost& output, const std::string& readUID,
             const std::string& writeUID)
        : input_(input), output_(output), inputGeneration_(input.status().streamGeneration),
          outputGeneration_(output.status().streamGeneration) {
        devices_.push_back(std::make_unique<DeviceClock>(readUID));
        if (writeUID != readUID)
            devices_.push_back(std::make_unique<DeviceClock>(writeUID));
    }
    size_t clockLeases() const noexcept {
        return devices_.size();
    }
    bool releaseClocks() noexcept {
        bool released = true;
        for (auto& device : devices_)
            released = device->lease->close() && released;
        return released;
    }
    void check() {
        for (const auto& device : devices_)
            device->check();
        const auto input = input_.status(), output = output_.status();
        if (input.state != singz::AudioHostState::Running ||
            output.state != singz::AudioHostState::Running ||
            input.streamGeneration != inputGeneration_ ||
            output.streamGeneration != outputGeneration_ || input.renderFailures ||
            output.renderFailures || input.invalidCallbacks || output.invalidCallbacks)
            throw std::runtime_error("An existing recovery client stopped or changed its stream");
        const auto now = Clock::now();
        for (size_t index = 0; index < progress_.size(); ++index) {
            const auto callbacks = index == 0 ? input.callbacks : output.callbacks;
            auto& progress = progress_[index];
            if (callbacks != progress.callbacks) {
                progress.callbacks = callbacks;
                progress.time = now;
            } else if (now - progress.time > std::chrono::milliseconds(500)) {
                throw std::runtime_error("An existing recovery client stalled for over 500 ms");
            }
        }
    }
    void pause(std::chrono::milliseconds duration) {
        const auto until = Clock::now() + duration;
        do {
            check();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (Clock::now() < until);
    }
    void observed(probe::RecoveryCapture& capture, Phase phase) {
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (capture.observed() != phase) {
            if (Clock::now() >= until)
                throw std::runtime_error("The existing capture client stopped producing callbacks");
            pause(std::chrono::milliseconds(20));
        }
    }
    void command(const std::string& expected, std::chrono::seconds timeout) {
        awaitCommand(expected, timeout, [this] {
            check();
        });
    }

  private:
    singz::AudioHost& input_;
    singz::AudioHost& output_;
    std::vector<std::unique_ptr<DeviceClock>> devices_;
    uint64_t inputGeneration_, outputGeneration_;
    struct Progress {
        uint64_t callbacks{};
        Clock::time_point time{Clock::now()};
    };
    std::array<Progress, 2> progress_;
};

NSDictionary* summarize(const probe::RecoverySegment& value, uint32_t channels, bool signal) {
    NSMutableDictionary* result = [@{
        @"frames" : @(value.frames),
        @"points" : @(value.count),
        @"zeroSamples" : @(value.zeroSamples),
        @"zeroFrames" : @(value.zeroFrames),
        @"longestSilentMs" : @(value.longestZeroRun * 1000.0 / 48000),
        @"timestampErrors" : @(value.timestampErrors),
        @"nonfiniteSamples" : @(value.nonfinite),
        @"pointOverflows" : @(value.overflows)
    } mutableCopy];
    if (signal && value.count >= 500) {
        const auto analysis = probe::checkChannels({value.points.data(), value.count}, channels);
        result[@"timestampMarkerDelayMs"] = @(analysis.delayMs);
        result[@"minimumCorrelation"] = @(analysis.minimumCorrelation);
        result[@"maximumCrossCorrelation"] = @(analysis.maximumCrossCorrelation);
        result[@"minimumGain"] = @(analysis.minimumGain);
        result[@"maximumGain"] = @(analysis.maximumGain);
    }
    return result;
}
} // namespace

int routingRecoveryCheck(const std::string& writeUID, const std::string& readUID, uint32_t channels,
                         uint32_t bufferFrames, bool coordinatedBaseline) {
    Writer writer{channels};
    Capture reader(channels);
    singz::AudioHost output, input;
    const auto written =
        output.open(config(writeUID, channels, false, bufferFrames), Writer::render, &writer);
    requireOK(written);
    const auto captured =
        input.open(config(readUID, channels, true, bufferFrames), Capture::render, &reader);
    requireOK(captured);
    if (written.format.sampleRate != 48000 || captured.format.sampleRate != 48000)
        throw std::invalid_argument("Continuous recovery checks currently require 48000 Hz");
    requireOK(output.start());
    requireOK(input.start());
    // AUHAL can briefly stop and restart while its duplex clients settle.
    // Establish clock identity after the warm-up, before any measured phase.
    // Observations after this boundary permit no clock restart or outage.
    waitForCallbacks(input, output);
    if (coordinatedBaseline) {
        emit(@{@"event" : @"prepared", @"protocol" : @"stdio-v2"});
        // Keep the exact IO hosts open while other clients negotiate periods.
        // Strict clock/stream identity starts only after the full graph settles.
        awaitCommand("begin-baseline", std::chrono::seconds(20), [&] {
            const auto captured = input.status(), written = output.status();
            if (captured.state != singz::AudioHostState::Running ||
                written.state != singz::AudioHostState::Running || captured.renderFailures ||
                written.renderFailures || captured.invalidCallbacks || written.invalidCallbacks)
                throw std::runtime_error("Prepared recovery clients did not remain healthy");
        });
    }
    Observer observer(input, output, readUID, writeUID);
    reader.samples.advance(Phase::Baseline);
    observer.observed(reader.samples, Phase::Baseline);
    observer.pause(std::chrono::seconds(2));
    emit(@{
        @"event" : @"ready",
        @"protocol" : coordinatedBaseline ? @"stdio-v2" : @"stdio-v1",
        @"timingServiceLeases" : @(observer.clockLeases())
    });
    observer.command("begin-disconnect", std::chrono::seconds(5));
    const auto recoveryDeadline = Clock::now() + std::chrono::seconds(25);
    reader.samples.advance(Phase::Disconnected);
    observer.observed(reader.samples, Phase::Disconnected);
    if (!probe::recoveredSignal(reader.samples.segment(Phase::Baseline), channels)) {
        emit(@{
            @"event" : @"baseline-failed",
            @"measurement" : summarize(reader.samples.segment(Phase::Baseline), channels, true)
        });
        throw std::runtime_error("Continuous clients did not produce a clean baseline");
    }
    emit(@{@"event" : @"disconnect-ready", @"baselinePassed" : @YES});
    observer.command("restored", std::chrono::seconds(25));
    // The controller can observe the new process while retained public devices
    // still await mapping recovery. Require a full second of finite, nonzero
    // audio on every channel before the independent three-second signal check.
    const auto restartObserved = Clock::now();
    while (reader.samples.continuousSignalFrames() < 48000) {
        if (Clock::now() >= recoveryDeadline)
            throw std::runtime_error("Audio did not recover within the 25-second disconnect bound");
        observer.pause(std::chrono::milliseconds(20));
    }
    const auto recoveryWaitMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - restartObserved)
            .count();
    reader.samples.advance(Phase::Recovered);
    observer.observed(reader.samples, Phase::Recovered);
    observer.pause(std::chrono::seconds(3));
    observer.check();
    // This is the end of observation. SingZ closes callback admission before
    // stopping AUHAL, so an already queued callback can increment its invalid
    // counter during deliberate shutdown. Report that delta separately from
    // the strict counters observed while clients must remain running.
    const auto activeInput = input.status(), activeOutput = output.status();
    input.stop();
    output.stop();
    const bool clocksReleased = observer.releaseClocks();
    const auto& before = reader.samples.segment(Phase::Baseline);
    const auto& fault = reader.samples.segment(Phase::Disconnected);
    const auto& after = reader.samples.segment(Phase::Recovered);
    const auto inputStatus = input.status(), outputStatus = output.status();
    const bool passed = clocksReleased && probe::recoveredSignal(before, channels) &&
                        probe::recoveredSignal(after, channels) && !fault.timestampErrors &&
                        !fault.nonfinite && !fault.overflows && !writer.timestampErrors &&
                        !inputStatus.renderFailures && !outputStatus.renderFailures &&
                        !activeInput.invalidCallbacks && !activeOutput.invalidCallbacks &&
                        inputStatus.state == singz::AudioHostState::Stopped &&
                        outputStatus.state == singz::AudioHostState::Stopped;
    emit(@{
        @"event" : @"result",
        @"passed" : @(passed),
        @"channels" : @(channels),
        @"sampleRate" : @48000,
        @"clientsReopened" : @NO,
        @"deviceAndStreamIDsPreserved" : @YES,
        @"clockOriginPreserved" : @YES,
        @"timingServiceLeases" : @(observer.clockLeases()),
        @"timingServicesReleased" : @(clocksReleased),
        @"requiredContinuousSignalMs" : @1000,
        @"recoveryWaitAfterRestartObservedMs" : @(recoveryWaitMs),
        @"before" : summarize(before, channels, true),
        @"interruption" : summarize(fault, channels, false),
        @"after" : summarize(after, channels, true),
        @"writerTimestampErrors" : @(writer.timestampErrors),
        @"inputRenderFailures" : @(inputStatus.renderFailures),
        @"inputInvalidCallbacks" : @(activeInput.invalidCallbacks),
        @"outputInvalidCallbacks" : @(activeOutput.invalidCallbacks),
        @"inputShutdownCallbackDelta" :
            @(inputStatus.invalidCallbacks - activeInput.invalidCallbacks),
        @"outputShutdownCallbackDelta" :
            @(outputStatus.invalidCallbacks - activeOutput.invalidCallbacks),
        @"outputRenderFailures" : @(outputStatus.renderFailures),
        @"permissionRequested" : @NO,
        @"physicalDevicesOpened" : @NO
    });
    return passed ? 0 : 1;
}
} // namespace loopz::macos
