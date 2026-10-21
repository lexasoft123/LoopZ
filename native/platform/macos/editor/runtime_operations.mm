#import <Foundation/Foundation.h>
#include <loopz/macos/editor_service.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/macos/permissions.hpp>
#include <loopz/sample_rates.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/macos/source_diagnostics.hpp>
#include <algorithm>

namespace loopz::macos {
namespace {
NSString* text(const std::string& value) {
    auto* result = [[NSString alloc] initWithBytes:value.data()
                                            length:value.size()
                                          encoding:NSUTF8StringEncoding];
    return result ? result : @"Unavailable";
}
NSString* counter(uint64_t value) {
    return text(std::to_string(value));
}
NSString* kind(SourceKind value) {
    switch (value) {
    case SourceKind::Application:
        return @"application";
    case SourceKind::InputDevice:
        return @"input-device";
    case SourceKind::VirtualDevice:
        return @"virtual-device";
    case SourceKind::PassThrough:
        return @"pass-through";
    }
    throw std::invalid_argument("Unknown runtime source kind");
}

NSDictionary* meterJSON(const AudioMeterSnapshot& value, bool valid) {
    auto* peak = [NSMutableArray array];
    auto* rms = [NSMutableArray array];
    if (valid)
        for (uint32_t channel = 0; channel < value.channels; ++channel) {
            [peak addObject:@(value.peak[channel])];
            [rms addObject:@(value.rms[channel])];
        }
    return @{
        @"valid" : @(valid),
        @"channels" : @(valid ? value.channels : 0),
        @"frames" : @(valid ? value.frames : 0),
        @"peak" : peak,
        @"rms" : rms
    };
}
NSDictionary* meter(const AudioScene& scene, size_t device, std::optional<size_t> source,
                    bool fresh) {
    AudioMeterSnapshot value;
    const bool valid =
        fresh &&
        (source ? scene.sourceMeters(device, *source, value) : scene.deviceMeters(device, value)) &&
        value.frames;
    return meterJSON(value, valid);
}

NSDictionary* transportCounters(const EngineRuntime& runtime, const std::string& device) {
    const auto& transports = runtime.devices();
    const auto found = std::find_if(transports.begin(), transports.end(), [&](const auto& item) {
        return item.id == device;
    });
    if (found == transports.end() || !found->region)
        return @{@"available" : @NO};
    // The control owner pins this region. Driver reads and scene source reads
    // are different boundaries; a healthy source does not certify public IO.
    const auto& header = found->region->view().header();
    return @{
        @"available" : @YES,
        @"generation" : counter(header.generation),
        @"captureReadFrames" : counter(header.captureReadFrames.load()),
        @"captureMissingFrames" : counter(header.captureMissingFrames.load()),
        @"playbackReadFrames" : counter(header.playbackReadFrames.load()),
        @"playbackMissingFrames" : counter(header.playbackMissingFrames.load())
    };
}

std::string runtimeSnapshot(EngineRuntime& runtime) {
    const auto snapshot = runtime.editorSnapshot();
    const auto encoded = encodeEditorSnapshot(snapshot);
    NSMutableDictionary* result = [NSJSONSerialization
        JSONObjectWithData:[NSData dataWithBytes:encoded.data() length:encoded.size()]
                   options:NSJSONReadingMutableContainers
                     error:nil];
    if (!result)
        throw std::runtime_error("Editor snapshot could not be assembled");
    const auto state = runtime.status();
    const auto now = hostNowNs();
    const bool fresh = state.render.running && !state.render.error &&
                       state.render.lastBlockHostNs && now >= state.render.lastBlockHostNs &&
                       now - state.render.lastBlockHostNs <= 250000000;
    auto* devices = [NSMutableArray array];
    auto scene = runtime.observe();
    if (scene)
        for (size_t index = 0; index < scene->document().devices.size(); ++index) {
            const auto& definition = scene->document().devices[index];
            const auto status = scene->deviceStatus(index);
            auto* sources = [NSMutableArray array];
            for (size_t source = 0; source < definition.routing.sources.size(); ++source) {
                const auto sourceStatus = scene->sourceStatus(index, source);
                [sources addObject:@{
                    @"id" : text(definition.routing.sources[source].id),
                    @"online" : @(sourceStatus.online),
                    @"missingFrames" : counter(sourceStatus.missingFrames),
                    @"historyBudgetFrames" : sourceStatus.historyBudgetFrames
                        ? (id) @(*sourceStatus.historyBudgetFrames)
                        : (id)NSNull.null,
                    @"resamplingFilter" : sourceStatus.resamplingFilter
                        ? (id)(*sourceStatus.resamplingFilter == ResamplingFilter::LinearPhase
                                   ? @"linear-phase"
                                   : @"minimum-phase")
                        : (id)NSNull.null,
                    @"meter" : meter(*scene.get(), index, source, fresh)
                }];
            }
            [devices addObject:@{
                @"id" : text(definition.id),
                @"channels" : @(definition.routing.outputChannels),
                @"active" : @(status.active),
                @"renderedFrames" : counter(status.renderedFrames),
                @"graphFailures" : counter(status.graphFailures),
                @"sinkFailures" : counter(status.sinkFailures),
                @"latencyFrames" : @(scene->deviceLatencyFrames(index)),
                @"renderRate" : @(scene->timing().renderRate),
                @"transport" : transportCounters(runtime, definition.id),
                @"meter" : meter(*scene.get(), index, std::nullopt, fresh),
                @"sources" : sources
            }];
        }
    auto* sources = [NSMutableArray array];
    for (const auto& source : state.sources) {
        NSMutableDictionary* object = [@{
            @"kind" : kind(source.kind),
            @"selector" : text(source.selector),
            @"demanded" : @(source.demanded),
            @"connected" : @(source.connected),
            @"complete" : @(source.complete),
            @"error" : text(source.error),
            @"attempts" : counter(source.attempts),
            @"missingReadFrames" : counter(source.missingReadFrames)
        } mutableCopy];
        if (source.diagnostics)
            object[@"diagnostics"] = sourceDiagnosticsJSON(*source.diagnostics);
        [sources addObject:object];
    }
    auto* monitors = [NSMutableArray array];
    for (const auto& monitor : state.monitors) {
        AudioMeterSnapshot value;
        const auto nowMs = now / 1000000;
        const bool current =
            monitor.connected && monitor.complete && monitor.lastCallbackMs &&
            nowMs >= monitor.lastCallbackMs && nowMs - monitor.lastCallbackMs <= 250 &&
            runtime.monitorMeters(monitor.device, monitor.monitor, value) && value.frames;
        [monitors addObject:@{
            @"device" : text(monitor.device),
            @"monitor" : text(monitor.monitor),
            @"selector" : text(monitor.selector),
            @"connected" : @(monitor.connected),
            @"complete" : @(monitor.complete),
            @"queueFrames" : @(monitor.queueFrames),
            @"directHardwareClock" : @(monitor.directHardwareClock),
            @"bufferFrames" : @(monitor.bufferFrames),
            @"sampleRate" : @(monitor.sampleRate),
            @"error" : text(monitor.error),
            @"attempts" : counter(monitor.attempts),
            @"audio" : @{
                @"callbacks" : counter(monitor.audio.callbacks),
                @"missingFrames" : counter(monitor.audio.missingFrames),
                @"graphFailures" : counter(monitor.audio.graphFailures),
                @"invalidBlocks" : counter(monitor.audio.invalidBlocks),
                @"sourceOnline" : @(monitor.audio.sourceOnline),
                @"measuredOutputRate" : @(monitor.audio.measuredOutputRate),
                @"hardwareTimestampErrors" : counter(monitor.hardwareTimestampErrors),
                @"hardwareDiscontinuities" : counter(monitor.hardwareDiscontinuities),
                @"hostXruns" : counter(monitor.hostXruns)
            },
            @"meter" : meterJSON(value, current)
        }];
    }
    auto* sampleRates = [NSMutableArray array];
    for (const auto rate : kVirtualSampleRates)
        if (state.sampleRateMask & virtualSampleRateBit(rate))
            [sampleRates addObject:@(rate)];
    const auto timingBatch = runtime.publicTimingObservations();
    auto* timingDevices = [NSMutableArray array];
    if (timingBatch)
        for (const auto& [uid, observation] : timingBatch->devices) {
            const auto& value = observation.value;
            [timingDevices addObject:@{
                @"uid" : text(uid),
                @"observed" : @(value.has_value()),
                @"bufferFrames" : @(value ? value->bufferFrames : 0),
                @"running" : @(value && value->running),
                @"measured" : @(value && value->measured),
                @"session" : @(value ? value->session : 0),
                @"error" : text(observation.error)
            }];
        }
    result[@"runtime"] = @{
        @"publicTiming" : @{
            @"batchRevision" : counter(timingBatch ? timingBatch->revision : 0),
            @"devices" : timingDevices
        },
        @"editCapabilities" : @{@"removeOutputPair" : @YES},
        @"sampleRates" : sampleRates,
        @"started" : @(state.started),
        @"restored" : @(state.restored),
        @"rendering" : @(state.render.running),
        @"metersFresh" : @(fresh),
        @"activeGeneration" : counter(state.configuration.activeGeneration),
        @"documentError" : text(state.documentError),
        @"controlError" : text(state.controlError),
        @"renderError" : @(state.render.error),
        @"renderBlocks" : counter(state.render.blocks),
        @"renderTiming" : @{
            @"workgroupJoined" : @(state.render.workgroupJoined),
            @"workgroupError" : @(state.render.workgroupError),
            @"publicationDeadlineMisses" : counter(state.render.publicationDeadlineMisses),
            @"maximumPublicationLatenessNs" : counter(state.render.maximumPublicationLatenessNs),
            @"maximumExecutionNs" : counter(state.render.maximumExecutionNs)
        },
        @"devices" : devices,
        @"sources" : sources,
        @"monitors" : monitors,
        @"permissions" : @{@"microphone" : @(permissionName(microphonePermission()))}
    };
    const auto hardware = runtime.hardwareSnapshot();
    result[@"hardware"] = [NSJSONSerialization
        JSONObjectWithData:[NSData dataWithBytes:hardware.data() length:hardware.size()]
                   options:0
                     error:nil];
    NSData* data = [NSJSONSerialization dataWithJSONObject:result
                                                   options:NSJSONWritingSortedKeys
                                                     error:nil];
    if (!data || data.length > kMaximumEditorReplyBytes)
        throw std::runtime_error("Editor runtime snapshot exceeds its payload bound");
    return {static_cast<const char*>(data.bytes), data.length};
}
} // namespace

EditorOperations runtimeEditorOperations(EngineRuntime& runtime) {
    return {[&runtime] {
                @autoreleasepool {
                    return runtimeSnapshot(runtime);
                }
            },
            [&runtime](std::string_view json) {
                const auto request = decodeEditorRequest(json);
                if (request.history)
                    return encodeEditorCommit(
                        runtime.moveEditorHistory(request.expected, *request.history));
                return encodeEditorCommit(runtime.applyEditor(request.expected, request.commands));
            },
            [&runtime] {
                runtime.retryNow();
            },
            [&runtime](std::string_view json) {
                const auto request = decodeEditorRequest(json);
                if (request.history)
                    throw std::invalid_argument("History cannot initialize a routing document");
                return encodeEditorCommit(
                    runtime.initializeEditor(request.expected, request.commands));
            },
            [&runtime](bool enabled) {
                runtime.setEditorPreview(enabled);
            },
            [&runtime](std::string_view request) {
                @autoreleasepool {
                    return runtime.applyHardwareBuffer(request);
                }
            }};
}
} // namespace loopz::macos
