#include "../../native/platform/macos/desktop/desktop.hpp"
#include "../../native/platform/macos/desktop/window_visibility.hpp"
#include "webkit_page_visibility.hpp"

#include <cstdio>

// This fixture deliberately does not construct LZBridge: its session supervisor
// owns the production Engine. Only the real resource handler is shared here.
@interface LZWebKitFixture : NSObject <WKScriptMessageHandlerWithReply, WKNavigationDelegate>
@property NSDictionary* envelope;
@property BOOL loaded;
@property NSError* navigationError;
@property NSUInteger stateReads;
@property NSMutableArray* commands;
@property WKWebView* webView;
@property NSUInteger nativeKeys;
@property NSUInteger appearanceChanges;
@property LZWindowVisibility* visibility;
@property NSWindow* cover;
@end

@implementation LZWebKitFixture
- (BOOL)sendNativeKey:(NSDictionary*)body {
    if (body.count != 4 || ![body[@"key"] isKindOfClass:NSString.class] ||
        ![body[@"shift"] isKindOfClass:NSNumber.class] ||
        CFGetTypeID((__bridge CFTypeRef)body[@"shift"]) != CFBooleanGetTypeID() ||
        ![body[@"command"] isKindOfClass:NSNumber.class] ||
        CFGetTypeID((__bridge CFTypeRef)body[@"command"]) != CFBooleanGetTypeID())
        return NO;
    NSDictionary* codes = @{
        @"Tab" : @[ @48, @"\t" ],
        @"Escape" : @[ @53, @"\x1b" ],
        @"Enter" : @[ @36, @"\r" ],
        @"Space" : @[ @49, @" " ],
        @"n" : @[ @45, @"n" ],
        @"," : @[ @43, @"," ]
    };
    NSArray* selected = codes[body[@"key"]];
    if (!selected || !self.webView.window)
        return NO;
    const bool command = [body[@"command"] boolValue];
    const bool shift = [body[@"shift"] boolValue];
    if ((command && ![body[@"key"] isEqual:@"n"] && ![body[@"key"] isEqual:@","]) ||
        (shift && ![body[@"key"] isEqual:@"Tab"]))
        return NO;
    const auto flags =
        (command ? NSEventModifierFlagCommand : 0) | (shift ? NSEventModifierFlagShift : 0);
    NSString* characters = shift ? @"\x19" : selected[1];
    for (const auto type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        auto* event = [NSEvent keyEventWithType:type
                                       location:NSZeroPoint
                                  modifierFlags:flags
                                      timestamp:NSProcessInfo.processInfo.systemUptime
                                   windowNumber:self.webView.window.windowNumber
                                        context:nil
                                     characters:characters
                    charactersIgnoringModifiers:selected[1]
                                      isARepeat:NO
                                        keyCode:[selected[0] unsignedShortValue]];
        if (!event)
            return NO;
        // Dispatch only through this fixture's own AppKit window. No event
        // tap, system injection, global shortcut or production bridge exists.
        [self.webView.window sendEvent:event];
    }
    ++_nativeKeys;
    return YES;
}
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message
                 replyHandler:(void (^)(id, NSString*))reply {
    (void)controller;
    if (![message.body isKindOfClass:NSDictionary.class]) {
        reply(nil, @"Invalid fixture message");
        return;
    }
    // A separate handler is available only in this test executable. It changes
    // synthetic snapshots; no such operation exists in the product bridge.
    if ([message.name isEqual:@"fixture"]) {
        NSDictionary* body = message.body;
        if ([body[@"operation"] isEqual:@"visibility-state"] && self.visibility) {
            reply(@{
                @"reads" : @(self.stateReads),
                @"hidden" : @(NSApp.hidden),
                @"miniaturized" : @(self.webView.window.miniaturized),
                @"visible" :
                    @((self.webView.window.occlusionState & NSWindowOcclusionStateVisible) != 0)
            },
                  nil);
        } else if ([body[@"operation"] isEqual:@"visibility"] && self.visibility &&
                   body.count == 2) {
            NSWindow* window = self.webView.window;
            NSString* action = body[@"value"];
            if ([action isEqual:@"hide"]) {
                [NSApp hide:nil];
            } else if ([action isEqual:@"show"]) {
                [NSApp unhideWithoutActivation];
                [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
            } else if ([action isEqual:@"minimize"]) {
                [window miniaturize:nil];
            } else if ([action isEqual:@"restore"]) {
                [window deminiaturize:nil];
                [window makeKeyAndOrderFront:nil];
            } else if ([action isEqual:@"cover"] || [action isEqual:@"partial-cover"]) {
                if (!self.cover) {
                    self.cover = [[NSWindow alloc] initWithContentRect:window.frame
                                                             styleMask:NSWindowStyleMaskBorderless
                                                               backing:NSBackingStoreBuffered
                                                                 defer:NO];
                    self.cover.backgroundColor = NSColor.windowBackgroundColor;
                    self.cover.opaque = YES;
                    self.cover.level = window.level + 1;
                }
                NSRect frame = NSInsetRect(window.frame, -12, -12);
                if ([action isEqual:@"partial-cover"])
                    frame.size.width /= 2;
                [self.cover setFrame:frame display:YES];
                [self.cover orderFront:nil];
            } else if ([action isEqual:@"uncover"]) {
                [self.cover orderOut:nil];
            } else {
                reply(nil, @"Unknown visibility fixture action");
                return;
            }
            reply(@{@"ok" : @YES}, nil);
        } else if ([message.body[@"operation"] isEqual:@"window-appearance"] && body.count == 2 &&
                   self.webView.window &&
                   [@[ @"light", @"dark" ] containsObject:message.body[@"value"]]) {
            // Change only this private fixture window, never macOS preferences.
            self.webView.window.appearance = [NSAppearance
                appearanceNamed:[message.body[@"value"] isEqual:@"dark"] ? NSAppearanceNameDarkAqua
                                                                         : NSAppearanceNameAqua];
            ++_appearanceChanges;
            reply(@{@"ok" : @YES}, nil);
        } else if ([message.body[@"operation"] isEqual:@"native-key"]) {
            const BOOL sent = [self sendNativeKey:message.body];
            reply(sent ? @{@"ok" : @YES} : nil, sent ? nil : @"Invalid fixture-only native key");
        } else if ([message.body[@"operation"] isEqual:@"replace-envelope"] &&
                   [message.body[@"envelope"] isKindOfClass:NSDictionary.class]) {
            self.envelope = message.body[@"envelope"];
            reply(@{@"ok" : @YES}, nil);
        } else if ([message.body[@"operation"] isEqual:@"commands"]) {
            reply(self.commands ?: @[], nil);
        } else {
            reply(nil, @"Unknown fixture operation");
        }
        return;
    }
    if ([message.body[@"operation"] isEqual:@"menu-context"]) {
        reply(@{@"ok" : @YES}, nil);
        return;
    }
    if (![message.body[@"operation"] isEqual:@"state"]) {
        if (!self.commands)
            self.commands = [NSMutableArray array];
        [self.commands addObject:message.body];
    }
    if ([message.body[@"operation"] isEqual:@"retry"]) {
        reply(@{@"ok" : @YES}, nil);
        return;
    }
    if ([message.body[@"operation"] isEqual:@"device-buffer"] &&
        [self.envelope[@"inventory"][@"simulateGlobalBuffers"] boolValue]) {
        // Fixture-only state mutation: no Core Audio or production Engine is contacted.
        NSData* bytes = [NSJSONSerialization dataWithJSONObject:self.envelope options:0 error:nil];
        NSMutableDictionary* envelope =
            [NSJSONSerialization JSONObjectWithData:bytes
                                            options:NSJSONReadingMutableContainers
                                              error:nil];
        NSMutableDictionary* inventory = envelope[@"inventory"];
        NSNumber* frames = message.body[@"buffer"][@"frames"];
        inventory[@"routingBufferFrames"] = frames;
        NSMutableArray* preferences = [NSMutableArray array];
        for (NSMutableDictionary* device in inventory[@"devices"])
            if ([inventory[@"routingBufferDevices"] containsObject:device[@"uid"]]) {
                device[@"bufferFrames"] = frames;
                [preferences addObject:@{
                    @"uid" : device[@"uid"],
                    @"frames" : frames,
                    @"applied" : @YES,
                    @"error" : @""
                }];
            }
        inventory[@"bufferPreferences"] = preferences;
        self.envelope = envelope;
        reply(@{@"ok" : @YES, @"saved" : @YES, @"applied" : @YES}, nil);
        return;
    }
    if (![message.body[@"operation"] isEqual:@"state"]) {
        reply(nil, @"The synthetic bridge refuses edits and system actions");
        return;
    }
    ++_stateReads;
    reply(_envelope, nil);
}
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    _loaded = YES;
    [_visibility didFinishNavigation];
}
- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    _navigationError = error;
}
- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error {
    [self webView:webView didFailProvisionalNavigation:navigation withError:error];
}
@end

