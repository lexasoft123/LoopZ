#include <loopz/macos/monitor_outputs.hpp>
#include <loopz/timeline.hpp>
#include <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
using namespace loopz;
using namespace loopz::macos;
uint64_t nowNs() {
    static const auto scale = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    return static_cast<uint64_t>(static_cast<long double>(mach_absolute_time()) * scale.numer /
                                 scale.denom);
}
bool emit(id value) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:value
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:nil];
    return data && std::fwrite(data.bytes, 1, data.length, stdout) == data.length;
}
class SilentCapture final : public AudioSink {
  public:
    explicit SilentCapture(uint32_t count) : channels_(count) {}
    uint32_t channels() const noexcept override {
        return channels_;
    }
    bool requested() const noexcept override {
        return false;
    }
    bool publish(uint64_t, uint32_t, RoutingInput, uint32_t) noexcept override {
        return true;
    }
    void idle() noexcept override {}

  private:
    const uint32_t channels_;
};
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if (argc == 2 && std::string(argv[1]) == "--list") {
                singz::AudioHost host;
                NSMutableArray* devices = [NSMutableArray array];
                for (const auto& device : host.enumerate().devices)
                    if (device.outputChannels)
                        [devices addObject:@{
                            @"uid" : @(device.uid.c_str()),
                            @"name" : @(device.label.c_str()),
                            @"channels" : @(device.outputChannels),
                            @"rate" : @(device.nominalSampleRate),
                            @"bufferFrames" : @(device.bufferFrames.preferredFrames)
                        }];
                return emit(devices) ? 0 : 1;
            }
            uint32_t seconds = 6, delay = 1024, channels = 2;
            std::vector<std::string> selectors;
            for (int index = 1; index < argc; ++index) {
                const std::string option = argv[index];
                if (index + 1 >= argc)
                    return 2;
                const std::string value = argv[++index];
                if (option == "--output")
                    selectors.push_back(value);
                else if (option == "--seconds" || option == "--delay-frames" ||
                         option == "--channels") {
                    char* end = nullptr;
                    const auto number = std::strtoul(value.c_str(), &end, 10);
                    if (!end || end == value.c_str() || *end || number > 16384)
                        return 2;
                    if (option == "--seconds")
                        seconds = static_cast<uint32_t>(number);
                    else if (option == "--delay-frames")
                        delay = static_cast<uint32_t>(number);
                    else
                        channels = static_cast<uint32_t>(number);
                } else
                    return 2;
            }
            if (selectors.empty() || selectors.size() > 32 || seconds < 3 || seconds > 30 ||
                !channels || channels > kMaximumRoutingChannels)
                return 2;
            MonitorOutputs outputs(48000, delay, {}, [] {
                return nowNs() / 1000000;
            });
            VirtualDeviceDefinition device;
            device.id = "isolated-silent-monitor-probe";
            device.name = "Silent Monitor Probe";
            device.routing.outputChannels = channels;
            for (const auto& selector : selectors)
                device.monitors.push_back({std::to_string(device.monitors.size()), selector,
                                           channels, 1, true, true,
                                           defaultChannelConnections(2, 2)});
            AudioScene scene({1, {device}}, {}, {}, [&](const auto& definition) {
                return outputs.decorate(definition, std::make_shared<SilentCapture>(channels));
            });
            scene.activate();
            const auto start = nowNs();
            std::atomic<uint64_t> lateBlocks{0}, reanchors{0}, renderFailures{0};
            std::jthread renderer([&](std::stop_token stop) {
                auto first = hostFrame(start, 48000);
                while (!stop.stop_requested()) {
                    const auto current = hostFrame(nowNs(), 48000);
                    if (first + kCapacity / 2 < current) {
                        first = current;
                        reanchors.fetch_add(1);
                    }
                    while (first <= current + kBlock) {
                        if (first + kBlock < current)
                            lateBlocks.fetch_add(1);
                        const auto result = scene.render(first, kBlock);
                        if (!result.validBlock || result.failedGraphs || result.failedSinks)
                            renderFailures.fetch_add(1);
                        first += kBlock;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            uint64_t nextPoll = 0;
            std::vector<MonitorConnectionStatus> baseline;
            while (nowNs() - start < static_cast<uint64_t>(seconds) * 1000000000) {
                const auto now = nowNs();
                if (now >= nextPoll) {
                    outputs.poll(&scene, now / 1000000);
                    nextPoll = now + 50000000;
                    if (baseline.empty() && now - start > 1500000000)
                        baseline = outputs.status();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            bool passed = outputs.restored() && !renderFailures.load() && !reanchors.load();
            NSMutableArray* status = [NSMutableArray array];
            const auto final = outputs.status();
            for (size_t index = 0; index < final.size(); ++index) {
                const auto& monitor = final[index];
                const auto* initial = index < baseline.size() ? &baseline[index] : nullptr;
                const auto missing =
                    initial ? monitor.audio.missingFrames - initial->audio.missingFrames
                            : UINT64_MAX;
                passed = passed && monitor.connected && monitor.complete &&
                         monitor.audio.sourceOnline && monitor.audio.callbacks > 10 &&
                         !monitor.audio.graphFailures && !monitor.audio.invalidBlocks && !missing;
                AudioMeterSnapshot meter;
                if (!outputs.meters(device.id, monitor.monitor, meter))
                    passed = false;
                else
                    for (uint32_t channel = 0; channel < meter.channels; ++channel)
                        if (meter.peak[channel] != 0 || meter.rms[channel] != 0)
                            passed = false;
                [status addObject:@{
                    @"uid" : @(monitor.resolvedUid.c_str()),
                    @"connected" : @(monitor.connected),
                    @"complete" : @(monitor.complete),
                    @"rate" : @(monitor.sampleRate),
                    @"physicalChannels" : @(monitor.physicalChannels),
                    @"requestedChannels" : @(monitor.requestedChannels),
                    @"callbacks" : @(monitor.audio.callbacks),
                    @"queueFrames" : @(monitor.queueFrames),
                    @"missingFrames" : @(monitor.audio.missingFrames),
                    @"steadyMissingFrames" : @(missing),
                    @"graphFailures" : @(monitor.audio.graphFailures),
                    @"invalidBlocks" : @(monitor.audio.invalidBlocks),
                    @"measuredOutputRate" : @(monitor.audio.measuredOutputRate),
                    @"attempts" : @(monitor.attempts),
                    @"error" : @(monitor.error.c_str())
                }];
            }
            renderer.request_stop();
            renderer.join();
            outputs.stop();
            emit(@{
                @"status" : passed ? @"passed" : @"failed",
                @"durationSeconds" : @(seconds),
                @"delayFrames" : @(delay),
                @"internalRate" : @48000,
                @"channels" : @(channels),
                @"alwaysMuted" : @YES,
                @"inputStreamsOpened" : @0,
                @"lateRenderBlocks" : @(lateBlocks.load()),
                @"renderReanchors" : @(reanchors.load()),
                @"renderFailures" : @(renderFailures.load()),
                @"monitors" : status
            });
            return passed ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
