#include <loopz/macos/capture.hpp>
#include <loopz/macos/capture_sources.hpp>
#include <loopz/macos/editor_service.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/permissions.hpp>
#include <loopz/macos/recovery_events.hpp>
#include "silent_monitor_backend.hpp"
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
using namespace loopz;
using namespace loopz::macos;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

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
    uint64_t revision_{};
};

std::optional<singz::AudioHostDeviceInfo> device(const std::string& uid) {
    singz::AudioHost inventory;
    const auto devices = inventory.enumerate().devices;
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& item) {
        return item.uid == uid;
    });
    return found == devices.end() ? std::nullopt : std::optional(*found);
}

// Only this process can see the aggregate, and only this owner destroys it.
// Removing/recreating it exercises native UID loss and replacement handles;
// it does not simulate USB firmware, power loss or sleep/wake.
class PrivateInputDevice {
  public:
    explicit PrivateInputDevice(std::string physical)
        : physical_(std::move(physical)),
          uid_("com.lexasoft.loopz.test-input." + std::string(NSUUID.UUID.UUIDString.UTF8String)) {}
    ~PrivateInputDevice() {
        if (id_)
            AudioHardwareDestroyAggregateDevice(id_);
    }
    const std::string& uid() const noexcept {
        return uid_;
    }
    AudioObjectID id() const noexcept {
        return id_;
    }
    void create() {
        require(!id_, "Private input already exists");
        NSDictionary* description = @{
            @kAudioAggregateDeviceNameKey : @"LoopZ private input recovery check",
            @kAudioAggregateDeviceUIDKey : @(uid_.c_str()),
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceMainSubDeviceKey : @(physical_.c_str()),
            @kAudioAggregateDeviceSubDeviceListKey : @[
                @{@kAudioSubDeviceUIDKey : @(physical_.c_str())}
            ]
        };
        require(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description, &id_) ==
                        noErr &&
                    id_,
                "Private input creation failed");
    }
    void remove() {
        require(id_ && AudioHardwareDestroyAggregateDevice(id_) == noErr,
                "Private input removal failed");
        id_ = 0;
    }

  private:
    const std::string physical_, uid_;
    AudioObjectID id_{};
};

VirtualDeviceDefinition input(std::string id, const std::string& uid, uint32_t channels) {
    VirtualDeviceDefinition result;
    result.id = result.name = std::move(id);
    result.routing.outputChannels = channels;
    SourceRouting source{"input", channels, 1, true, false, {}};
    for (uint32_t index = 0; index < channels; ++index)
        source.connections.push_back({index, index, 1});
    result.routing.sources.push_back(std::move(source));
    result.bindings = {{"input", SourceKind::InputDevice, uid, false}};
    return result;
}

RouteDocument graph(const std::string& target, const std::string& peer, bool monitors) {
    auto affected = input("affected", target, 8);
    auto healthy = input("healthy", peer, 16);
    if (monitors) {
        affected.monitors = {{"silent", target, 2, 0, true, true, {{2, 0, 1}, {3, 1, 1}}}};
        healthy.monitors = {{"silent", peer, 2, 0, true, true, {{2, 0, 1}}}};
    }
    VirtualDeviceDefinition dependency;
    dependency.id = dependency.name = "dependency";
    dependency.routing.sources = {{"upstream", 8, 1, true, false, {{2, 0, 1}, {3, 1, 1}}}};
    dependency.bindings = {{"upstream", SourceKind::VirtualDevice, "affected", false}};
    VirtualDeviceDefinition mixed;
    mixed.id = mixed.name = "mixed";
    mixed.routing.sources = {{"return", 2, 1, true, false, {{0, 1, 1}}},
                             {"peer", 16, 1, true, false, {{2, 0, 1}}}};
    mixed.bindings = {{"return", SourceKind::VirtualDevice, "dependency", false},
                      {"peer", SourceKind::VirtualDevice, "healthy", false}};
    return {kRouteDocumentVersion, {affected, healthy, dependency, mixed}};
}

