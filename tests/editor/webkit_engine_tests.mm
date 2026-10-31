#include "../../native/platform/macos/desktop/desktop.hpp"
#include "../../native/platform/macos/desktop/editing_actions.hpp"
#include "../platform/macos/test_code_identity.hpp"
#include "webkit_page_visibility.hpp"
#include <loopz/macos/capture.hpp>
#include <loopz/macos/engine_runtime.hpp>

#include <cassert>
#include <cstdio>
#include <functional>
#include <thread>

struct TestMicrophoneState {
    std::atomic<unsigned> requests{0}, canceled{0};
    std::atomic<bool> pending{false}, release{false};
    std::atomic<bool> installationPending{false}, installationRelease{false};
    std::atomic<loopz::macos::MicrophonePermission> permission{
        loopz::macos::MicrophonePermission::NotDetermined};
};

@interface LZMicrophoneFixture
    : NSObject <WKScriptMessageHandlerWithReply, LZEditingActionTarget, NSMenuItemValidation>
@property(weak) WKWebView* webView;
@property(weak) LZBridge* bridge;
@property(copy) NSString* documentPath;
- (instancetype)initWithState:(std::shared_ptr<TestMicrophoneState>)state;
@end

@implementation LZMicrophoneFixture {
    std::shared_ptr<TestMicrophoneState> _state;
}
- (void)undoRoutingOrText:(id)sender {
    (void)sender;
    loopzPerformEditingAction(self.webView, false);
}
- (void)redoRoutingOrText:(id)sender {
    (void)sender;
    loopzPerformEditingAction(self.webView, true);
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(undoRoutingOrText:))
        return loopzCanPerformEditingAction(self.webView, self.bridge.editingContext, false);
    if (item.action == @selector(redoRoutingOrText:))
        return loopzCanPerformEditingAction(self.webView, self.bridge.editingContext, true);
    return YES;
}
- (NSDictionary*)sendNativeKey:(NSDictionary*)body {
    if (body.count != 4 || ![body[@"key"] isKindOfClass:NSString.class])
        return nil;
    for (NSString* flag in @[ @"command", @"shift" ])
        if (![body[flag] isKindOfClass:NSNumber.class] ||
            CFGetTypeID((__bridge CFTypeRef)body[flag]) != CFBooleanGetTypeID())
            return nil;
    const bool command = [body[@"command"] boolValue];
    const bool shift = [body[@"shift"] boolValue];
    const bool history = [body[@"key"] isEqual:@"z"] && command;
    const bool text = [body[@"key"] isEqual:@"x"] && !command && !shift;
    const bool left = [body[@"key"] isEqual:@"ArrowLeft"];
    const bool right = [body[@"key"] isEqual:@"ArrowRight"];
    const bool range = (left || right) && !command && !shift;
    if ((!history && !text && !range) || !self.webView.window)
        return nil;
    NSString* characters = history ? (shift ? @"Z" : @"z")
                           : text  ? @"x"
                           : left  ? @"\uF702"
                                   : @"\uF703";
    const unsigned short keyCode = history ? 6 : text ? 7 : left ? 123 : 124;
    bool menuHandled = false;
    const auto flags =
        (command ? NSEventModifierFlagCommand : 0) | (shift ? NSEventModifierFlagShift : 0);
    for (const auto type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        auto* event = [NSEvent keyEventWithType:type
                                       location:NSZeroPoint
                                  modifierFlags:flags
                                      timestamp:NSProcessInfo.processInfo.systemUptime
                                   windowNumber:self.webView.window.windowNumber
                                        context:nil
                                     characters:characters
                    charactersIgnoringModifiers:characters
                                      isARepeat:NO
                                        keyCode:keyCode];
        if (!event)
            return nil;
        // This handler exists only in the private fixture. Key equivalents use
        // the shared product menu; other keys target this fixture's own window.
        if (type == NSEventTypeKeyDown)
            menuHandled = [NSApp.mainMenu performKeyEquivalent:event];
        if (!menuHandled)
            [self.webView.window sendEvent:event];
    }
    return @{@"ok" : @YES, @"menuHandled" : @(menuHandled)};
}
- (instancetype)initWithState:(std::shared_ptr<TestMicrophoneState>)state {
    if ((self = [super init]))
        _state = std::move(state);
    return self;
}
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message
                 replyHandler:(void (^)(id, NSString*))reply {
    (void)controller;
    if (![message.body isKindOfClass:NSDictionary.class]) {
        reply(nil, @"Invalid microphone fixture control");
        return;
    }
    NSDictionary* body = message.body;
    if ([body[@"operation"] isEqual:@"native-key"]) {
        NSDictionary* result = [self sendNativeKey:body];
        reply(result, result ? nil : @"Invalid private native-key request");
    } else if (body.count == 1 && [body[@"operation"] isEqual:@"saved-device-gains"]) {
        // Read only the fixture's fixed private file, never a page-chosen path.
        try {
            const auto saved = loopz::macos::loadRouteDocumentFile(self.documentPath.UTF8String);
            auto* devices = [NSMutableArray array];
            for (const auto& device : saved.document.devices)
                [devices addObject:@{
                    @"id" : @(device.id.c_str()),
                    @"gain" : @(device.routing.gain),
                    @"sampleRate" : @(device.sampleRate)
                }];
            const auto encoded = loopz::macos::encodeRouteDocumentJSON(saved.document);
            NSDictionary* document = [NSJSONSerialization
                JSONObjectWithData:[NSData dataWithBytes:encoded.data() length:encoded.size()]
                           options:0
                             error:nil];
            assert(document);
            reply(@{
                @"fromPrimary" : @(!saved.recoveredBackup),
                @"devices" : devices,
                @"document" : document
            },
                  nil);
        } catch (const std::exception& error) {
            reply(nil, @(error.what()));
        }
    } else if (body.count == 1 && [body[@"operation"] isEqual:@"edit-menu-status"]) {
        reply(@{
            @"undo" :
                @(loopzCanPerformEditingAction(self.webView, self.bridge.editingContext, false)),
            @"redo" :
                @(loopzCanPerformEditingAction(self.webView, self.bridge.editingContext, true))
        },
              nil);
    } else if (body.count == 1 && ([message.body[@"operation"] isEqual:@"menu-undo"] ||
                                   [message.body[@"operation"] isEqual:@"menu-redo"])) {
        const bool redo = [message.body[@"operation"] isEqual:@"menu-redo"];
        loopzPerformEditingAction(self.webView, redo);
        reply(@{@"ok" : @YES}, nil);
    } else if ([message.body[@"operation"] isEqual:@"installation-release"]) {
        _state->installationRelease.store(true);
        reply(@{@"ok" : @YES}, nil);
    } else if ([message.body[@"operation"] isEqual:@"installation-status"]) {
        reply(@{@"pending" : @(_state->installationPending.load())}, nil);
    } else if ([message.body[@"operation"] isEqual:@"release"]) {
        _state->release.store(true);
        reply(@{@"ok" : @YES}, nil);
    } else if ([message.body[@"operation"] isEqual:@"status"]) {
        reply(@{
            @"requests" : @(_state->requests.load()),
            @"pending" : @(_state->pending.load()),
            @"canceled" : @(_state->canceled.load())
        },
              nil);
    } else {
        reply(nil, @"Unknown microphone fixture control");
    }
}
@end

