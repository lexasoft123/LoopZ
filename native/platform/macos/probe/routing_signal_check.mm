#include "routing_signal_check.hpp"
#include "routing_io_support.hpp"
#include <loopz/macos/permissions.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace loopz::macos {
namespace {
uint32_t integer(const std::string& value, uint32_t maximum) {
    size_t end = 0;
    const auto parsed = std::stoul(value, &end);
    if (end != value.size() || parsed > maximum)
        throw std::invalid_argument("Invalid signal-check integer");
    return static_cast<uint32_t>(parsed);
}
double frequency(const std::string& value) {
    size_t end = 0;
    const auto parsed = std::stod(value, &end);
    if (end != value.size() || !std::isfinite(parsed) || parsed < 40 || parsed > 20000)
        throw std::invalid_argument("Signal-check frequencies must be 40–20000 Hz");
    return parsed;
}
struct Reader {
    probe::StreamCheck checker;
    bool tone{true}, havePrevious{false};
    uint64_t frames{0}, steadyFrames{0}, expectedFrame{0};
    uint64_t timestampErrors{0}, nonfinite{0}, quietFrames{0}, zeroSamples{0};
    double energy[2]{}, maximumMagnitude{0};
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Reader*>(context);
        if (!block.input || !block.output || block.inputChannels != 2 || block.outputChannels != 2)
            return false;
        for (uint32_t channel = 0; channel < 2; ++channel)
            std::fill_n(block.output[channel], block.frames, 0);
        const bool valid = block.inputTimestampValid && block.inputTimestampHardware;
        if (!valid || (self.havePrevious && block.inputSourceFrame != self.expectedFrame))
            ++self.timestampErrors;
        self.havePrevious = true;
        self.expectedFrame = block.inputSourceFrame + block.frames;
        if (self.tone)
            self.checker.append(block.input[0], block.input[1], block.frames,
                                block.inputSampleHostTimeNs,
                                static_cast<double>(block.inputSourceFrame), valid);
        if (self.frames >= static_cast<uint64_t>(block.sampleRate)) {
            double peak[2]{};
            for (uint32_t frame = 0; frame < block.frames; ++frame)
                for (uint32_t channel = 0; channel < 2; ++channel) {
                    const double value = block.input[channel][frame];
                    if (!std::isfinite(value)) {
                        ++self.nonfinite;
                        continue;
                    }
                    self.energy[channel] += value * value;
                    if (value == 0)
                        ++self.zeroSamples;
                    peak[channel] = std::max(peak[channel], std::abs(value));
                }
            self.maximumMagnitude = std::max({self.maximumMagnitude, peak[0], peak[1]});
            if (peak[0] < 0.02 || peak[1] < 0.02)
                self.quietFrames += block.frames;
            self.steadyFrames += block.frames;
        }
        self.frames += block.frames;
        return true;
    }
};
int run(int argc, char** argv) {
    std::string uid, expect = "tone";
    uint32_t seconds = 5, bufferFrames = 0, components = 1;
    double frequencies[2]{1100, 2200};
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc)
            throw std::invalid_argument("Signal-check arguments require values");
        const std::string key = argv[index], value = argv[index + 1];
        if (key == "--read-device")
            uid = value;
        else if (key == "--expect")
            expect = value;
        else if (key == "--seconds")
            seconds = integer(value, 86400);
        else if (key == "--tone-components")
            components = integer(value, 2);
        else if (key == "--buffer-frames") {
            bufferFrames = integer(value, 2048);
            if (bufferFrames < 32 || (bufferFrames & (bufferFrames - 1)))
                throw std::invalid_argument("Signal buffer must be a power of two from 32–2048");
        } else if (key == "--frequency-left")
            frequencies[0] = frequency(value);
        else if (key == "--frequency-right")
            frequencies[1] = frequency(value);
        else
            throw std::invalid_argument("Unknown signal-check argument");
    }
    if (seconds < 3 || (expect != "tone" && expect != "silence") ||
        frequencies[0] == frequencies[1] || !components || (expect == "silence" && components != 1))
        throw std::invalid_argument("Invalid signal-check duration, expectation or frequencies");
    diagnostic::requireRoutingDevice(uid);
    if (microphonePermission() != MicrophonePermission::Authorized) {
        std::puts("{\"passed\":false,\"permissionRequired\":true,\"permissionRequested\":false}");
        return 77;
    }
    auto reader = std::make_unique<Reader>();
    reader->tone = expect == "tone";
    singz::AudioHost host;
    const auto opened =
        host.open(diagnostic::config(uid, 2, true, bufferFrames), Reader::render, reader.get());
    diagnostic::requireOK(opened);
    const auto rate = opened.format.sampleRate;
    if (std::max(frequencies[0], frequencies[1]) >= rate / 2)
        throw std::invalid_argument("Signal-check frequency exceeds the device Nyquist limit");
    reader->checker.rate = rate;
    std::copy_n(frequencies, 2, reader->checker.frequencies);
    diagnostic::requireOK(host.start());
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
           seconds) {
        if (reader->tone)
            reader->checker.consume();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    host.stop();
    if (reader->tone)
        reader->checker.consume();
    const auto status = host.status();
    const auto& check = reader->checker;
    const double rms[2]{
        reader->steadyFrames ? std::sqrt(reader->energy[0] / reader->steadyFrames) : 0,
        reader->steadyFrames ? std::sqrt(reader->energy[1] / reader->steadyFrames) : 0};
    const bool signal = reader->tone
                            ? check.windows >= seconds - 2 && !check.badWindows &&
                                  !check.timestampErrors.load() && !check.queueOverruns.load() &&
                                  check.maximumDelayMs - check.minimumDelayMs <= 2 &&
                                  !reader->quietFrames && rms[0] > 0.055 && rms[1] > 0.055 &&
                                  (rate != 48000 || !reader->zeroSamples) &&
                                  rms[0] < 0.095 * components && rms[1] < 0.095 * components
                            : reader->maximumMagnitude < 0.000001;
    const bool passed = signal && reader->steadyFrames + 4096 >= (seconds - 1) * rate &&
                        !reader->timestampErrors && !reader->nonfinite && !status.renderFailures;
    NSDictionary* report = @{
        @"passed" : @(passed),
        @"expect" : @(expect.c_str()),
        @"toneComponents" : @(components),
        @"seconds" : @(seconds),
        @"sampleRate" : @(rate),
        @"frames" : @(reader->frames),
        @"steadyFrames" : @(reader->steadyFrames),
        @"inputBufferFrames" : @(opened.format.nominalBufferFrames),
        @"rms" : @[ @(rms[0]), @(rms[1]) ],
        @"maximumMagnitude" : @(reader->maximumMagnitude),
        @"nonfiniteSamples" : @(reader->nonfinite),
        @"quietFrames" : @(reader->quietFrames),
        @"zeroSamples" : @(reader->zeroSamples),
        @"timestampErrors" : @(reader->timestampErrors),
        @"renderFailures" : @(status.renderFailures),
        @"windows" : @(check.windows),
        @"badWindows" : @(check.badWindows),
        @"minimumAmplitude" : @(check.minimumAmplitude),
        @"maximumCross" : @(check.maximumCross),
        @"minimumMarkerCorrelation" : @(check.minimumCorrelation),
        @"markerDelayMs" : reader->tone ? @[ @(check.minimumDelayMs), @(check.maximumDelayMs) ]
                                        : (id)NSNull.null,
        @"delayResolutionMs" : @1,
        @"queueOverruns" : @(check.queueOverruns.load()),
        @"permissionRequested" : @NO,
        @"physicalDevicesOpened" : @NO,
        @"audioSamplesRecorded" : @NO
    };
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
    if (!bytes)
        return 1;
    std::fwrite(bytes.bytes, 1, bytes.length, stdout);
    std::puts("");
    return passed ? 0 : 1;
}
} // namespace
int routingSignalCheck(int argc, char** argv) {
    @autoreleasepool {
        try {
            return run(argc, argv);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Routing signal check: %s\n", error.what());
            return 2;
        }
    }
}
} // namespace loopz::macos