SourceConnectionStatus source(const EngineRuntime& runtime, const std::string& uid) {
    const auto status = runtime.status();
    const auto found =
        std::find_if(status.sources.begin(), status.sources.end(), [&](const auto& item) {
            return item.kind == SourceKind::InputDevice && item.selector == uid;
        });
    require(found != status.sources.end(), "Expected physical provider disappeared from intent");
    return *found;
}

MonitorConnectionStatus monitor(const EngineRuntime& runtime, const std::string& id) {
    const auto status = runtime.status();
    const auto found =
        std::find_if(status.monitors.begin(), status.monitors.end(), [&](const auto& item) {
            return item.device == id && item.monitor == "silent";
        });
    require(found != status.monitors.end(), "Expected monitor disappeared from saved intent");
    return *found;
}

NSDictionary* snapshot(const EditorOperations& editor) {
    const auto value = editor.snapshot();
    NSData* data = [NSData dataWithBytes:value.data() length:value.size()];
    NSDictionary* result = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    require([result isKindOfClass:NSDictionary.class], "Runtime snapshot is invalid");
    return result;
}

NSDictionary* renderedDevice(NSDictionary* value, NSString* id) {
    for (NSDictionary* item in value[@"runtime"][@"devices"])
        if ([item[@"id"] isEqual:id])
            return item;
    throw std::runtime_error("Expected rendered device is absent");
}

bool silent(NSArray* peaks) {
    require([peaks isKindOfClass:NSArray.class] && peaks.count, "Expected channel peaks");
    for (NSNumber* peak in peaks)
        if (peak.doubleValue != 0)
            return false;
    return true;
}

class Check {
  public:
    Check(EngineRuntime& runtime, std::string target, std::string peer, bool monitors)
        : runtime_(runtime), target_(std::move(target)), peer_(std::move(peer)),
          editor_(runtimeEditorOperations(runtime)), monitors_(monitors) {}

