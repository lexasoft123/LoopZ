#include <loopz/macos/capture.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/capture_sources.hpp>
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>
#include <optional>
#include <stdexcept>
#include "hal_input_experiment.hpp"

namespace {
using namespace loopz;
using namespace loopz::macos;

volatile sig_atomic_t stopping = 0;

void stop(int) {
    stopping = 1;
}

// The actual capture, graph, scheduler and monitor run unchanged. Only public
// device publication is replaced, so this test cannot alter installed routes.
class UnpublishedLink final : public DriverConfigurationLink {
  public:
    uint64_t session() const noexcept override {
        return 1;
    }

    int32_t submit(std::span<const DeviceTransport>, uint64_t revision) override {
        revision_ = revision;
        return 0;
    }

    DriverConfigurationResult status(uint64_t) override {
        return {revision_, DriverConfigurationPhase::Committed, 0};
    }

    int32_t cancel(uint64_t) override {
        return 0;
    }

    void invalidate() noexcept override {}

  private:
    uint64_t revision_ = 0;
};

uint32_t number(const char* text, uint32_t maximum) {
    size_t end = 0;
    const std::string value(text);
    const auto parsed = std::stoul(value, &end);
    if (end != value.size() || parsed > maximum)
        throw std::invalid_argument("Invalid bounded numeric option");
    return static_cast<uint32_t>(parsed);
}

struct Counters {
    EngineRuntimeStatus runtime;
    SceneSourceStatus source;
};

Counters counters(EngineRuntime& runtime) {
    Counters result;
    result.runtime = runtime.status();
    auto scene = runtime.observe();
    if (!scene || scene->document().devices.size() != 1)
        throw std::runtime_error("The private physical test scene is unavailable");
    result.source = scene->sourceStatus(0, 0);
    return result;
}

void emit(NSDictionary* value) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if (!data || std::fwrite(data.bytes, 1, data.length, stdout) != data.length)
        throw std::runtime_error("Could not write physical test status");
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Explicit experiment only. Normal Engine acquisition never changes the shared
// period. Rollback declines to overwrite a different rate/period selected by
// another application while this experiment was running.
class TemporaryPeriod {
  public:
    TemporaryPeriod(const std::string& uid, uint32_t requested) : uid_(uid), requested_(requested) {
        device_ = resolve();
        if (!device_)
            throw std::runtime_error("The exact physical device is unavailable");
        original_ = read<UInt32>(kAudioDevicePropertyBufferFrameSize);
        rate_ = read<Float64>(kAudioDevicePropertyNominalSampleRate);
        if (!requested_ || requested_ == original_)
            return;
        const auto range = read<AudioValueRange>(kAudioDevicePropertyBufferFrameSizeRange);
        if (requested_ < range.mMinimum || requested_ > range.mMaximum ||
            (requested_ & (requested_ - 1)))
            throw std::invalid_argument("The requested hardware period is unsupported");
        const AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        Boolean settable = false;
        if (AudioObjectIsPropertySettable(device_, &address, &settable) || !settable)
            throw std::runtime_error("The shared hardware period is not writable");
        try {
            if (resolve() != device_ ||
                read<UInt32>(kAudioDevicePropertyBufferFrameSize) != original_ ||
                read<Float64>(kAudioDevicePropertyNominalSampleRate) != rate_)
                throw std::runtime_error("The device changed before the period request");
            active_ = true;
            if (AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(requested_),
                                           &requested_) ||
                read<UInt32>(kAudioDevicePropertyBufferFrameSize) != requested_)
                throw std::runtime_error("The exact hardware period request was not accepted");
        } catch (...) {
            restore();
            throw;
        }
    }

    ~TemporaryPeriod() {
        if (!restore())
            std::fprintf(stderr, "Hardware period restoration could not be verified\n");
    }

    TemporaryPeriod(const TemporaryPeriod&) = delete;
    TemporaryPeriod& operator=(const TemporaryPeriod&) = delete;

    uint32_t original() const noexcept {
        return original_;
    }

    uint32_t current() const {
        if (resolve() != device_)
            throw std::runtime_error("The original physical device is unavailable");
        return read<UInt32>(kAudioDevicePropertyBufferFrameSize);
    }

