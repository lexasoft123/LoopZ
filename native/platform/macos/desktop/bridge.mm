#include "desktop.hpp"
#include "release_updates.hpp"
#include <loopz/macos/device_inventory.hpp>
#include "session.hpp"
#include "login.hpp"
#include <loopz/macos/capture.hpp>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <algorithm>
#include <set>
#include <utility>
#include <cmath>

namespace {
using namespace loopz::macos;
NSString* text(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding]
               ?: @"Unavailable";
}
bool ownApp(NSString* identifier) {
    return [identifier hasPrefix:@"com.lexasoft.loopz."];
}
id parsed(const std::string& json) {
    id value = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json.data()
                                                                      length:json.size()]
                                               options:0
                                                 error:nil];
    if (!value)
        throw std::runtime_error("The engine response could not be read");
    return value;
}
NSString* failureCode(EditorFailure code) {
    switch (code) {
    case EditorFailure::Busy:
        return @"busy";
    case EditorFailure::Expired:
        return @"expired";
    case EditorFailure::Conflict:
        return @"conflict";
    case EditorFailure::Invalid:
        return @"invalid";
    case EditorFailure::Unavailable:
        return @"unavailable";
    case EditorFailure::Failed:
        return @"failed";
    }
    return @"failed";
}
NSDictionary* failure(NSString* code, NSString* detail) {
    return @{@"ok" : @NO, @"code" : code, @"detail" : detail};
}
NSArray* applications() {
    auto* result = [NSMutableDictionary dictionary];
    NSArray* chosen = [NSUserDefaults.standardUserDefaults arrayForKey:@"ChosenApplications"];
    for (id app in chosen) {
        if (![app isKindOfClass:NSDictionary.class] ||
            ![app[@"bundle"] isKindOfClass:NSString.class] ||
            ![app[@"name"] isKindOfClass:NSString.class] || ownApp(app[@"bundle"]))
            continue;
        result[app[@"bundle"]] = app;
    }
    for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
        if (!app.bundleIdentifier || ownApp(app.bundleIdentifier) ||
            app.activationPolicy != NSApplicationActivationPolicyRegular)
            continue;
        result[app.bundleIdentifier] = @{
            @"bundle" : app.bundleIdentifier,
            @"name" : app.localizedName ?: app.bundleIdentifier,
            @"pid" : @(app.processIdentifier)
        };
    }
    return [result.allValues sortedArrayUsingComparator:^NSComparisonResult(id a, id b) {
        return [a[@"name"] localizedCaseInsensitiveCompare:b[@"name"]];
    }];
}
struct Worker {
    const dispatch_queue_t queue{
        dispatch_queue_create("com.lexasoft.loopz.desktop.transport", DISPATCH_QUEUE_SERIAL)};
    const dispatch_queue_t permissionQueue{
        dispatch_queue_create("com.lexasoft.loopz.desktop.permission", DISPATCH_QUEUE_SERIAL)};
    const dispatch_queue_t setupQueue{
        dispatch_queue_create("com.lexasoft.loopz.desktop.setup", DISPATCH_QUEUE_SERIAL)};
    std::atomic<bool> running{true};
    std::atomic<unsigned> pending{0};
    std::atomic<bool> requestingMicrophone{false};
    std::atomic<bool> installingAudio{false};
    std::unique_ptr<EditorConnection> connection;
    DeviceInventory inventory;
    SessionSupervisor session;
    MicrophoneSetup microphoneSetup;
    InstallationProbe installationProbe;
    std::shared_ptr<const InstallationSnapshot> installation; // Transport queue only.
    bool checkingInstallation{false};
    uint64_t nextInstallationCheck{0}; // Transport queue only.
    Worker(EditorServiceEndpoint endpoint, std::unique_ptr<SessionHost> host, MicrophoneSetup setup,
           InstallationProbe probe)
        : connection(std::make_unique<EditorConnection>(std::move(endpoint))),
          session(std::move(host)), microphoneSetup(std::move(setup)),
          installationProbe(std::move(probe)) {
        if (!microphoneSetup || !installationProbe)
            throw std::invalid_argument("Desktop setup operations are required.");
    }
};
// Pass the owner by value: an escaping Objective-C block captures a C++
// reference as a reference, rather than retaining the referenced shared_ptr.
void refreshInstallation(std::shared_ptr<Worker> state, uint64_t now) {
    if (now < state->nextInstallationCheck || state->checkingInstallation)
        return;
    state->checkingInstallation = true;
    state->nextInstallationCheck = now + 30000;
    // Signature and filesystem inspection must never block live meter polls.
    dispatch_async(state->setupQueue, ^{
        @autoreleasepool {
            auto snapshot = std::make_shared<InstallationSnapshot>();
            try {
                if (state->running.load())
                    *snapshot = state->installationProbe();
            } catch (...) {
                for (auto& component : snapshot->components)
                    component = {
                        InstallationState::Unknown,
                        "LoopZ could not check its audio components. Retry the setup check."};
            }
            dispatch_async(state->queue, ^{
                if (state->running.load())
                    state->installation = snapshot;
                state->checkingInstallation = false;
            });
        }
    });
}
} // namespace

