#include <loopz/macos/capture.hpp>
#include <loopz/macos/capture_sources.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/editor_service.hpp>
#include <loopz/macos/permissions.hpp>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace loopz;
using namespace loopz::macos;

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
    uint64_t revision_{0};
};

uint32_t number(std::string_view text, uint32_t minimum, uint32_t maximum) {
    uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum ||
        value > maximum)
        throw std::invalid_argument("Invalid bounded input diagnostic option");
    return value;
}

NSDictionary* readDiagnostics(const AudioHistoryReadSnapshot& value) {
    auto* missing = [NSMutableDictionary dictionary];
    constexpr std::array names{"invalidRequest", "offline",           "anchorUnavailable",
                               "clockMapping",   "resamplingSupport", "generationChanged"};
    static_assert(names.size() == static_cast<size_t>(AudioHistoryMissingReason::Count));
    for (size_t index = 0; index < names.size(); ++index)
        missing[@(names[index])] = @(value.missingFrames[index]);
    return @{
        @"enabled" : @(value.enabled),
        @"reads" : @(value.reads),
        @"requestedFrames" : @(value.requestedFrames),
        @"missingOutputFrames" : missing,
        @"unpublishedSampleReads" : @(value.unpublishedSampleReads),
        @"expiredSampleReads" : @(value.expiredSampleReads),
        @"racingSampleReads" : @(value.racingSampleReads),
        @"absentSampleReads" : @(value.absentSampleReads)
    };
}

