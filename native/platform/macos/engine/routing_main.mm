#include "routing_main.hpp"
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/routing_service.hpp>
#include <loopz/macos/editor_service.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/macos/recovery_events.hpp>
#include <loopz/macos/source_diagnostics.hpp>
#import <Foundation/Foundation.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace loopz::macos {
namespace {
volatile sig_atomic_t stopping = 0, retrying = 0;
void stop(int) {
    stopping = 1;
}
void retry(int) {
    retrying = 1;
}
uint32_t number(const std::string& value, uint32_t maximum) {
    char* end = nullptr;
    const auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || !end || *end || parsed > maximum)
        throw std::invalid_argument("Invalid numeric routing option");
    return static_cast<uint32_t>(parsed);
}
NSString* phaseName(RestorationPhase phase) {
    switch (phase) {
    case RestorationPhase::NoDesiredDocument:
        return @"waitingForConfiguration";
    case RestorationPhase::WaitingForDriver:
        return @"waitingForDriver";
    case RestorationPhase::Reconciling:
        return @"reconciling";
    case RestorationPhase::Applying:
        return @"applying";
    case RestorationPhase::Cancelling:
        return @"cancelling";
    case RestorationPhase::Retry:
        return @"retrying";
    case RestorationPhase::Restored:
        return @"configured";
    }
    return @"unknown";
}
void emit(const EngineRuntimeStatus& state, std::span<const DeviceTransport> devices,
          const std::string& serviceError, uint64_t session, const EngineRuntimeOptions& options,
          const EditorOperations* details, const RecoveryEvents* events) {
    NSMutableArray* endpoints = [NSMutableArray array];
    for (const auto& device : devices) {
        const auto view = device.region->view();
        [endpoints addObject:@{
            @"id" : @(device.id.c_str()),
            @"uid" : @(device.uid.c_str()),
            @"channels" : @(view.channels()),
            @"sampleRate" : @(view.sampleRate()),
            @"hasOutput" : @(device.hasOutput),
            @"inputLatencyFrames" : @(device.inputLatencyFrames)
        }];
    }
    NSMutableArray* sources = [NSMutableArray array];
    for (const auto& source : state.sources) {
        NSMutableDictionary* object = [@{
            @"kind" : @(static_cast<uint32_t>(source.kind)),
            @"selector" : @(source.selector.c_str()),
            @"demanded" : @(source.demanded),
            @"connected" : @(source.connected),
            @"complete" : @(source.complete),
            @"attempts" : @(source.attempts),
            @"missingReadFrames" : @(source.missingReadFrames),
            @"error" : @(source.error.c_str())
        } mutableCopy];
        if (source.diagnostics)
            object[@"diagnostics"] = sourceDiagnosticsJSON(*source.diagnostics);
        [sources addObject:object];
    }
    NSMutableArray* monitors = [NSMutableArray array];
    for (const auto& monitor : state.monitors)
        [monitors addObject:@{
            @"device" : @(monitor.device.c_str()),
            @"monitor" : @(monitor.monitor.c_str()),
            @"selector" : @(monitor.selector.c_str()),
            @"connected" : @(monitor.connected),
            @"complete" : @(monitor.complete),
            @"queueFrames" : @(monitor.queueFrames),
            @"directHardwareClock" : @(monitor.directHardwareClock),
            @"rate" : @(monitor.sampleRate),
            @"attempts" : @(monitor.attempts),
            @"missingFrames" : @(monitor.audio.missingFrames),
            @"error" : @(monitor.error.c_str())
        }];
    NSMutableDictionary* object = [@{
        @"mode" : @"routingDocument",
        @"installationValidation" : @(options.installationValidation),
        @"inputResamplingFilter" : options.inputFilter == ResamplingFilter::MinimumPhase
            ? @"minimum-phase"
            : @"linear-phase",
        @"physicalInputDelayFrames" : options.inputDelayFrames ? (id) @(*options.inputDelayFrames)
                                                               : (id)NSNull.null,
        @"fallbackSourceDelayFrames" : @(options.scene.sourceDelayFrames),
        @"automaticApplicationTiming" : @(options.automaticApplicationTiming),
        @"inputTimingPolicy" : options.inputDelayFrames ? @"physical-override"
        : options.automaticInputTiming                  ? @"automatic"
                                                        : @"global-override",
        @"serviceSession" : @(session),
        @"devices" : endpoints,
        @"started" : @(state.started),
        @"restored" : @(state.restored),
        @"phase" : phaseName(state.configuration.phase),
        @"desiredGeneration" : @(state.configuration.desiredGeneration),
        @"activeGeneration" : @(state.configuration.activeGeneration),
        @"revision" : @(state.configuration.transactionRevision),
        @"recoveredBackup" : @(state.configuration.recoveredBackup),
        @"configurationDetail" : @(state.configuration.detail.c_str()),
        @"documentError" : @(state.documentError.c_str()),
        @"controlError" : @(state.controlError.c_str()),
        @"serviceError" : @(serviceError.c_str()),
        @"render" : @{
            @"running" : @(state.render.running),
            @"realtimePolicy" : @(state.render.realtimePolicy),
            @"workgroupJoined" : @(state.render.workgroupJoined),
            @"workgroupError" : @(state.render.workgroupError),
            @"publicationDeadlineMisses" : @(state.render.publicationDeadlineMisses),
            @"maximumPublicationLatenessNs" : @(state.render.maximumPublicationLatenessNs),
            @"error" : @(state.render.error),
            @"blocks" : @(state.render.blocks),
            @"frames" : @(state.render.frames),
            @"lateFrames" : @(state.render.lateFrames),
            @"skippedFrames" : @(state.render.skippedFrames),
            @"reanchors" : @(state.render.reanchors),
            @"graphFailures" : @(state.render.graphFailures),
            @"sinkFailures" : @(state.render.sinkFailures),
            @"maximumExecutionNs" : @(state.render.maximumExecutionNs)
        },
        @"sources" : sources,
        @"monitors" : monitors
    } mutableCopy];
    const auto notifications = events ? events->status() : RecoveryEventStatus{};
    object[@"recoveryEvents"] = @{
        @"enabled" : @(events != nullptr),
        @"audioListeners" : @(notifications.audioListeners),
        @"registrationError" : @(notifications.registrationError)
    };
    if (details) {
        try {
            // Reuse the editor's bounded, atomic meter/counter snapshot. This
            // runs only on the control thread and never records audio samples.
            const auto encoded = details->snapshot();
            NSData* bytes = [NSData dataWithBytes:encoded.data() length:encoded.size()];
            NSDictionary* decoded = [NSJSONSerialization JSONObjectWithData:bytes
                                                                    options:0
                                                                      error:nil];
            id audio = decoded[@"runtime"];
            if (![audio isKindOfClass:NSDictionary.class])
                throw std::runtime_error("Detailed routing status has no runtime snapshot");
            object[@"audio"] = audio;
        } catch (const std::exception& error) {
            // A diagnostic failure must not stop a working audio graph.
            object[@"audioStatusError"] = @(error.what());
        }
    }
    NSData* data = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    if (data) {
        std::fwrite(data.bytes, 1, data.length, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
}
} // namespace
int routingMain(int argc, char** argv) {
    @autoreleasepool {
        try {
            EngineRuntimeOptions options;
            uint32_t seconds = 0;
            bool editorEnabled = false, statusLevels = false;
            bool applicationTimingExplicit = false;
            for (int index = 1; index < argc; ++index) {
                if (index + 1 >= argc)
                    throw std::invalid_argument("Routing options require a value");
                const std::string key = argv[index], value = argv[++index];
                if (key == "--routing-document")
                    options.documentPath = value;
                else if (key == "--installation-validation" && value == "true")
                    options.installationValidation = true;
                else if (key == "--editor-service" && value == "true")
                    editorEnabled = true;
                else if (key == "--status-levels" && value == "true")
                    statusLevels = true;
                else if (key == "--seconds")
                    seconds = number(value, 86400);
                else if (key == "--block-frames")
                    options.scene.blockFrames = number(value, 256);
                else if (key == "--render-rate")
                    options.scene.renderRate = number(value, 192000);
                else if (key == "--source-delay-frames") {
                    options.scene.sourceDelayFrames = number(value, 16384);
                    options.automaticInputTiming = false; // Explicit diagnostic profile.
                } else if (key == "--automatic-application-timing") {
                    if (value != "true" && value != "false")
                        throw std::invalid_argument(
                            "Automatic application timing requires true or false");
                    options.automaticApplicationTiming = value == "true";
                    applicationTimingExplicit = true;
                } else if (key == "--capture-delivery-diagnostics") {
                    if (value != "true" && value != "false")
                        throw std::invalid_argument(
                            "Capture delivery diagnostics requires true or false");
                    options.captureDeliveryDiagnostics = value == "true";
                } else if (key == "--physical-input-delay-frames") {
                    options.inputDelayFrames = number(value, 16384);
                } else if (key == "--experimental-input-filter") {
                    if (value != "linear-phase" && value != "minimum-phase")
                        throw std::invalid_argument(
                            "Experimental input filter requires linear-phase or minimum-phase");
                    options.inputFilter = value == "minimum-phase" ? ResamplingFilter::MinimumPhase
                                                                   : ResamplingFilter::LinearPhase;
                } else if (key == "--application-buffer-frames") {
                    options.applicationBufferFrames = number(value, 128);
                    if (options.applicationBufferFrames != 32 &&
                        options.applicationBufferFrames != 64 &&
                        options.applicationBufferFrames != 128)
                        throw std::invalid_argument(
                            "Application capture buffer must be 32, 64 or 128 frames");
                } else if (key == "--pass-delay-frames") {
                    options.scene.passThroughDelayFrames = number(value, 16384);
                    options.automaticPassThroughTiming = false; // Explicit diagnostic profile.
                } else if (key == "--render-lead-frames")
                    options.renderLeadFrames = number(value, 8192);
                else if (key == "--monitor-delay-frames")
                    options.monitorDelayFrames = number(value, 16384);
                else if (key == "--monitor-period-reserve")
                    options.monitorPeriodReserve = number(value, 2);
                else
                    throw std::invalid_argument("Unknown routing option: " + key);
            }
            // A global diagnostic budget keeps its original meaning. Explicit
            // app selection still rejects contradictory profiles in either order.
            if (!options.automaticInputTiming && !applicationTimingExplicit)
                options.automaticApplicationTiming = false;
            if (options.automaticApplicationTiming && !options.automaticInputTiming)
                throw std::invalid_argument(
                    "Automatic application timing conflicts with a global source-delay override");
            if (options.documentPath.empty())
                throw std::invalid_argument("A routing document path is required");
            if (editorEnabled && options.installationValidation)
                throw std::invalid_argument(
                    "Installation validation cannot expose editor commands");
            if (editorEnabled)
                options.deviceSettingsPath = options.documentPath.string() + ".audio-settings.json";
            InstalledDriverLink link;
            EngineRuntime runtime(link, options);
            EditorOperations details;
            if (statusLevels)
                details = runtimeEditorOperations(runtime);
            std::signal(SIGINT, stop);
            std::signal(SIGTERM, stop);
            std::signal(SIGUSR1, retry);
            runtime.start();
            std::unique_ptr<RecoveryEvents> events;
            if (!options.installationValidation)
                events = std::make_unique<RecoveryEvents>();
            const auto begun = hostNowNs();
            uint64_t nextReport = 0;
            uint64_t nextEditorAttempt = 0;
            std::unique_ptr<EditorService> editor;
            while (!stopping && (!seconds || hostNowNs() - begun <
                                                 static_cast<uint64_t>(seconds) * 1000000000)) {
                @autoreleasepool {
                    if (events) {
                        const auto changed = events->poll(hostNowNs() / 1000000);
                        if (includes(changed, RecoveryEvent::AudioServiceReset))
                            runtime.audioServiceReset();
                        else if (changed != RecoveryEvent::None)
                            runtime.retryNow();
                    }
                    if (retrying) {
                        retrying = 0;
                        runtime.retryNow();
                    }
                    runtime.poll();
                    const auto now = hostNowNs();
                    if (editor && !editor->status().listening)
                        editor.reset();
                    if (editorEnabled && !editor && now >= nextEditorAttempt) {
                        nextEditorAttempt = now + 5000000000;
                        try {
                            editor = EditorService::machService(productionEditorPolicy(),
                                                                runtimeEditorOperations(runtime));
                        } catch (const std::exception& error) {
                            std::fprintf(stderr, "Editor connection: %s\n", error.what());
                        }
                    }
                    if (editor)
                        editor->poll();
                    if (now >= nextReport) {
                        emit(runtime.status(), runtime.devices(), link.lastError(), link.session(),
                             options, statusLevels ? &details : nullptr, events.get());
                        nextReport = now + 1000000000;
                    }
                    [[NSRunLoop currentRunLoop]
                        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
                }
            }
            editor.reset();
            runtime.stop();
            emit(runtime.status(), runtime.devices(), link.lastError(), link.session(), options,
                 statusLevels ? &details : nullptr, events.get());
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Routing engine: %s\n", error.what());
            return 2;
        }
    }
}
} // namespace loopz::macos