namespace {
using namespace loopz;
using namespace loopz::macos;
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr NSTimeInterval kRenderProgressSeconds = 60;
#else
constexpr NSTimeInterval kRenderProgressSeconds = 20;
#endif

// The real document runtime and editor service run above this transport. No
// public HAL objects or physical audio endpoints are opened by the fake driver.
class Driver final : public DriverConfigurationLink {
  public:
    std::vector<DeviceTransport> mappings;
    DriverConfigurationResult result{0, DriverConfigurationPhase::Committed, 0};
    void poll(uint64_t) override {
        for (auto& mapping : mappings) {
            mapping.region->header().driverOnline.store(1);
            mapping.region->header().driverHeartbeatNs.store(hostNowNs());
        }
    }
    uint64_t session() const noexcept override {
        return 1;
    }
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override {
        mappings.assign(devices.begin(), devices.end());
        result = {revision, DriverConfigurationPhase::Committed, 0};
        return 0;
    }
    DriverConfigurationResult status(uint64_t) override {
        return result;
    }
    int32_t cancel(uint64_t) override {
        return 0;
    }
    void invalidate() noexcept override {}
};

class PrivateSession final : public SessionHost {
  public:
    SessionJob inspect() override {
        return SessionJob::Managed;
    }
    void validateEngine() override {
        throw std::logic_error("Unexpected session validation");
    }
    void launch() override {
        throw std::logic_error("The fixture cannot launch an Engine");
    }
};

bool until(const std::function<bool()>& done, const std::function<void()>& tick,
           NSTimeInterval seconds = 20) {
    auto* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!done() && deadline.timeIntervalSinceNow > 0) {
        tick();
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
    }
    return done();
}