    bool restore() noexcept {
        if (!active_)
            return restored_;
        try {
            if (resolve() != device_ ||
                read<Float64>(kAudioDevicePropertyNominalSampleRate) != rate_)
                return abandon();
            const auto current = read<UInt32>(kAudioDevicePropertyBufferFrameSize);
            if (current == original_) {
                active_ = false;
                return true;
            }
            if (current != requested_)
                return abandon();
            const AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                                     kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMain};
            if (AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(original_),
                                           &original_))
                return false;
            restored_ = read<UInt32>(kAudioDevicePropertyBufferFrameSize) == original_;
            active_ = !restored_;
            return restored_;
        } catch (...) {
            return false;
        }
    }

  private:
    bool abandon() noexcept {
        active_ = false;
        restored_ = false;
        return false;
    }

    AudioDeviceID resolve() const {
        CFStringRef uid = CFStringCreateWithCString(nullptr, uid_.c_str(), kCFStringEncodingUTF8);
        if (!uid)
            return 0;
        AudioDeviceID device = 0;
        UInt32 bytes = sizeof(device);
        const AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                       sizeof(uid), &uid, &bytes, &device);
        CFRelease(uid);
        return status || bytes != sizeof(device) ? 0 : device;
    }

    template <class Value> Value read(AudioObjectPropertySelector selector) const {
        const AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        Value value{};
        UInt32 bytes = sizeof(value);
        if (AudioObjectGetPropertyData(device_, &address, 0, nullptr, &bytes, &value) ||
            bytes != sizeof(value))
            throw std::runtime_error("The physical device property is unavailable");
        return value;
    }

    const std::string uid_;
    UInt32 requested_ = 0;
    UInt32 original_ = 0;
    AudioDeviceID device_ = 0;
    Float64 rate_ = 0;
    bool active_ = false;
    bool restored_ = true;
};

struct DirectPath {
    RoutingGraph graph;
    uint64_t frames = 0;
    uint64_t invalid = 0;
    uint64_t nonfinite = 0;
    uint64_t timestampErrors = 0;
    uint64_t discontinuities = 0;
    uint64_t expectedOutputFrame = 0;
    bool previousOutput = false;
    double peak = 0;

    explicit DirectPath(bool muted)
        : graph(48000, [&] {
              RoutingConfiguration routing;
              routing.outputChannels = 1;
              routing.sources = {{"microphone", 1, 0.5f, true, muted, {{0, 0, 1}}}};
              return routing;
          }()) {}

    static bool render(void* context, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<DirectPath*>(context);
        if (!block.input || !block.output || block.inputChannels != 1 ||
            block.outputChannels != 1 || !block.input[0] || !block.output[0] ||
            !block.inputTimestampValid || !block.inputTimestampHardware ||
            !block.outputTimestampValid || !block.outputTimestampHardware) {
            ++self.invalid;
            if (block.output && block.outputChannels && block.output[0])
                std::fill_n(block.output[0], block.frames, 0.f);
            return false;
        }
        for (uint32_t offset = 0; offset < block.frames; offset += kBlock) {
            const auto count = std::min(kBlock, block.frames - offset);
            const float* input[] = {block.input[0] + offset};
            float* output[] = {block.output[0] + offset};
            const RoutingInput source{input, count};
            if (!self.graph.render({&source, 1}, {output, count}, count, self.frames + offset))
                ++self.invalid;
            for (uint32_t frame = 0; frame < count; ++frame) {
                const auto sample = output[0][frame];
                if (!std::isfinite(sample))
                    ++self.nonfinite;
                else
                    self.peak = std::max(self.peak, double(std::fabs(sample)));
            }
        }
        if (self.frames >= 96000) {
            if (self.previousOutput && block.outputFrame != self.expectedOutputFrame)
                ++self.timestampErrors;
            if (block.discontinuity != singz::AudioHostDiscontinuityNone)
                ++self.discontinuities;
        }
        self.expectedOutputFrame = block.outputFrame + block.frames;
        self.previousOutput = true;
        self.frames += block.frames;
        return true;
    }
};