    void poll() {
        const auto event = events_.poll(hostNowNs() / 1000000);
        require(!includes(event, RecoveryEvent::AudioServiceReset),
                "External audio service reset during test");
        if (includes(event, RecoveryEvent::AudioInventory)) {
            ++inventoryEvents;
            runtime_.retryNow();
        }
        runtime_.poll();
        if (peerBaseline_) {
            const auto current = source(runtime_, peer_);
            require(current.connected && current.complete &&
                        current.attempts == peerBaseline_->attempts,
                    "Unaffected physical provider reopened or disconnected");
            require(current.missingReadFrames == peerBaseline_->missingReadFrames,
                    "Unaffected physical provider lost history frames");
        }
        if (monitorBaseline_) {
            const auto current = monitor(runtime_, "healthy");
            const auto& baseline = *monitorBaseline_;
            require(current.connected && current.complete && current.attempts == baseline.attempts,
                    "Unaffected physical monitor reopened or disconnected");
            require(current.audio.callbacks >= baseline.audio.callbacks &&
                        current.audio.missingFrames == baseline.audio.missingFrames &&
                        current.audio.invalidBlocks == baseline.audio.invalidBlocks &&
                        current.audio.graphFailures == baseline.audio.graphFailures &&
                        current.hardwareTimestampErrors == baseline.hardwareTimestampErrors &&
                        current.hardwareDiscontinuities == baseline.hardwareDiscontinuities &&
                        current.hostXruns == baseline.hostXruns,
                    "Unaffected physical monitor reported an audio discontinuity");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    template <class Condition> void wait(Condition condition) {
        const auto deadline = hostNowNs() + 6000000000ULL;
        do {
            poll();
            if (condition())
                return;
        } while (hostNowNs() < deadline);
        throw std::runtime_error("Native device recovery phase timed out");
    }

    void observe(uint32_t milliseconds) {
        const auto end = hostNowNs() + milliseconds * 1000000ULL;
        while (hostNowNs() < end)
            poll();
    }

    void baseline() {
        wait([&] {
            const auto a = source(runtime_, target_), b = source(runtime_, peer_);
            return runtime_.status().restored && a.connected && a.complete && b.connected &&
                   b.complete;
        });
        observe(2000);
        peerBaseline_ = source(runtime_, peer_);
        if (monitors_)
            monitorBaseline_ = monitor(runtime_, "healthy");
    }

    NSDictionary* inspect(bool online) {
        auto* value = snapshot(editor_);
        auto* affected = renderedDevice(value, @"affected");
        NSDictionary* meter = affected[@"sources"][0][@"meter"];
        require([meter[@"valid"] boolValue] && [meter[@"channels"] unsignedIntValue] == 8,
                "Affected physical source meter layout changed");
        require([affected[@"sources"][0][@"online"] boolValue] == online,
                "Affected source online state is incorrect");
        if (!online) {
            require(silent(meter[@"peak"]), "Disconnected physical source was not silent");
            require(silent(renderedDevice(value, @"dependency")[@"meter"][@"peak"]),
                    "Disconnected virtual dependency was not silent");
        } else {
            require(!silent(meter[@"peak"]), "Restored physical input has no observed signal");
        }
        NSDictionary* healthy = renderedDevice(value, @"healthy")[@"sources"][0];
        require([healthy[@"online"] boolValue] && [healthy[@"meter"][@"valid"] boolValue] &&
                    [healthy[@"meter"][@"channels"] unsignedIntValue] == 16,
                "Unaffected source meter stopped");
        require(!silent(healthy[@"meter"][@"peak"]), "Unaffected source has no observed signal");
        require(runtime_.status().restored == online, "Full graph recovery state is incorrect");
        if (monitors_) {
            const auto affectedMonitor = monitor(runtime_, "affected");
            require(affectedMonitor.connected == online && affectedMonitor.complete == online,
                    "Affected monitor recovery state is incorrect");
            const auto healthyMonitor = monitor(runtime_, "healthy");
            require(healthyMonitor.connected && healthyMonitor.complete &&
                        healthyMonitor.bufferFrames == 512,
                    "Unaffected monitor changed hardware period or stopped");
            if (online)
                require(affectedMonitor.bufferFrames == 512,
                        "Restored monitor changed hardware period");
        }
        return value;
    }

    uint64_t inventoryEvents{};

  private:
    EngineRuntime& runtime_;
    const std::string target_, peer_;
    EditorOperations editor_;
    RecoveryEvents events_;
    std::optional<SourceConnectionStatus> peerBaseline_;
    const bool monitors_;
    std::optional<MonitorConnectionStatus> monitorBaseline_;
};

void report(NSDictionary* value) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    require(data && std::fwrite(data.bytes, 1, data.length, stdout) == data.length,
            "Device recovery report failed");
    std::fputc('\n', stdout);
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            require(argc == 3 || (argc == 4 && std::string_view(argv[3]) == "--silent-monitors"),
                    "Expected physical UID, peer UID and optional --silent-monitors");
            const bool monitors = argc == 4;
            const std::string physical(argv[1]), peer(argv[2]);
            require(physical != peer, "Recovery check needs independent physical devices");
            require(microphonePermission() == MicrophonePermission::Authorized,
                    "Existing microphone grant required; no prompt requested");
            const auto originalTarget = device(physical), originalPeer = device(peer);
            require(originalTarget && originalPeer && originalTarget->inputChannels == 8 &&
                        originalPeer->inputChannels == 16 &&
                        originalTarget->nominalSampleRate == 48000 &&
                        originalPeer->nominalSampleRate == 48000 &&
                        originalTarget->bufferFrames.preferredFrames == 512 &&
                        originalPeer->bufferFrames.preferredFrames == 512,
                    "Exact original 48 kHz, 512-frame physical devices required");
            PrivateInputDevice temporary(physical);
            temporary.create();
            const auto firstID = temporary.id();
            UnpublishedLink link;
            EngineRuntimeOptions options;
            options.scene.blockFrames = 64;
            options.captureDeliveryDiagnostics = true;
            options.automaticApplicationTiming = false;
            auto counters = std::make_shared<diagnostic::SilentMonitorCounters>();
            MonitorOutputs::HostFactory outputs;
            if (monitors)
                outputs = [&] {
                    return std::make_unique<singz::AudioHost>(
                        std::make_unique<diagnostic::SilentMonitorBackend>(temporary.uid(), peer,
                                                                           counters));
                };
            EngineRuntime runtime(link, options, {}, outputs);
            runtime.setDesired(graph(temporary.uid(), peer, monitors));
            runtime.setEditorPreview(true);
            runtime.start();
            Check check(runtime, temporary.uid(), peer, monitors);
            check.baseline();
            auto* before = check.inspect(true);
            const auto attempts = source(runtime, temporary.uid()).attempts;
            temporary.remove();
            check.wait([&] {
                return !device(temporary.uid()) && !source(runtime, temporary.uid()).connected &&
                       (!monitors || !monitor(runtime, "affected").connected);
            });
            check.observe(500);
            auto* absent = check.inspect(false);
            const auto removedAt = hostNowNs();
            temporary.create();
            const auto secondID = temporary.id();
            check.wait([&] {
                return source(runtime, temporary.uid()).connected &&
                       source(runtime, temporary.uid()).complete && runtime.status().restored;
            });
            const auto recoveryNs = hostNowNs() - removedAt;
            check.observe(2000);
            auto* restored = check.inspect(true);
            require([before[@"document"] isEqual:absent[@"document"]] &&
                        [before[@"document"] isEqual:restored[@"document"]],
                    "Saved graph intent changed during device recovery");
            require(source(runtime, temporary.uid()).attempts > attempts,
                    "Physical provider was not reacquired after device replacement");
            require(check.inventoryEvents >= 2, "Native device inventory events were not observed");
            runtime.stop();
            require(!counters->nonzeroSamples.load() && !counters->invalidBlocks.load(),
                    "Muted monitor renderer violated the silence boundary");
            if (monitors)
                require(counters->frames.load() > 96000 && counters->opens.load() >= 3,
                        "Native monitor callbacks or reacquisition were not observed");
            temporary.remove();
            const auto cleanupDeadline = hostNowNs() + 5000000000ULL;
            while (device(temporary.uid()) && hostNowNs() < cleanupDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(!device(temporary.uid()), "Private device remained after cleanup");
            const auto afterTarget = device(physical), afterPeer = device(peer);
            require(afterTarget && afterPeer && afterTarget->bufferFrames.preferredFrames == 512 &&
                        afterPeer->bufferFrames.preferredFrames == 512 &&
                        afterTarget->nominalSampleRate == 48000 &&
                        afterPeer->nominalSampleRate == 48000,
                    "Physical hardware buffer or rate changed");
            report(@{
                @"passed" : @YES,
                @"privateInputUID" : @(temporary.uid().c_str()),
                @"firstObjectID" : @(firstID),
                @"replacementObjectID" : @(secondID),
                @"recoveryNs" : @(recoveryNs),
                @"inventoryEvents" : @(check.inventoryEvents),
                @"before" : before,
                @"absent" : absent,
                @"restored" : restored,
                @"privateDeviceRemoved" : @YES,
                @"physicalOutputsOpened" : @(monitors),
                @"monitorOutputSilenceGuard" : @(monitors),
                @"outputCallbacks" : @(counters->callbacks.load()),
                @"outputFrames" : @(counters->frames.load()),
                @"nonzeroOutputSamplesBeforeGuard" : @(counters->nonzeroSamples.load()),
                @"monitorOpens" : @(counters->opens.load()),
                @"publicDevicesPublished" : @NO,
                @"bufferChanged" : @NO,
                @"permissionRequested" : @NO,
                @"audioSamplesSaved" : @NO,
                @"scope" : @"Real private aggregate disappearance/recreation with physical input "
                           @"and independent peer; not a USB or sleep/wake test"
            });
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
