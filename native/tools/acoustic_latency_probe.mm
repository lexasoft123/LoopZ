#include <zcore/device/audio_host.h>
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
volatile sig_atomic_t canceled = 0;
void cancel(int) {
    canceled = 1;
}
constexpr uint32_t kCodeFrames = 1024;

double correlation(const std::vector<float>& reference, const std::vector<float>& returned,
                   int start, int count, int lag, int stride) {
    double xy = 0, xx = 0, yy = 0;
    for (int i = start; i < start + count; i += stride) {
        // Remove slow room noise and DC before matching the actual microphone waveform.
        double x = 0, y = 0;
        for (int offset = 0; offset < stride; ++offset) {
            x += reference[i - offset] - 0.97 * reference[i - offset - stride];
            y += returned[i + lag - offset] - 0.97 * returned[i + lag - offset - stride];
        }
        xy += x * y;
        xx += x * x;
        yy += y * y;
    }
    return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
}

NSDictionary* matchReturn(const std::vector<float>& reference, const std::vector<float>& returned,
                          double rate, double startSeconds) {
    const int start = int(rate * startSeconds), count = int(rate / 2);
    int bestLag = 0;
    double best = 0;
    auto* curve = [NSMutableArray array];
    for (int lag = -2048; lag <= int(rate / 4); lag += 8) {
        const auto value = correlation(reference, returned, start, count, lag, 8);
        [curve addObject:@(value)];
        if (std::fabs(value) > std::fabs(best)) {
            best = value;
            bestLag = lag;
        }
    }
    const auto coarseLag = bestLag;
    best = 0;
    for (int lag = coarseLag - 8; lag <= coarseLag + 8; ++lag) {
        const auto value = correlation(reference, returned, start, count, lag, 1);
        if (std::fabs(value) > std::fabs(best)) {
            best = value;
            bestLag = lag;
        }
    }
    return @{
        @"startSeconds" : @(startSeconds),
        @"windowSeconds" : @0.5,
        @"lagFrames" : @(bestLag),
        @"lagMs" : @(1000.0 * bestLag / rate),
        @"correlation" : @(best),
        @"coarseFirstLag" : @(-2048),
        @"coarseStepFrames" : @8,
        @"coarseCorrelation" : curve
    };
}