id scenario(WKWebView* view, NSString* script, const std::function<void()>& tick) {
    const auto done = std::make_shared<bool>(false);
    __block id result = nil;
    __block NSError* error = nil;
    [view callAsyncJavaScript:script
        arguments:@{}
        inFrame:nil
        inContentWorld:WKContentWorld.pageWorld
        completionHandler:^(id value, NSError* failure) {
            result = value;
            error = failure;
            *done = true;
        }];
    if (!until(
            [&] {
                return *done;
            },
            tick, 40) ||
        error) {
        std::fprintf(stderr, "WebKit Engine scenario failed: %s\n", error.description.UTF8String);
        return nil;
    }
    return result;
}
} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 5) {
            std::fprintf(
                stderr,
                "usage: webkit-engine-tests interface-root private-directory before.js after.js\n");
            return 2;
        }
        const std::filesystem::path directory{argv[2]};
        // Never accept an existing graph as an integration-test fixture.
        if (!std::filesystem::is_directory(directory) || !std::filesystem::is_empty(directory)) {
            std::fprintf(stderr, "The private directory must exist and be empty\n");
            return 2;
        }
        const auto documentPath = directory / "routing.json";
        [NSApplication.sharedApplication
            setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        const auto requirement = selfCodeRequirement();
        Driver driver;
        EngineRuntimeOptions options;
        options.documentPath = documentPath;
        options.automaticPassThroughTiming = false;
        auto runtime = std::make_unique<EngineRuntime>(driver, options);
        runtime->start();
        auto microphone = std::make_shared<TestMicrophoneState>();
        const auto makeService = [&] {
            auto operations = runtimeEditorOperations(*runtime);
            const auto snapshot = operations.snapshot;
            // Only this fixture's permission metadata is synthetic. Routing,
            // edits, authenticated IPC and document storage use the real runtime.
            operations.snapshot = [snapshot, microphone] {
                const auto encoded = snapshot();
                auto* bytes = [NSData dataWithBytes:encoded.data() length:encoded.size()];
                NSMutableDictionary* value =
                    [NSJSONSerialization JSONObjectWithData:bytes
                                                    options:NSJSONReadingMutableContainers
                                                      error:nil];
                value[@"runtime"][@"permissions"] =
                    @{@"microphone" : @(permissionName(microphone->permission.load()))};
                auto* result = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
                assert(result);
                return std::string(static_cast<const char*>(result.bytes), result.length);
            };
            return EditorService::anonymous({requirement, static_cast<uint32_t>(geteuid())},
                                            std::move(operations));
        };
        auto service = makeService();
        const auto tick = [&] {
            runtime->poll();
            service->poll();
        };
        NSMutableArray* results = [NSMutableArray array];
        std::string canonical;
        std::string firstEpoch;
        for (int phase = 0; phase < 2; ++phase) {
            microphone->installationRelease.store(false);
            auto* bridge = [[LZBridge alloc]
                initWithEndpoint:{kEditorServiceName, requirement, static_cast<uint32_t>(geteuid()),
                                  service->endpoint()}
                sessionHost:std::make_unique<PrivateSession>()
                microphoneSetup:[microphone](const std::atomic<bool>& running) {
                    microphone->release.store(false);
                    microphone->requests.fetch_add(1);
                    microphone->pending.store(true);
                    while (running.load() && !microphone->release.load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    microphone->pending.store(false);
                    if (!running.load()) {
                        microphone->canceled.fetch_add(1);
                        throw std::runtime_error("The fixture request was canceled.");
                    }
                    microphone->permission.store(MicrophonePermission::Authorized);
                    return PermissionRequest{MicrophonePermission::Authorized, true, true};
                }
                installationProbe:[microphone] {
                    microphone->installationPending.store(true);
                    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    while (!microphone->installationRelease.load() &&
                           std::chrono::steady_clock::now() < end)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    microphone->installationPending.store(false);
                    InstallationSnapshot result;
                    for (auto& component : result.components)
                        component.state = InstallationState::Installed;
                    return result;
                }];
            auto* configuration = [WKWebViewConfiguration new];
            loopzConfigureOffscreenFixtureVisibility(configuration);
            loopzConfigureWebView(configuration);
            configuration.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
            [configuration setURLSchemeHandler:[[LZResources alloc]
                                                   initWithRoot:[NSURL fileURLWithPath:@(argv[1])
                                                                           isDirectory:YES]]
                                  forURLScheme:@"loopz"];
            [configuration.userContentController
                addScriptMessageHandlerWithReply:bridge
                                    contentWorld:WKContentWorld.pageWorld
                                            name:@"loopz"];
            auto* fixture = [[LZMicrophoneFixture alloc] initWithState:microphone];
            fixture.bridge = bridge;
            fixture.documentPath = @(documentPath.c_str());
            [configuration.userContentController
                addScriptMessageHandlerWithReply:fixture
                                    contentWorld:WKContentWorld.pageWorld
                                            name:@"microphoneFixture"];
            auto* view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1360, 860)
                                            configuration:configuration];
            fixture.webView = view;
            auto* menu = [NSMenu new];
            auto* edit = [NSMenuItem new];
            edit.submenu = loopzCreateEditingMenu(fixture);
            [menu addItem:edit];
            NSApp.mainMenu = menu;
            auto* window = [[NSWindow alloc] initWithContentRect:view.frame
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
            window.releasedWhenClosed = NO;
            window.contentView = view;
            [view loadRequest:[NSURLRequest
                                  requestWithURL:[NSURL URLWithString:@"loopz://app/index.html"]]];
            bool loaded = false;
            if (!until(
                    [&] {
                        return loaded;
                    },
                    [&] {
                        tick();
                        if (!view.loading &&
                            [view.URL.absoluteString isEqual:@"loopz://app/index.html"])
                            loaded = true;
                    }))
                return 1;
            NSError* error = nil;
            auto* script = [NSString stringWithContentsOfFile:@(argv[phase ? 4 : 3])
                                                     encoding:NSUTF8StringEncoding
                                                        error:&error];
            if (!script)
                return 2;
            [window makeFirstResponder:view];
            id result = scenario(view, script, tick);
            if (!result)
                return 1;
            [results addObject:result];
            const auto closed = std::make_shared<bool>(false);
            [bridge shutdown:^{
                *closed = true;
            }];
            if (!until(
                    [&] {
                        return *closed;
                    },
                    tick))
                return 1;
            [configuration.userContentController
                removeScriptMessageHandlerForName:@"loopz"
                                     contentWorld:WKContentWorld.pageWorld];
            [configuration.userContentController
                removeScriptMessageHandlerForName:@"microphoneFixture"
                                     contentWorld:WKContentWorld.pageWorld];
            [view stopLoading];
            window.contentView = nil;
            NSApp.mainMenu = nil;
            [window close];
            assert(microphone->requests.load() == 2 && microphone->canceled.load() == 1 &&
                   !microphone->pending.load());

            const auto saved = loadRouteDocumentFile(documentPath).document;
            assert(saved.devices.size() == 1);
            const auto& device = saved.devices[0];
            assert(device.name == "Renamed Acceptance Mix" && device.routing.outputChannels == 8);
            assert(device.sampleRate == 192000);
            assert(device.routing.muted && device.routing.enabled);
            assert(runtime->status().restored);
            const auto current = encodeRouteDocumentJSON(saved);
            if (phase == 0) {
                canonical = current;
                firstEpoch = runtime->editorSnapshot().revision.epoch;
                const auto blocks = runtime->status().render.blocks;
                const bool progressed = until(
                    [&] {
                        return runtime->status().render.blocks >= blocks + 100;
                    },
                    tick, kRenderProgressSeconds);
                if (!progressed) {
                    const auto render = runtime->status().render;
                    std::fprintf(stderr,
                                 "Fixture render progress: before=%llu after=%llu running=%d "
                                 "error=%d maximumExecutionNs=%llu\n",
                                 static_cast<unsigned long long>(blocks),
                                 static_cast<unsigned long long>(render.blocks), render.running,
                                 render.error,
                                 static_cast<unsigned long long>(render.maximumExecutionNs));
                }
                assert(progressed);
                service.reset();
                runtime->stop();
                runtime = std::make_unique<EngineRuntime>(driver, options);
                runtime->start();
                assert(runtime->editorSnapshot().revision.epoch != firstEpoch);
                service = makeService();
            } else {
                assert(current == canonical);
            }
        }
        service.reset();
        runtime->stop();
        auto* report = @{
            @"status" : @"passed",
            @"engine" : @"WKWebView",
            @"phases" : results,
            @"runtimeReconstructed" : @YES,
            @"routingDocumentUnchangedAfterReopen" : @YES,
            @"renderContinuedAfterEditorExit" : @YES,
            @"microphoneRequests" : @(microphone->requests.load()),
            @"microphoneCanceledOnClose" : @(microphone->canceled.load()),
            @"renderProgressBlocks" : @100,
            @"renderProgressTimeoutSeconds" : @(kRenderProgressSeconds),
            @"limits" : @[
                @"Real desktop bridge and authenticated anonymous XPC; fake driver transport",
                @"No installed Engine, public virtual devices or physical audio IO",
                @"Runtime reconstruction in one process, not an Engine process crash",
                @"Native keys target only the fixture window and shared Edit menu; no system events, clipboard or physical keyboard delivery",
                @"Microphone request and permission metadata are injected; no OS consent prompt or grant is exercised"
            ]
        };
        NSData* output = [NSJSONSerialization dataWithJSONObject:report
                                                         options:NSJSONWritingPrettyPrinted
                                                           error:nil];
        assert(output);
        std::fwrite(output.bytes, 1, output.length, stdout);
        std::fputc('\n', stdout);
    }
}