@implementation LZBridge {
    std::shared_ptr<Worker> _worker;
    NSArray* _applications;
    CFAbsoluteTime _nextApplications;
    bool _choosing;
    NSDictionary* _editingContext;
}
- (NSDictionary*)editingContext {
    return _editingContext ?: @{};
}
- (instancetype)initWithEndpoint:(EditorServiceEndpoint)endpoint {
    return [self initWithEndpoint:std::move(endpoint) sessionHost:productionSessionHost()];
}
- (instancetype)initWithEndpoint:(EditorServiceEndpoint)endpoint
                     sessionHost:(std::unique_ptr<SessionHost>)sessionHost {
    return [self initWithEndpoint:std::move(endpoint)
                      sessionHost:std::move(sessionHost)
                  microphoneSetup:productionMicrophoneSetup()];
}
- (instancetype)initWithEndpoint:(EditorServiceEndpoint)endpoint
                     sessionHost:(std::unique_ptr<SessionHost>)sessionHost
                 microphoneSetup:(MicrophoneSetup)microphoneSetup {
    return [self initWithEndpoint:std::move(endpoint)
                      sessionHost:std::move(sessionHost)
                  microphoneSetup:std::move(microphoneSetup)
                installationProbe:productionInstallationProbe()];
}
- (instancetype)initWithEndpoint:(EditorServiceEndpoint)endpoint
                     sessionHost:(std::unique_ptr<SessionHost>)sessionHost
                 microphoneSetup:(MicrophoneSetup)microphoneSetup
               installationProbe:(InstallationProbe)installationProbe {
    if ((self = [super init]))
        _worker =
            std::make_shared<Worker>(std::move(endpoint), std::move(sessionHost),
                                     std::move(microphoneSetup), std::move(installationProbe));
    return self;
}
- (void)shutdown:(dispatch_block_t)completion {
    const auto state = _worker;
    state->running.store(false);
    dispatch_async(state->queue, ^{
        state->connection.reset();
        dispatch_async(state->permissionQueue, ^{
            dispatch_async(state->setupQueue, ^{
                dispatch_async(dispatch_get_main_queue(), completion);
            });
        });
    });
}
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message
                 replyHandler:(void (^)(id, NSString*))reply {
    (void)controller;
    @try {
        if (!message.frameInfo.isMainFrame || !loopzLocalUIURL(message.frameInfo.request.URL) ||
            ![message.frameInfo.request.URL.path isEqual:@"/index.html"] ||
            ![message.body isKindOfClass:NSDictionary.class]) {
            reply(nil, @"This page cannot access LoopZ controls");
            return;
        }
        NSDictionary* body = message.body;
        id version = body[@"version"];
        NSString* operation = body[@"operation"];
        if (![version isKindOfClass:NSNumber.class] ||
            CFGetTypeID((__bridge CFTypeRef)version) == CFBooleanGetTypeID() ||
            [version doubleValue] != 1 || ![operation isKindOfClass:NSString.class]) {
            reply(nil, @"Unsupported desktop message");
            return;
        }
        if ([operation isEqual:@"audio-setup"]) {
            const auto state = _worker;
            if (body.count != 2 || state->installingAudio.exchange(true)) {
                reply(failure(@"busy", @"Audio setup is already running."), nil);
                return;
            }
            NSString* helper = @"/Applications/LoopZ.app/Contents/Library/Helpers/LoopZSetup";
            if (![NSFileManager.defaultManager isExecutableFileAtPath:helper]) {
                state->installingAudio.store(false);
                reply(failure(@"unavailable", @"Install the complete LoopZ download in "
                                              @"Applications before setting up audio."),
                      nil);
                return;
            }
            dispatch_async(state->setupQueue, ^{
                @autoreleasepool {
                    // Fixed executable and no caller-supplied command or arguments.
                    NSAppleScript* script = [[NSAppleScript alloc]
                        initWithSource:@"do shell script "
                                       @"\"'/Applications/LoopZ.app/Contents/Library/Helpers/"
                                       @"LoopZSetup'\" with administrator privileges"];
                    NSDictionary* error = nil;
                    const BOOL success = [script executeAndReturnError:&error] != nil;
                    NSString* detail = error[NSAppleScriptErrorMessage]
                                           ?: @"Audio setup was cancelled or could not finish.";
                    dispatch_async(dispatch_get_main_queue(), ^{
                        state->installingAudio.store(false);
                        if (success)
                            reply(@{@"ok" : @YES}, nil);
                        else
                            reply(failure(@"failed", detail), nil);
                    });
                }
            });
            return;
        }
        if ([@[ @"updates", @"check-updates", @"open-update", @"automatic-updates" ]
                containsObject:operation]) {
            const bool setting = [operation isEqual:@"automatic-updates"];
            if (body.count != (setting ? 3 : 2) ||
                (setting &&
                 (![body[@"enabled"] isKindOfClass:NSNumber.class] ||
                  CFGetTypeID((__bridge CFTypeRef)body[@"enabled"]) != CFBooleanGetTypeID()))) {
                reply(nil, @"Invalid update request");
                return;
            }
            LZReleaseUpdates* updates = LZReleaseUpdates.shared;
            if (setting)
                [updates setAutomatic:[body[@"enabled"] boolValue]];
            if ([operation isEqual:@"check-updates"])
                [updates check];
            if ([operation isEqual:@"open-update"] && ![updates openRelease]) {
                reply(failure(@"unavailable", @"The release page could not be opened."), nil);
                return;
            }
            reply([updates snapshot], nil);
            return;
        }
        const bool editing = [operation isEqual:@"edit"] || [operation isEqual:@"initialize"];
        if ([operation isEqual:@"menu-context"]) {
            if (body.count != 5) {
                reply(nil, @"Invalid editing context");
                return;
            }
            for (NSString* key in @[ @"textEditing", @"canUndo", @"canRedo" ])
                if (![body[key] isKindOfClass:NSNumber.class] ||
                    CFGetTypeID((__bridge CFTypeRef)body[key]) != CFBooleanGetTypeID()) {
                    reply(nil, @"Invalid editing context flag");
                    return;
                }
            _editingContext = @{
                @"textEditing" : body[@"textEditing"],
                @"canUndo" : body[@"canUndo"],
                @"canRedo" : body[@"canRedo"]
            };
            reply(@{@"ok" : @YES}, nil);
            return;
        }
        const bool privacy = [operation isEqual:@"privacy"];
        const bool login = [operation isEqual:@"login"];
        const bool hardwareBuffer = [operation isEqual:@"device-buffer"];
        if (body.count != (editing || privacy || login || hardwareBuffer ? 3 : 2)) {
            reply(nil, @"Unexpected desktop message fields");
            return;
        }
        if ([operation isEqual:@"request-microphone"]) {
            const auto state = _worker;
            if (!state->running.load() || state->requestingMicrophone.exchange(true)) {
                reply(failure(@"busy",
                              @"A microphone request is already pending or the editor is closing."),
                      nil);
                return;
            }
            dispatch_async(state->permissionQueue, ^{
                @autoreleasepool {
                    id value;
                    try {
                        const auto result = state->microphoneSetup(state->running);
                        value = @{
                            @"ok" : @YES,
                            @"microphone" : @(permissionName(result.status)),
                            @"requested" : @(result.requested),
                            @"completed" : @(result.completed)
                        };
                    } catch (const std::exception& error) {
                        value = failure(@"failed", text(error.what()));
                    }
                    dispatch_async(dispatch_get_main_queue(), ^{
                        state->requestingMicrophone.store(false);
                        reply(value, nil);
                    });
                }
            });
            return;
        }
        if ([operation isEqual:@"login-settings"]) {
            openLoginStartupSettings();
            reply(@{@"ok" : @YES}, nil);
            return;
        }
        if (privacy) {
            NSString* pane = body[@"pane"];
            if (![pane isEqual:@"microphone"] && ![pane isEqual:@"audio"]) {
                reply(nil, @"Unknown privacy setting");
                return;
            }
            NSURL* url = [NSURL URLWithString:[pane isEqual:@"microphone"]
                                                  ? @"x-apple.systempreferences:com.apple."
                                                    @"preference.security?Privacy_Microphone"
                                                  : @"x-apple.systempreferences:com.apple."
                                                    @"preference.security?Privacy_ScreenCapture"];
            const bool opened = [NSWorkspace.sharedWorkspace openURL:url];
            reply(@{@"ok" : @(opened)},
                  opened ? nil : @"Open Privacy & Security in System Settings");
            return;
        }
        if ([operation isEqual:@"choose-application"]) {
            if (_choosing) {
                reply(nil, @"An application chooser is already open");
                return;
            }
            _choosing = true;
            NSOpenPanel* panel = NSOpenPanel.openPanel;
            panel.allowedContentTypes = @[ UTTypeApplicationBundle ];
            panel.allowsMultipleSelection = NO;
            panel.canChooseDirectories = NO;
            panel.canChooseFiles = YES;
            panel.prompt = @"Add application";
            [panel beginSheetModalForWindow:message.webView.window
                          completionHandler:^(NSModalResponse result) {
                              self->_choosing = false;
                              if (result != NSModalResponseOK) {
                                  reply(nil, nil);
                                  return;
                              }
                              NSBundle* bundle = [NSBundle bundleWithURL:panel.URL];
                              NSString* identifier = bundle.bundleIdentifier;
                              NSString* name = [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: [bundle objectForInfoDictionaryKey:@"CFBundleName"] ?: panel.URL.lastPathComponent;
                              if (!identifier.length || identifier.length > 4096 ||
                                  ![name isKindOfClass:NSString.class] || name.length > 4096 ||
                                  ownApp(identifier)) {
                                  reply(nil, @"Choose another application");
                                  return;
                              }
                              NSDictionary* app =
                                  @{@"bundle" : identifier,
                                    @"name" : name,
                                    @"pid" : @0};
                              NSMutableArray* chosen =
                                  [[NSUserDefaults.standardUserDefaults
                                      arrayForKey:@"ChosenApplications"] mutableCopy]
                                      ?: [NSMutableArray array];
                              NSIndexSet* same = [chosen
                                  indexesOfObjectsPassingTest:^BOOL(id item, NSUInteger index,
                                                                    BOOL* stop) {
                                      (void)index;
                                      (void)stop;
                                      return ![item isKindOfClass:NSDictionary.class] ||
                                             ![item[@"bundle"] isKindOfClass:NSString.class] ||
                                             ![item[@"name"] isKindOfClass:NSString.class] ||
                                             [item[@"bundle"] isEqual:identifier];
                                  }];
                              [chosen removeObjectsAtIndexes:same];
                              [chosen addObject:app];
                              if (chosen.count > 256)
                                  [chosen removeObjectAtIndex:0];
                              [NSUserDefaults.standardUserDefaults setObject:chosen
                                                                      forKey:@"ChosenApplications"];
                              self->_nextApplications = 0;
                              reply(app, nil);
                          }];
            return;
        }
        if (login && (![body[@"enabled"] isKindOfClass:NSNumber.class] ||
                      CFGetTypeID((__bridge CFTypeRef)body[@"enabled"]) != CFBooleanGetTypeID())) {
            reply(nil, @"Login startup requires an explicit choice");
            return;
        }
        NSDictionary* buffer = hardwareBuffer ? body[@"buffer"] : nil;
        if (hardwareBuffer) {
            if (![buffer isKindOfClass:NSDictionary.class] ||
                (buffer.count != 1 && buffer.count != 3) ||
                (buffer.count == 3 && ![buffer[@"uid"] isKindOfClass:NSString.class]) ||
                (buffer.count == 1 && !buffer[@"frames"])) {
                reply(nil, @"Invalid hardware buffer request");
                return;
            }
            for (NSString* key in (buffer.count == 1 ? @[ @"frames" ]
                                                     : @[ @"frames", @"expectedFrames" ])) {
                id value = buffer[key];
                if (![value isKindOfClass:NSNumber.class] ||
                    CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ||
                    !std::isfinite([value doubleValue]) || [value doubleValue] < 1 ||
                    [value doubleValue] > ([key isEqual:@"frames"] ? 4096 : UINT32_MAX) ||
                    std::floor([value doubleValue]) != [value doubleValue]) {
                    reply(nil, @"Invalid hardware buffer size");
                    return;
                }
            }
        }
        if (![operation isEqual:@"state"] && !editing && !login && !hardwareBuffer &&
            ![operation isEqual:@"retry"] && ![operation isEqual:@"retry-session"]) {
            reply(nil, @"Unknown desktop operation");
            return;
        }
        NSData* request = nil;
        if (editing) {
            if (![body[@"request"] isKindOfClass:NSDictionary.class]) {
                reply(nil, @"Invalid routing request");
                return;
            }
            request = [NSJSONSerialization dataWithJSONObject:body[@"request"] options:0 error:nil];
            if (!request || request.length > kMaximumEditorRequestBytes) {
                reply(nil, @"Routing request exceeds its limit");
                return;
            }
        }
        if (CFAbsoluteTimeGetCurrent() >= _nextApplications) {
            _applications = applications();
            _nextApplications = CFAbsoluteTimeGetCurrent() + 2;
        }
        const auto state = _worker;
        if (!state->running.load()) {
            reply(nil, @"The editor is closing");
            return;
        }
        if (state->pending.fetch_add(1) >= 8) {
            state->pending.fetch_sub(1);
            reply(nil, @"LoopZ is busy; wait for the current operation");
            return;
        }
        NSArray* apps = _applications;
        const bool loginEnabled = [body[@"enabled"] boolValue];
        dispatch_async(state->queue, ^{
            @autoreleasepool {
                id value = nil;
                try {
                    if (!state->running.load() || !state->connection)
                        throw std::runtime_error("The editor is closing");
                    if ([operation isEqual:@"state"]) {
                        state->connection->poll();
                        const auto now = loopz::hostNowNs() / 1000000;
                        refreshInstallation(state, now);
                        state->session.poll(now, !state->connection->connected());
                        NSMutableDictionary* inventory =
                            [state->inventory.snapshot(now) mutableCopy];
                        id snapshot = state->connection->snapshot().empty()
                                          ? nil
                                          : parsed(state->connection->snapshot());
                        id hardware = [snapshot isKindOfClass:NSDictionary.class]
                                          ? snapshot[@"hardware"]
                                          : nil;
                        const bool engineBuffers = [hardware isKindOfClass:NSDictionary.class];
                        if (engineBuffers)
                            inventory = [hardware mutableCopy];
                        inventory[@"bufferSettingsAvailable"] =
                            @(engineBuffers && state->connection->ready());
                        inventory[@"applications"] = apps ?: @[];
                        const auto installation = state->installation;
                        value = @{
                            @"version" : @1,
                            @"connection" : @{
                                @"connected" : @(state->connection->connected()),
                                @"ready" : @(state->connection->ready())
                            },
                            @"snapshot" : snapshot ?: [NSNull null],
                            @"inventory" : inventory,
                            @"permissionSetup" :
                                @{@"microphonePending" : @(state->requestingMicrophone.load())},
                            @"installation" : installationJSON(
                                installation ? *installation : InstallationSnapshot{}),
                            @"background" : @{
                                @"managed" : @(state->session.status().managed),
                                @"detail" : text(state->session.status().detail),
                                @"login" : loginStartupStatus()
                            }
                        };
                    } else if (hardwareBuffer) {
                        NSMutableDictionary* forwarded = [buffer mutableCopy];
                        forwarded[@"version"] = buffer.count == 1 ? @2 : @1;
                        auto* data = [NSJSONSerialization dataWithJSONObject:forwarded
                                                                     options:0
                                                                       error:nil];
                        if (!data || data.length > 8192)
                            throw std::invalid_argument("Invalid hardware buffer request");
                        const std::string json(static_cast<const char*>(data.bytes), data.length);
                        value = parsed(state->connection->hardwareBuffer(json));
                    } else if (login) {
                        setLoginStartup(loginEnabled);
                        value = @{@"ok" : @YES, @"login" : loginStartupStatus()};
                    } else if ([operation isEqual:@"retry-session"]) {
                        state->nextInstallationCheck = 0;
                        refreshInstallation(state, loopz::hostNowNs() / 1000000);
                        state->session.retry();
                        state->session.poll(loopz::hostNowNs() / 1000000,
                                            !state->connection->connected());
                        if (state->connection->ready())
                            state->connection->retry();
                        value = @{@"ok" : @YES};
                    } else if ([operation isEqual:@"retry"]) {
                        state->connection->retry();
                        value = @{@"ok" : @YES};
                    } else {
                        const std::string json(static_cast<const char*>(request.bytes),
                                               request.length);
                        const auto response = [operation isEqual:@"initialize"]
                                                  ? state->connection->initialize(json)
                                                  : state->connection->edit(json);
                        value = @{@"ok" : @YES, @"commit" : parsed(response)};
                    }
                } catch (const UncertainEditorOutcome& error) {
                    value = failure(@"uncertain", text(error.what()));
                } catch (const EditorServiceError& error) {
                    value = failure(failureCode(error.code), text(error.what()));
                } catch (const std::exception& error) {
                    value = failure(@"failed", text(error.what()));
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    state->pending.fetch_sub(1);
                    reply(value, nil);
                });
            }
        });
    } @catch (NSException*) {
        reply(nil, @"The desktop request could not be handled");
    }
}
@end