struct Probe {
    double rate;
    float amplitude;
    std::vector<float> code, input;
    std::vector<std::vector<float>> additionalInputs;
    uint32_t primaryChannel{0};
    bool passive{false};
    std::array<uint64_t, 3> emissions;
    uint64_t position{0}, expectedInput{0};
    uint64_t timestampErrors{0}, invalidFrames{0}, nonfinite{0};
    bool previous{false};
    std::atomic<bool> done{false};
    explicit Probe(double sampleRate, float level)
        : rate(sampleRate), amplitude(level), code(kCodeFrames), input(size_t(rate * 4)),
          emissions{uint64_t(rate / 2), uint64_t(rate * 1.5), uint64_t(rate * 2.5)} {
        std::mt19937 random(0x4c6f6f70);
        std::array<float, 5> history{};
        float prior = 0;
        for (size_t i = 0; i < code.size(); ++i) {
            history[i % history.size()] = (random() & 1) ? 1.f : -1.f;
            const auto low = std::accumulate(history.begin(), history.end(), 0.f) / history.size();
            const auto high = low - prior;
            prior = low * 0.9f + prior * 0.1f;
            code[i] = high * float(0.5 - 0.5 * std::cos(2 * M_PI * i / (code.size() - 1)));
        }
        const auto peak = std::max_element(code.begin(), code.end(), [](float a, float b) {
            return std::fabs(a) < std::fabs(b);
        });
        const auto scale = std::fabs(*peak);
        for (auto& sample : code)
            sample /= scale;
    }
    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<Probe*>(context);
        for (uint32_t channel = 0; channel < block.outputChannels; ++channel)
            if (block.output && block.output[channel])
                std::fill_n(block.output[channel], block.frames, 0.f);
        if (self.done.load())
            return true;
        const auto count = std::min<uint64_t>(block.frames, self.input.size() - self.position);
        const bool valid = block.input && block.inputChannels > self.primaryChannel &&
                           block.input[self.primaryChannel] && block.inputTimestampValid &&
                           block.inputTimestampHardware;
        if (self.position >= uint64_t(self.rate / 4)) {
            if (!valid)
                self.invalidFrames += count;
            else if (self.previous && block.inputSourceFrame != self.expectedInput)
                ++self.timestampErrors;
        }
        self.previous = valid;
        self.expectedInput = singz::advanceAudioHostFrame(block.inputSourceFrame, block.frames);
        for (uint64_t i = 0; i < count; ++i) {
            const auto index = self.position + i;
            const auto sample = valid ? block.input[self.primaryChannel][i] : 0.f;
            if (!std::isfinite(sample))
                ++self.nonfinite;
            self.input[index] = std::isfinite(sample) ? sample : 0.f;
            for (size_t channel = 0; channel < self.additionalInputs.size(); ++channel) {
                const auto value = valid && channel < block.inputChannels && block.input[channel]
                                       ? block.input[channel][i]
                                       : 0.f;
                self.additionalInputs[channel][index] = std::isfinite(value) ? value : 0.f;
            }
            float output = 0;
            for (auto start : self.emissions)
                if (!self.passive && index >= start && index - start < self.code.size())
                    output = self.amplitude * self.code[index - start];
            if (block.output && block.outputChannels && block.output[0])
                block.output[0][i] = output;
        }
        self.position += count;
        if (self.position == self.input.size())
            self.done.store(true);
        return true;
    }
    NSDictionary* result(const singz::AudioHostFormat& format,
                         const std::vector<float>* selected = nullptr) const {
        const auto& samples = selected ? *selected : input;
        const auto lags = size_t(rate * 0.5);
        auto* response = [NSMutableArray arrayWithCapacity:lags];
        const auto energy = std::inner_product(code.begin(), code.end(), code.begin(), 0.0);
        for (size_t lag = 0; lag < lags; ++lag) {
            double sum = 0;
            for (auto start : emissions)
                sum += std::inner_product(code.begin(), code.end(), samples.begin() + start + lag,
                                          0.0);
            [response addObject:@(sum / (energy * amplitude * emissions.size()))];
        }
        double peak = 0, square = 0;
        for (auto sample : samples) {
            peak = std::max(peak, double(std::fabs(sample)));
            square += sample * sample;
        }
        return @{
            @"passed" :
                @(done.load() && !invalidFrames && !timestampErrors && !nonfinite && peak < 0.95),
            @"sampleRate" : @(rate),
            @"amplitude" : @(amplitude),
            @"codeFrames" : @(code.size()),
            @"emissions" : @(emissions.size()),
            @"inputFrames" : @(position),
            @"timestampErrors" : @(timestampErrors),
            @"invalidFrames" : @(invalidFrames),
            @"nonfiniteSamples" : @(nonfinite),
            @"inputPeak" : @(peak),
            @"inputRms" : @(std::sqrt(square / samples.size())),
            @"clientBufferFrames" : @(format.nominalBufferFrames),
            @"responseStepFrames" : @1,
            @"response" : response,
            @"rawAudioSaved" : @NO
        };
    }
};
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            Probe probe(48000, 0.006f);
            for (auto start : probe.emissions)
                for (size_t i = 0; i < probe.code.size(); ++i) {
                    probe.input[start + 512 + i] += probe.amplitude * probe.code[i];
                    probe.input[start + 3072 + i] += probe.amplitude * probe.code[i] * 0.15f;
                }
            probe.position = probe.input.size();
            probe.done.store(true);
            NSDictionary* result = probe.result({});
            NSArray* response = result[@"response"];
            std::vector<float> reference(probe.input.size()), returned(probe.input.size());
            std::mt19937 random(19);
            for (size_t i = 0; i < reference.size(); ++i) {
                reference[i] = float(int(random() % 2001) - 1000) / 1000;
                if (i >= 2203)
                    returned[i] = 0.5f * reference[i - 2203];
            }
            auto* matched = matchReturn(reference, returned, 48000, 0.5);
            const bool passed = [result[@"passed"] boolValue] &&
                                std::fabs([response[512] doubleValue] - 1) < 0.00001 &&
                                std::fabs([response[3072] doubleValue] - 0.15) < 0.00001 &&
                                [matched[@"lagFrames"] intValue] == 2203 &&
                                [matched[@"correlation"] doubleValue] > 0.999;
            std::printf("{\"passed\":%s,\"directFrames\":512,\"echoFrames\":3072,\"roundTripMs\":"
                        "53.3333333333,\"waveformLagFrames\":%d,\"waveformCorrelation\":%.9f}\n",
                        passed ? "true" : "false", [matched[@"lagFrames"] intValue],
                        [matched[@"correlation"] doubleValue]);
            return passed ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--permission-status") {
            std::printf("{\"authorized\":%s}\n",
                        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] ==
                                AVAuthorizationStatusAuthorized
                            ? "true"
                            : "false");
            return 0;
        }
        const bool passive = argc == 7 && std::string_view(argv[1]) == "--loopback";
        const bool scan = passive || (argc == 7 && std::string_view(argv[1]) == "--scan");
        if (scan) {
            --argc;
            ++argv;
        }
        if (argc != 6)
            return 2;
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] !=
            AVAuthorizationStatusAuthorized) {
            std::fprintf(stderr,
                         "Existing microphone permission is required; no request was made.\n");
            return 3;
        }
        try {
            const std::string uid = argv[1];
            const auto channel = std::stoul(argv[2]), output = std::stoul(argv[3]);
            const auto level = std::stof(argv[4]);
            if (!channel || channel > 64 || !output || output > 64 || !std::isfinite(level) ||
                level < 0.0001f || level > 0.02f)
                return 2;
            singz::AudioHost host;
            double rate = 0;
            for (const auto& device : host.enumerate().devices)
                if (device.uid == uid && device.inputChannels >= channel &&
                    device.outputChannels >= output)
                    rate = device.nominalSampleRate;
            if (rate != 48000 && rate != 44100)
                throw std::runtime_error("The unchanged device rate must be 44.1 or 48 kHz");
            Probe probe(rate, level);
            probe.passive = passive;
            if (scan) {
                probe.primaryChannel = static_cast<uint32_t>(channel - 1);
                probe.additionalInputs.resize(16, std::vector<float>(probe.input.size()));
            }
            singz::AudioHostConfig config;
            config.inputDeviceUid = config.outputDeviceUid = uid;
            config.inputChannels = {static_cast<uint32_t>(channel - 1)};
            if (scan) {
                config.inputChannels.resize(16);
                std::iota(config.inputChannels.begin(), config.inputChannels.end(), 0);
            }
            config.outputChannels = {static_cast<uint32_t>(output - 1)};
            auto opened = host.open(config, Probe::render, &probe);
            if (!opened.ok)
                throw std::runtime_error(opened.message);
            const auto format = host.status().format;
            if (format.sampleRate != rate)
                throw std::runtime_error("The device rate changed during opening");
            auto started = host.start();
            if (!started.ok)
                throw std::runtime_error(started.message);
            std::signal(SIGTERM, cancel);
            std::signal(SIGINT, cancel);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!probe.done.load() && !canceled && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            host.stop();
            if (!probe.done.load() || canceled)
                throw std::runtime_error("The bounded probe did not complete");
            NSMutableDictionary* report = [probe.result(format) mutableCopy];
            report[@"deviceUID"] = @(uid.c_str());
            report[@"inputChannel"] = @(channel);
            report[@"outputChannel"] = @(output);
            if (passive) {
                [report removeObjectForKey:@"response"];
                report[@"emissions"] = @0;
                report[@"method"] = @"simultaneous physical input channels, waveform correlation";
                auto* returns = [NSMutableArray array];
                for (const auto index : {8, 9}) {
                    auto* windows = [NSMutableArray array];
                    for (const auto start : {0.5, 1.5, 2.5})
                        [windows addObject:matchReturn(probe.input, probe.additionalInputs[index],
                                                       rate, start)];
                    [returns addObject:@{@"inputChannel" : @(index + 1), @"windows" : windows}];
                }
                report[@"returns"] = returns;
            } else if (scan) {
                auto* channels = [NSMutableArray array];
                for (size_t index = 0; index < probe.additionalInputs.size(); ++index) {
                    NSMutableDictionary* item =
                        [probe.result(format, &probe.additionalInputs[index]) mutableCopy];
                    item[@"inputChannel"] = @(index + 1);
                    [channels addObject:item];
                }
                report[@"channels"] = channels;
            }
            auto* data = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
            if (![data writeToFile:@(argv[5]) atomically:YES])
                throw std::runtime_error("Could not save correlation report");
            return [report[@"passed"] boolValue] ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