int directPath(const std::string& uid, uint32_t requestedPeriod, uint32_t seconds, bool muted) {
    if (seconds < 4 || seconds > 60 || requestedPeriod < 16 || requestedPeriod > 512)
        return 2;
    TemporaryPeriod period(uid, requestedPeriod);
    DirectPath path(muted);
    singz::AudioHost host;
    singz::AudioHostConfig config;
    config.inputDeviceUid = config.outputDeviceUid = uid;
    config.inputChannels = {2};
    config.outputChannels = {0};
    auto opened = host.open(config, DirectPath::render, &path);
    if (!opened.ok || opened.format.sampleRate != 48000)
        throw std::runtime_error("The unchanged 48 kHz direct path did not open");
    auto started = host.start();
    if (!started.ok)
        throw std::runtime_error(started.message);
    const auto begun = std::chrono::steady_clock::now();
    bool announced = false;
    while (!stopping && std::chrono::steady_clock::now() - begun < std::chrono::seconds(seconds)) {
        if (!announced && std::chrono::steady_clock::now() - begun > std::chrono::seconds(2)) {
            emit(@{@"phase" : @"ready", @"restored" : @YES, @"directHardwareClock" : @YES});
            announced = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto hostStatus = host.status();
    host.stop();
    const bool restored = period.restore();
    const bool passed = !stopping && restored && path.frames >= seconds * 47000 && !path.invalid &&
                        !path.nonfinite && !path.timestampErrors && !path.discontinuities &&
                        !hostStatus.xruns && !hostStatus.renderFailures && path.peak < 0.5;
    emit(@{
        @"phase" : @"complete",
        @"passed" : @(passed),
        @"directHardwareClock" : @YES,
        @"gain" : @0.5,
        @"muted" : @(muted),
        @"frames" : @(path.frames),
        @"invalidBlocks" : @(path.invalid),
        @"nonfinite" : @(path.nonfinite),
        @"outputPeak" : @(path.peak),
        @"hardwareTimestampErrors" : @(path.timestampErrors),
        @"hardwareDiscontinuities" : @(path.discontinuities),
        @"hostXruns" : @(hostStatus.xruns),
        @"physicalPeriodBefore" : @(period.original()),
        @"physicalPeriodAfter" : @(period.current()),
        @"physicalPeriodRestored" : @(restored),
        @"publicDevicePublished" : @NO,
        @"resamplingUsed" : @NO
    });
    return passed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            std::signal(SIGTERM, stop);
            std::signal(SIGINT, stop);
            if (argc == 6 && std::string(argv[1]) == "--direct")
                return directPath(argv[2], number(argv[3], 512), number(argv[4], 60),
                                  number(argv[5], 1) == 0);
            if (argc == 3 && std::string(argv[1]) == "--inspect") {
                singz::AudioHost inventory;
                for (const auto& device : inventory.enumerate().devices) {
                    if (device.uid != argv[2])
                        continue;
                    emit(@{
                        @"uid" : @(device.uid.c_str()),
                        @"name" : @(device.label.c_str()),
                        @"sampleRate" : @(device.nominalSampleRate),
                        @"bufferFrames" : @(device.bufferFrames.preferredFrames),
                        @"minimumBufferFrames" : @(device.bufferFrames.minimumFrames),
                        @"maximumBufferFrames" : @(device.bufferFrames.maximumFrames),
                        @"inputChannels" : @(device.inputChannels),
                        @"outputChannels" : @(device.outputChannels),
                        @"hardwareChanged" : @NO
                    });
                    return 0;
                }
                return 1;
            }
            const bool rawInput = argc > 1 && std::string(argv[1]) == "--raw-input";
            if (rawInput) {
                --argc;
                ++argv;
            }
            if (argc < 6 || argc > 10)
                return 2;
            const std::string selector(argv[1]);
            const auto sourceDelay = number(argv[2], 4096);
            const auto monitorDelay = number(argv[3], 4096);
            const auto seconds = number(argv[4], 60);
            const auto audible = number(argv[5], 1);
            const auto periodReserve = argc >= 7 ? number(argv[6], 2) : 2;
            const auto requestedPeriod = argc >= 8 ? number(argv[7], 512) : 0;
            const auto quantum = argc >= 9 ? number(argv[8], 256) : 64;
            const auto lead = argc >= 10 ? number(argv[9], 256) : 64;
            if (selector.empty() || seconds < 4 || sourceDelay < 64)
                return 2;
            if ((periodReserve != 1 && periodReserve != 2) ||
                (quantum != 32 && quantum != 64 && quantum != 128 && quantum != 256))
                return 2;

            singz::AudioHost inventory;
            const auto inputs = inventory.enumerate().devices;
            const auto selected =
                std::find_if(inputs.begin(), inputs.end(), [&](const auto& device) {
                    return device.uid == selector;
                });
            if (selected == inputs.end() || selected->inputChannels < 3 ||
                !selected->outputChannels || selected->inputChannels > 64 ||
                selected->outputChannels > 64 || selected->nominalSampleRate != 48000)
                throw std::runtime_error("The selected unchanged 48 kHz device is unavailable");

            TemporaryPeriod period(selector, requestedPeriod);

            VirtualDeviceDefinition device;
            device.id = "unpublished-physical-path-probe";
            device.name = "Private Physical Path Test";
            device.routing.outputChannels = 2;
            device.routing.safetyLimiter = true;
            device.bindings.push_back({"microphone", SourceKind::InputDevice, selector, false});
            device.routing.sources.push_back(
                {"microphone", selected->inputChannels, 1, true, false, {{2, 0, 1}}});
            device.monitors.push_back(
                {"monitor", selector, selected->outputChannels, 0.5f, true, !audible, {{0, 0, 1}}});

            EngineRuntimeOptions options;
            options.scene.blockFrames = quantum;
            options.scene.sourceDelayFrames = sourceDelay;
            options.automaticInputTiming = false; // This probe supplies an exact diagnostic budget.
            options.monitorDelayFrames = monitorDelay;
            options.monitorPeriodReserve = periodReserve;
            options.renderLeadFrames = lead;
            UnpublishedLink link;
            std::shared_ptr<SourceEndpoint> physicalSource;
            const auto factory = captureSourceFactory({128, true});
            EngineRuntime runtime(
                link, options, [&](auto kind, const auto& selector, auto channels) {
                    physicalSource = rawInput
                                         ? std::make_shared<HalInputExperiment>(selector, channels)
                                         : factory(kind, selector, channels);
                    return physicalSource;
                });
            runtime.setDesired({2, {device}});
            runtime.start();

            const auto begun = std::chrono::steady_clock::now();
            std::optional<Counters> baseline;
            while (!stopping) {
                runtime.poll();
                const auto elapsed = std::chrono::steady_clock::now() - begun;
                if (!baseline && elapsed >= std::chrono::seconds(2)) {
                    baseline = counters(runtime);
                    emit(@{
                        @"phase" : @"ready",
                        @"sourceDelayFrames" : @(sourceDelay),
                        @"monitorDelayFrames" : @(monitorDelay),
                        @"alwaysMuted" : @(!audible),
                        @"restored" : @(baseline->runtime.restored)
                    });
                }
                if (elapsed >= std::chrono::seconds(seconds))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            const auto* rawSource = dynamic_cast<HalInputExperiment*>(physicalSource.get());
            if (auto* raw = dynamic_cast<HalInputExperiment*>(physicalSource.get()))
                raw->finishDelivery();
            else if (physicalSource)
                finishInputDelivery(*physicalSource);
            const auto final = counters(runtime);
            runtime.stop();
            const auto restoredPeriod = period.restore();
            const auto delivery = rawSource        ? rawSource->stoppedDelivery()
                                  : physicalSource ? stoppedInputDelivery(*physicalSource)
                                                   : std::nullopt;
            NSMutableArray* deliveryEvents = [NSMutableArray array];
            if (delivery)
                for (uint32_t i = 0; i < delivery->eventCount; ++i) {
                    const auto& event = delivery->events[i];
                    [deliveryEvents addObject:@{
                        @"elapsedNs" : @(event.elapsedNs),
                        @"hostIntervalNs" : @(event.hostIntervalNs),
                        @"callbackIntervalNs" : @(event.callbackIntervalNs),
                        @"processingNs" : @(event.processingNs),
                        @"previousFrames" : @(event.previousFrames),
                        @"frames" : @(event.frames),
                        @"sourceFrameDelta" : @(event.sourceFrameDelta),
                        @"expectedSourceFrameDelta" : @(event.expectedSourceFrameDelta),
                        @"sourceFrameValid" : @(event.sourceFrameValid)
                    }];
                }
            if (!baseline || final.runtime.monitors.size() != 1 ||
                baseline->runtime.monitors.size() != 1)
                throw std::runtime_error("The private monitoring path did not initialize");
            const auto& monitor = final.runtime.monitors[0];
            const auto sourceMissing = final.source.missingFrames - baseline->source.missingFrames;
            const auto monitorMissing =
                monitor.audio.missingFrames - baseline->runtime.monitors[0].audio.missingFrames;
            const auto& render = final.runtime.render;
            const auto& initial = baseline->runtime.render;
            const auto monitorPathPassed =
                !stopping && restoredPeriod && monitor.connected && monitor.complete &&
                monitor.directHardwareClock && monitor.audio.sourceOnline && !monitorMissing &&
                !monitor.audio.invalidBlocks && !monitor.audio.graphFailures &&
                !monitor.hardwareTimestampErrors && !monitor.hardwareDiscontinuities &&
                !monitor.hostXruns;
            const auto passed =
                !stopping && restoredPeriod && final.runtime.restored && final.source.online &&
                monitor.connected && monitor.complete && monitor.audio.sourceOnline &&
                !sourceMissing && !monitorMissing &&
                render.graphFailures == initial.graphFailures &&
                render.sinkFailures == initial.sinkFailures &&
                render.skippedFrames == initial.skippedFrames &&
                render.reanchors == initial.reanchors && !monitor.audio.invalidBlocks && delivery &&
                !delivery->sourceFrameDiscontinuities && !monitor.hardwareTimestampErrors &&
                !monitor.hardwareDiscontinuities && !monitor.hostXruns;
            emit(@{
                @"phase" : @"complete",
                @"passed" : @(passed),
                @"monitorPathPassed" : @(monitorPathPassed),
                @"inputBackend" : rawInput ? @"direct-hal-experiment" : @"auhal",
                @"directHardwareClock" : @(monitor.directHardwareClock),
                @"monitorHardwareTimestampErrors" : @(monitor.hardwareTimestampErrors),
                @"monitorHardwareDiscontinuities" : @(monitor.hardwareDiscontinuities),
                @"monitorHostXruns" : @(monitor.hostXruns),
                @"sampleRate" : @48000,
                @"seconds" : @(seconds),
                @"warmupSeconds" : @2,
                @"alwaysMuted" : @(!audible),
                @"publicDevicePublished" : @NO,
                @"sourceDelayFrames" : @(sourceDelay),
                @"monitorPeriodReserve" : @(periodReserve),
                @"renderBlockFrames" : @(quantum),
                @"renderLeadFrames" : @(lead),
                @"monitorQueueFrames" : @(monitor.queueFrames),
                @"sourceMissingFrames" : @(sourceMissing),
                @"monitorMissingFrames" : @(monitorMissing),
                @"sourceOnline" : @(final.source.online),
                @"renderFrames" : @(render.frames - initial.frames),
                @"lateFrames" : @(render.lateFrames - initial.lateFrames),
                @"skippedFrames" : @(render.skippedFrames - initial.skippedFrames),
                @"reanchors" : @(render.reanchors - initial.reanchors),
                @"graphFailures" : @(render.graphFailures - initial.graphFailures),
                @"sinkFailures" : @(render.sinkFailures - initial.sinkFailures),
                @"monitorInvalidBlocks" : @(monitor.audio.invalidBlocks),
                @"monitorCallbacks" : @(monitor.audio.callbacks),
                @"renderMaximumExecutionNs" : @(render.maximumExecutionNs),
                @"microphonePermissionRequested" : @NO,
                @"inputDeliveryBoundedBeforeTeardown" : @YES,
                @"physicalPeriodRequested" : @(requestedPeriod),
                @"physicalPeriodBefore" : @(period.original()),
                @"physicalPeriodRestored" : @(restoredPeriod),
                @"physicalPeriodAfter" : @(period.current()),
                @"inputDelivery" : delivery ? @{
                    @"events" : deliveryEvents,
                    @"callbacks" : @(delivery->callbacks),
                    @"minimumFrames" : @(delivery->minimumFrames),
                    @"maximumFrames" : @(delivery->maximumFrames),
                    @"minimumEntryAgeNs" : @(delivery->minimumEntryAgeNs),
                    @"maximumEntryAgeNs" : @(delivery->maximumEntryAgeNs),
                    @"minimumReadyEndAgeNs" : @(delivery->minimumReadyEndAgeNs),
                    @"maximumReadyEndAgeNs" : @(delivery->maximumReadyEndAgeNs),
                    @"maximumGapNs" : @(delivery->maximumGapNs),
                    @"maximumProcessingNs" : @(delivery->maximumProcessingNs),
                    @"rejected" : @(delivery->rejected),
                    @"maximumAnchorErrorNs" : @(delivery->maximumAnchorErrorNs),
                    @"unsupportedAnchorIntervals" : @(delivery->unsupportedAnchorIntervals),
                    @"sourceFrameDiscontinuities" : @(delivery->sourceFrameDiscontinuities)
                }
                                            : @{},
                @"error" : @(monitor.error.c_str())
            });
            return passed ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