static bool pumpUntil(BOOL (^complete)(), NSTimeInterval seconds) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!complete() && deadline.timeIntervalSinceNow > 0) {
        if (NSApp.activationPolicy != NSApplicationActivationPolicyProhibited) {
            // Occlusion/minimize notifications arrive through AppKit events,
            // not just CFRunLoop timers. Offscreen fixtures need no event drain.
            NSEvent* event = nil;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:NSDate.distantPast
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES]))
                [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return complete();
}

static id runScenario(WKWebView* webView, NSString* script) {
    __block BOOL finished = NO;
    __block id result = nil;
    __block NSError* error = nil;
    [webView callAsyncJavaScript:script
        arguments:@{}
        inFrame:nil
        inContentWorld:WKContentWorld.pageWorld
        completionHandler:^(id value, NSError* failure) {
            result = value;
            error = failure;
            finished = YES;
        }];
    if (!pumpUntil(
            ^BOOL {
                return finished;
            },
            30) ||
        error) {
        std::fprintf(stderr, "WebKit scenario failed: %s\n", error.description.UTF8String);
        return nil;
    }
    return result;
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 6) {
            std::fprintf(stderr, "usage: webkit-fixture interface-root envelope.json before.js "
                                 "after.js snapshot.png\n");
            return 2;
        }
        const bool visibilityTest =
            [NSProcessInfo.processInfo.environment[@"LOOPZ_WEBKIT_VISIBILITY_TEST"] isEqual:@"1"];
        [NSApplication.sharedApplication
            setActivationPolicy:visibilityTest ? NSApplicationActivationPolicyAccessory
                                               : NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        auto* fixture = [LZWebKitFixture new];
        auto* data = [NSData dataWithContentsOfFile:@(argv[2])];
        NSError* error = nil;
        fixture.envelope =
            data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        auto* script = [NSString stringWithContentsOfFile:@(argv[3])
                                                 encoding:NSUTF8StringEncoding
                                                    error:&error];
        auto* afterReload = [NSString stringWithContentsOfFile:@(argv[4])
                                                      encoding:NSUTF8StringEncoding
                                                         error:&error];
        if (!fixture.envelope || !script || !afterReload) {
            std::fprintf(stderr, "Invalid fixture inputs: %s\n", error.description.UTF8String);
            return 2;
        }
        auto* configuration = [WKWebViewConfiguration new];
        loopzConfigureWebView(configuration);
        if (visibilityTest)
            loopzInstallVisibilityScript(configuration);
        else
            loopzConfigureOffscreenFixtureVisibility(configuration);
        // Never read or overwrite the user's appearance preferences.
        configuration.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
        [configuration
            setURLSchemeHandler:[[LZResources alloc] initWithRoot:[NSURL fileURLWithPath:@(argv[1])
                                                                             isDirectory:YES]]
                   forURLScheme:@"loopz"];
        [configuration.userContentController
            addScriptMessageHandlerWithReply:fixture
                                contentWorld:WKContentWorld.pageWorld
                                        name:@"loopz"];
        [configuration.userContentController
            addScriptMessageHandlerWithReply:fixture
                                contentWorld:WKContentWorld.pageWorld
                                        name:@"fixture"];
        auto* webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1360, 860)
                                           configuration:configuration];
        webView.navigationDelegate = fixture;
        auto* window = [[NSWindow alloc]
            initWithContentRect:webView.frame
                      styleMask:visibilityTest
                                    ? (NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable)
                                    : NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;
        window.contentView = webView;
        // Deterministic starting appearance; no global or user defaults change.
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        fixture.webView = webView;
        if (visibilityTest) {
            fixture.visibility = [[LZWindowVisibility alloc] initWithWindow:window webView:webView];
            [window center];
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
        [webView loadRequest:[NSURLRequest
                                 requestWithURL:[NSURL URLWithString:@"loopz://app/index.html"]]];
        if (!pumpUntil(
                ^BOOL {
                    return fixture.loaded || fixture.navigationError;
                },
                15) ||
            fixture.navigationError) {
            std::fprintf(stderr, "WebKit navigation failed: %s\n",
                         fixture.navigationError.description.UTF8String);
            return 1;
        }
        [window makeFirstResponder:webView];
        id beforeResult = runScenario(webView, script);
        if (!beforeResult)
            return 1;
        fixture.loaded = NO;
        [webView reload];
        if (!pumpUntil(
                ^BOOL {
                    return fixture.loaded || fixture.navigationError;
                },
                15) ||
            fixture.navigationError)
            return 1;
        [window makeFirstResponder:webView];
        id afterResult = runScenario(webView, afterReload);
        if (!afterResult)
            return 1;
        __block BOOL captured = NO;
        __block BOOL saved = NO;
        [webView
            takeSnapshotWithConfiguration:nil
                        completionHandler:^(NSImage* image, NSError* failure) {
                            if (!failure && image) {
                                auto* bitmap =
                                    [NSBitmapImageRep imageRepWithData:image.TIFFRepresentation];
                                auto* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                                                 properties:@{}];
                                saved = [png writeToFile:@(argv[5]) atomically:YES];
                            }
                            captured = YES;
                        }];
        if (!pumpUntil(
                ^BOOL {
                    return captured;
                },
                10) ||
            !saved) {
            std::fprintf(stderr, "WebKit snapshot failed\n");
            return 1;
        }
        auto* report = @{
            @"engine" : @"WKWebView",
            @"url" : webView.URL.absoluteString,
            @"stateReads" : @(fixture.stateReads),
            @"nativeKeys" : @(fixture.nativeKeys),
            @"windowAppearanceChanges" : @(fixture.appearanceChanges),
            @"beforeReload" : beforeResult,
            @"afterReload" : afterResult,
            @"snapshot" : @(argv[5]),
            @"limits" : @[
                @"Synthetic bridge and permission metadata; no production Engine, permission request or audio IO",
                @"Nonpersistent WebKit store; no claim about persistence across application restarts",
                @"Native key checks use the fixture's own NSWindow; system event delivery, production menus and VoiceOver remain untested"
            ]
        };
        auto* output = [NSJSONSerialization dataWithJSONObject:report
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:&error];
        if (!output)
            return 1;
        std::fwrite(output.bytes, 1, output.length, stdout);
        std::fputc('\n', stdout);
        [configuration.userContentController
            removeScriptMessageHandlerForName:@"loopz"
                                 contentWorld:WKContentWorld.pageWorld];
        [configuration.userContentController
            removeScriptMessageHandlerForName:@"fixture"
                                 contentWorld:WKContentWorld.pageWorld];
        [webView stopLoading];
        window.contentView = nil;
        [window close];
        return 0;
    }
}
