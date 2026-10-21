#include "routing_io_check.hpp"
#include "routing_io_support.hpp"
#include "routing_recovery_check.hpp"
#include <loopz/macos/permissions.hpp>
#include <loopz/probe_channels.hpp>
#include <zcore/device/audio_host.h>
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <optional>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace loopz::macos {
namespace {
using namespace diagnostic;
struct Reader {
    struct TimingAnomaly {
        uint64_t capturedFrame, hostNs, receivedNs, sourceFrame, sequence;
        uint32_t discontinuity;
        bool valid, hardware;
    };
    uint32_t channels;
    bool coordinated{false};
    std::atomic<bool> measuring{true};
    uint64_t frames{0}, timestampErrors{0}, nonfinite{0}, overflow{0}, previousPointNs{0};
    uint64_t zeroSamples{0}, firstZeroFrame{UINT64_MAX}, lastZeroFrame{0};
    probe::CallbackCadence cadence{};
    std::optional<TimingAnomaly> firstCadenceAnomaly{};
    size_t count{0};
    std::vector<probe::ChannelPoint> points{};
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Reader*>(context);
        if (!block.input || !block.output || block.inputChannels != self.channels ||
            block.outputChannels != self.channels)
            return false;
        for (uint32_t channel = 0; channel < block.outputChannels; ++channel)
            std::fill_n(block.output[channel], block.frames, 0);
        if (!self.measuring.load(std::memory_order_acquire))
            return true;
        const auto step = static_cast<uint64_t>(std::max(1.0, std::round(block.sampleRate / 1000)));
        const bool warm =
            self.coordinated || self.frames >= static_cast<uint64_t>(block.sampleRate);
        if (warm &&
            !self.cadence.observe(block.inputSampleHostTimeNs, block.inputSourceFrame,
                                  block.callbackSequence, block.frames, block.sampleRate,
                                  block.inputTimestampValid && block.inputTimestampHardware)) {
            ++self.timestampErrors;
            if (!self.firstCadenceAnomaly)
                self.firstCadenceAnomaly = TimingAnomaly{self.frames,
                                                         block.inputSampleHostTimeNs,
                                                         block.callbackHostTimeNs,
                                                         block.inputSourceFrame,
                                                         block.callbackSequence,
                                                         block.discontinuity,
                                                         block.inputTimestampValid,
                                                         block.inputTimestampHardware};
        }
        for (uint32_t frame = 0; frame < block.frames; ++frame) {
            if (!warm)
                continue;
            for (uint32_t channel = 0; channel < self.channels; ++channel) {
                const auto sample = block.input[channel][frame];
                if (!std::isfinite(sample))
                    ++self.nonfinite;
                else if (sample == 0) {
                    ++self.zeroSamples;
                    self.firstZeroFrame = std::min(self.firstZeroFrame, self.frames + frame);
                    self.lastZeroFrame = self.frames + frame;
                }
            }
            if ((self.frames + frame) % step)
                continue;
            if (self.count >= self.points.size()) {
                ++self.overflow;
                continue;
            }
            auto& point = self.points[self.count++];
            point.hostNs = block.inputSampleHostTimeNs +
                           static_cast<uint64_t>((frame + 0.5) * 1e9 / block.sampleRate);
            if (self.previousPointNs && point.hostNs <= self.previousPointNs)
                ++self.timestampErrors;
            self.previousPointNs = point.hostNs;
            for (uint32_t channel = 0; channel < self.channels; ++channel) {
                const auto sample = block.input[channel][frame];
                point.samples[channel] = sample;
            }
        }
        self.frames += block.frames;
        return true;
    }
};
void emitEvent(const char* event) {
    if (std::printf("{\"event\":\"%s\",\"protocol\":\"stdio-v1\"}\n", event) < 0 ||
        std::fflush(stdout))
        throw std::runtime_error("Capture controller disconnected");
}
void awaitCaptureCommand(const std::string& expected,
                         std::chrono::seconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string command;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&input, 1, 20);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Capture controller polling failed");
        }
        if (!ready)
            continue;
        char byte;
        if (::read(STDIN_FILENO, &byte, 1) != 1)
            throw std::runtime_error("Capture controller closed before completion");
        if (byte == '\n') {
            if (command == expected)
                return;
            throw std::invalid_argument("Unexpected capture control command");
        }
        if (command.size() >= 32)
            throw std::invalid_argument("Capture control command is too long");
        command += byte;
    }
    throw std::runtime_error("Capture control deadline expired");
}
int run(int argc, char** argv) {
    std::string writeUID, readUID;
    uint32_t channels = 2, seconds = 5, bufferFrames = 0, preparationTimeout = 20;
    double expectedGain = 1;
    bool recovery = false, explicitSeconds = false, coordinatedBaseline = false;
    bool coordinatedCapture = false;
    bool startupEvents = false, explicitPreparationTimeout = false;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc)
            throw std::invalid_argument("IO check arguments need values");
        const std::string key = argv[index], value = argv[index + 1];
        if (key == "--write-device")
            writeUID = value;
        else if (key == "--read-device")
            readUID = value;
        else if (key == "--recovery-protocol") {
            if ((value != "stdio-v1" && value != "stdio-v2") || recovery)
                throw std::invalid_argument(
                    "Recovery protocol must be stdio-v1 or stdio-v2 and appear once");
            recovery = true;
            coordinatedBaseline = value == "stdio-v2";
        } else if (key == "--capture-protocol") {
            if (value != "stdio-v1" || coordinatedCapture)
                throw std::invalid_argument("Capture protocol must be stdio-v1 and appear once");
            coordinatedCapture = true;
        } else if (key == "--startup-events") {
            if (value != "stdio-v1" || startupEvents)
                throw std::invalid_argument("Startup events must be stdio-v1 and appear once");
            startupEvents = true;
        } else if (key == "--prepare-timeout-seconds") {
            size_t end = 0;
            const auto number = std::stoul(value, &end);
            if (explicitPreparationTimeout || end != value.size() || number < 20 || number > 120)
                throw std::invalid_argument(
                    "Preparation timeout must be 20–120 seconds and appear once");
            preparationTimeout = static_cast<uint32_t>(number);
            explicitPreparationTimeout = true;
        } else if (key == "--expected-gain") {
            size_t end = 0;
            expectedGain = std::stod(value, &end);
            if (end != value.size() || !std::isfinite(expectedGain) || expectedGain < 0 ||
                expectedGain > 1)
                throw std::invalid_argument("Expected gain must be between 0 and 1");
        } else if (key == "--buffer-frames") {
            size_t end = 0;
            const auto number = std::stoul(value, &end);
            if (end != value.size() || number < 32 || number > 2048 || (number & (number - 1)))
                throw std::invalid_argument("Buffer frames must be a power of two from 32 to 2048");
            bufferFrames = static_cast<uint32_t>(number);
        } else if (key == "--channels" || key == "--seconds") {
            size_t end = 0;
            const auto number = std::stoul(value, &end);
            if (end != value.size() || number > 64)
                throw std::invalid_argument("Invalid IO check bound");
            if (key == "--channels")
                channels = static_cast<uint32_t>(number);
            else {
                seconds = static_cast<uint32_t>(number);
                explicitSeconds = true;
            }
        } else
            throw std::invalid_argument("Unknown IO check argument");
    }
    if (channels < 1 || channels > 64 || seconds < 3 || seconds > 30)
        throw std::invalid_argument("IO check requires 1–64 channels and 3–30 seconds");
    if (recovery && (explicitSeconds || expectedGain != 1))
        throw std::invalid_argument("Recovery protocol has fixed duration and unit gain");
    if (recovery && coordinatedCapture)
        throw std::invalid_argument("Capture and recovery protocols cannot be combined");
    if ((startupEvents || explicitPreparationTimeout) && !coordinatedCapture)
        throw std::invalid_argument("Startup options require the coordinated capture protocol");
    requireRoutingDevice(writeUID);
    requireRoutingDevice(readUID);
    if (microphonePermission() != MicrophonePermission::Authorized) {
        std::puts("{\"passed\":false,\"permissionRequired\":true,\"permissionRequested\":false}");
        return 77;
    }
    if (recovery)
        return routingRecoveryCheck(writeUID, readUID, channels, bufferFrames, coordinatedBaseline);
    Writer writer{channels};
    Reader reader{channels};
    reader.coordinated = coordinatedCapture;
    reader.measuring.store(!coordinatedCapture);
    reader.points.resize(static_cast<size_t>(seconds + 1) * 1100);
    singz::AudioHost output, input;
    if (startupEvents)
        emitEvent("opening-output");
    const auto written =
        output.open(config(writeUID, channels, false, bufferFrames), Writer::render, &writer);
    requireOK(written);
    if (startupEvents)
        emitEvent("opening-input");
    const auto captured =
        input.open(config(readUID, channels, true, bufferFrames), Reader::render, &reader);
    requireOK(captured);
    if (written.format.sampleRate != captured.format.sampleRate)
        throw std::invalid_argument("Channel checks require matching virtual-device sample rates");
    if (startupEvents)
        emitEvent("starting-output");
    requireOK(output.start());
    if (startupEvents)
        emitEvent("starting-input");
    requireOK(input.start());
    if (coordinatedCapture) {
        // All participating writers remain open until every reader has stopped.
        // The controller starts this strict window only after graph negotiation;
        // no sample inside it receives a warm-up exemption.
        emitEvent("prepared");
        awaitCaptureCommand("begin-capture", std::chrono::seconds(preparationTimeout));
        reader.measuring.store(true, std::memory_order_release);
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    input.stop();
    if (coordinatedCapture) {
        emitEvent("captured");
        awaitCaptureCommand("release-writer");
    }
    output.stop();
    const auto analysis = probe::checkChannels({reader.points.data(), reader.count}, channels);
    const auto inStatus = input.status(), outStatus = output.status();
    const bool signal = expectedGain == 0 ? analysis.maximumMagnitude < 0.000001
                                          : analysis.minimumCorrelation > 0.90 &&
                                                analysis.maximumCrossCorrelation < 0.35 &&
                                                analysis.minimumGain > expectedGain * 0.90 &&
                                                analysis.maximumGain < expectedGain * 1.10;
    const bool passed =
        signal && analysis.finite && !reader.nonfinite && !reader.overflow &&
        !reader.timestampErrors && !writer.timestampErrors && !inStatus.renderFailures &&
        !outStatus.renderFailures &&
        // At the common 48 kHz clock the nonzero marker needs
        // no sample-rate conversion. Even a short silent hole
        // must fail, rather than pass an averaged correlation.
        (expectedGain == 0 || captured.format.sampleRate != 48000 || !reader.zeroSamples);
    id firstCadenceAnomaly = [NSNull null];
    if (reader.firstCadenceAnomaly) {
        const auto& value = *reader.firstCadenceAnomaly;
        firstCadenceAnomaly = @{
            @"capturedFrame" : @(std::to_string(value.capturedFrame).c_str()),
            @"hostNs" : @(std::to_string(value.hostNs).c_str()),
            @"receivedNs" : @(std::to_string(value.receivedNs).c_str()),
            @"sourceFrame" : @(std::to_string(value.sourceFrame).c_str()),
            @"sequence" : @(std::to_string(value.sequence).c_str()),
            @"discontinuity" : @(value.discontinuity),
            @"timestampValid" : @(value.valid),
            @"timestampHardware" : @(value.hardware)
        };
    }
    NSDictionary* report = @{
        @"passed" : @(passed),
        @"channels" : @(channels),
        @"seconds" : @(seconds),
        @"sampleRate" : @(captured.format.sampleRate),
        @"requestedBufferFrames" : @(bufferFrames),
        @"inputBufferFrames" : @(captured.format.nominalBufferFrames),
        @"outputBufferFrames" : @(written.format.nominalBufferFrames),
        @"inputDeviceLatencyFrames" : @(captured.latency.inputDeviceFrames),
        @"outputDeviceLatencyFrames" : @(written.latency.outputDeviceFrames),
        @"points" : @(reader.count),
        @"minimumCorrelation" : @(analysis.minimumCorrelation),
        @"maximumCrossCorrelation" : @(analysis.maximumCrossCorrelation),
        @"minimumGain" : @(analysis.minimumGain),
        @"maximumGain" : @(analysis.maximumGain),
        @"maximumMagnitude" : @(analysis.maximumMagnitude),
        @"expectedGain" : @(expectedGain),
        @"timestampMarkerDelayMs" : @(analysis.delayMs),
        @"timestampErrors" : @(reader.timestampErrors + writer.timestampErrors),
        @"nonfiniteSamples" : @(reader.nonfinite),
        @"zeroSamples" : @(reader.zeroSamples),
        @"firstZeroFrame" : reader.zeroSamples ? @(reader.firstZeroFrame) : [NSNull null],
        @"lastZeroFrame" : reader.zeroSamples ? @(reader.lastZeroFrame) : [NSNull null],
        @"capturedFrames" : @(reader.frames),
        @"captureCallbacks" : @(reader.cadence.callbacks),
        @"callbackCadenceErrors" : @(reader.cadence.errors),
        @"firstCadenceAnomaly" : firstCadenceAnomaly,
        @"callbackFrames" : @[ @(reader.cadence.minimumFrames), @(reader.cadence.maximumFrames) ],
        @"coordinatedCapture" : @(coordinatedCapture),
        @"preparationTimeoutSeconds" : @(preparationTimeout),
        @"startupEvents" : @(startupEvents),
        @"pointOverflows" : @(reader.overflow),
        @"inputRenderFailures" : @(inStatus.renderFailures),
        @"outputRenderFailures" : @(outStatus.renderFailures),
        @"hostCountersScope" : @"entire-probe-including-preparation-and-shutdown",
        @"inputHostXruns" : @(inStatus.xruns),
        @"outputHostXruns" : @(outStatus.xruns),
        @"inputHostDeadlineMisses" : @(inStatus.deadlineMisses),
        @"outputHostDeadlineMisses" : @(outStatus.deadlineMisses),
        @"inputHostDiscontinuities" : @(inStatus.discontinuities),
        @"outputHostDiscontinuities" : @(outStatus.discontinuities),
        @"permissionRequested" : @NO,
        @"physicalDevicesOpened" : @NO
    };
    const auto data = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
    std::fwrite(data.bytes, 1, data.length, stdout);
    std::putchar('\n');
    return passed ? 0 : 1;
}
} // namespace
int routingIOCheck(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::out_of_range&) {
        std::fprintf(stderr, "LoopZ IO check: Numeric argument is out of range\n");
        return 2;
    } catch (const std::invalid_argument& error) {
        std::fprintf(stderr, "LoopZ IO check: %s\n", error.what());
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LoopZ IO check: %s\n", error.what());
        return 1;
    }
}
} // namespace loopz::macos