AudioHistoryReadSnapshot since(const AudioHistoryReadSnapshot& value,
                               const AudioHistoryReadSnapshot& baseline) {
    AudioHistoryReadSnapshot result = value;
    result.reads -= baseline.reads;
    result.requestedFrames -= baseline.requestedFrames;
    for (size_t index = 0; index < result.missingFrames.size(); ++index)
        result.missingFrames[index] -= baseline.missingFrames[index];
    result.unpublishedSampleReads -= baseline.unpublishedSampleReads;
    result.expiredSampleReads -= baseline.expiredSampleReads;
    result.racingSampleReads -= baseline.racingSampleReads;
    result.absentSampleReads -= baseline.absentSampleReads;
    return result;
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if (argc != 4 && (argc != 5 || std::string_view(argv[4]) != "--snapshots"))
                throw std::invalid_argument("Expected input UID, history frames (0=auto), seconds "
                                            "and optional --snapshots");
            const std::string uid(argv[1]);
            const auto delay = number(argv[2], 0, 4096), seconds = number(argv[3], 4, 30);
            if (delay && delay < 64)
                throw std::invalid_argument("Explicit history must be at least 64 frames");
            if (microphonePermission() != MicrophonePermission::Authorized)
                throw std::runtime_error("Existing microphone grant required; no prompt requested");
            singz::AudioHost inventory;
            const auto devices = inventory.enumerate().devices;
            const auto selected =
                std::find_if(devices.begin(), devices.end(), [&](const auto& item) {
                    return item.uid == uid;
                });
            if (selected == devices.end() || !selected->inputChannels ||
                selected->inputChannels > 64 || selected->nominalSampleRate != 48000)
                throw std::runtime_error("Exact unchanged 48 kHz physical input required");
            const auto period = selected->bufferFrames.preferredFrames;
            VirtualDeviceDefinition device;
            device.id = device.name = "unpublished-input-delivery";
            device.routing.sources = {{"input", selected->inputChannels, 1, true, false, {}}};
            device.bindings = {{"input", SourceKind::InputDevice, uid, false}};
            EngineRuntimeOptions options;
            options.scene.blockFrames = 64;
            options.scene.sourceDelayFrames = delay ? delay : 1024;
            options.automaticInputTiming = !delay;
            options.automaticApplicationTiming = false;
            UnpublishedLink link;
            std::shared_ptr<SourceEndpoint> capture;
            const auto factory = captureSourceFactory({128, true});
            EngineRuntime runtime(link, options,
                                  [&](auto kind, const auto& selector, auto channels) {
                                      capture = factory(kind, selector, channels);
                                      return capture;
                                  });
            runtime.setDesired({kRouteDocumentVersion, {device}});
            runtime.setEditorPreview(true);
            runtime.start();
            const auto editor = runtimeEditorOperations(runtime);
            uint64_t nextSnapshot = 0, snapshots = 0, nextCounter = 0;
            auto* readCounters = [NSMutableArray array];
            const auto start = hostNowNs();
            std::optional<uint64_t> baseline;
            std::optional<AudioHistoryReadSnapshot> baselineReads;
            while (hostNowNs() - start < seconds * 1000000000ULL) {
                runtime.poll();
                if (hostNowNs() >= nextCounter) {
                    const auto current = runtime.status();
                    if (!current.sources.empty())
                        [readCounters addObject:@{
                            @"elapsedNs" : @(hostNowNs() - start),
                            @"missingReadFrames" : @(current.sources[0].missingReadFrames),
                            @"connected" : @(current.sources[0].connected)
                        }];
                    nextCounter = hostNowNs() + 250000000;
                }
                if (argc == 5 && hostNowNs() >= nextSnapshot) {
                    (void)editor.snapshot();
                    ++snapshots;
                    nextSnapshot = hostNowNs() + 250000000;
                }
                if (!baseline && hostNowNs() - start >= 2000000000ULL &&
                    !runtime.status().sources.empty()) {
                    baseline = runtime.status().sources[0].missingReadFrames;
                    if (capture)
                        baselineReads = inputHistoryReadDiagnostics(*capture);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto status = runtime.status();
            const auto scene = runtime.observe();
            const auto appliedDelay = scene ? scene->deviceLatencyFrames(0) : 0;
            if (!capture || !baseline || status.sources.size() != 1)
                throw std::runtime_error("Input capture did not initialize");
            finishInputDelivery(*capture);
            runtime.stop();
            const auto delivery = stoppedInputDelivery(*capture);
            const auto finalReads = inputHistoryReadDiagnostics(*capture);
            if (!delivery || !baselineReads || !finalReads)
                throw std::runtime_error("Joined input delivery statistics unavailable");
            const auto missing = status.sources[0].missingReadFrames - *baseline;
            auto* events = [NSMutableArray array];
            for (uint32_t index = 0; index < delivery->eventCount; ++index) {
                const auto& event = delivery->events[index];
                [events addObject:@{
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
            const auto readDelta = since(*finalReads, *baselineReads);
            const bool passed = status.restored && !missing && !delivery->rejected &&
                                delivery->callbacks && !delivery->sourceFrameDiscontinuities &&
                                !delivery->unsupportedAnchorIntervals &&
                                !status.render.publicationDeadlineMisses && readDelta.reads &&
                                std::all_of(readDelta.missingFrames.begin(),
                                            readDelta.missingFrames.end(), [](auto frames) {
                                                return frames == 0;
                                            });
            NSDictionary* result = @{
                @"passed" : @(passed),
                @"physicalInput" : @(uid.c_str()),
                @"physicalOutputsOpened" : @NO,
                @"publicDevicesPublished" : @NO,
                @"permissionRequested" : @NO,
                @"bufferChanged" : @NO,
                @"audioSamplesSaved" : @NO,
                @"seconds" : @(seconds),
                @"historyFrames" : @(delay),
                @"automaticInputTiming" : @(!delay),
                @"appliedHistoryFrames" : @(appliedDelay),
                @"controlSnapshots" : @(snapshots),
                @"hardwareBufferFrames" : @(period),
                @"sourceMissingReadFrames" : @(missing),
                @"warmupMissingReadFrames" : @(*baseline),
                @"totalMissingReadFrames" : @(status.sources[0].missingReadFrames),
                @"readCounterObservations" : readCounters,
                @"historyReadsAtBaseline" : readDiagnostics(*baselineReads),
                @"historyReadsAfterJoin" : readDiagnostics(*finalReads),
                @"historyReadsAfterWarmup" : readDiagnostics(readDelta),
                @"sourceAttempts" : @(status.sources[0].attempts),
                @"callbacks" : @(delivery->callbacks),
                @"frames" : @(delivery->frames),
                @"callbackFrames" : @[ @(delivery->minimumFrames), @(delivery->maximumFrames) ],
                @"entryAgeNs" : @[ @(delivery->minimumEntryAgeNs), @(delivery->maximumEntryAgeNs) ],
                @"readyEndAgeNs" :
                    @[ @(delivery->minimumReadyEndAgeNs), @(delivery->maximumReadyEndAgeNs) ],
                @"callbackGapNs" : @[ @(delivery->minimumGapNs), @(delivery->maximumGapNs) ],
                @"maximumProcessingNs" : @(delivery->maximumProcessingNs),
                @"unsupportedAnchorIntervals" : @(delivery->unsupportedAnchorIntervals),
                @"sourceFrameDiscontinuities" : @(delivery->sourceFrameDiscontinuities),
                @"maximumAnchorErrorNs" : @(delivery->maximumAnchorErrorNs),
                @"events" : events,
                @"publicationDeadlineMisses" : @(status.render.publicationDeadlineMisses),
                @"maximumPublicationLatenessNs" : @(status.render.maximumPublicationLatenessNs)
            };
            NSData* data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
            if (!data || std::fwrite(data.bytes, 1, data.length, stdout) != data.length)
                throw std::runtime_error("Input diagnostic report failed");
            std::fputc('\n', stdout);
            return passed ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 2;
        }
    }
}
