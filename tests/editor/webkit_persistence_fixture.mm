#include "../../native/platform/macos/desktop/desktop.hpp"

#include <cstdio>
#include <unistd.h>

// This executable is packaged with a unique test-only bundle identity. It uses
// the same default persistent WebKit store as the desktop, never LZBridge.
@interface LZPreferenceFixture : NSObject <WKScriptMessageHandlerWithReply, WKNavigationDelegate>
@property NSDictionary* envelope;
@property BOOL loaded;
@property NSError* navigationError;
@property NSUInteger reads;
@property NSUInteger rejectedCommands;
@end

@implementation LZPreferenceFixture
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message
                 replyHandler:(void (^)(id, NSString*))reply {
    (void)controller;
    if (![message.body isKindOfClass:NSDictionary.class]) {
        reply(nil, @"Invalid preference fixture message");
        return;
    }
    if ([message.body[@"operation"] isEqual:@"menu-context"]) {
        reply(@{@"ok" : @YES}, nil);
    } else if ([message.body[@"operation"] isEqual:@"state"]) {
        ++_reads;
        reply(_envelope, nil);
    } else {
        ++_rejectedCommands;
        reply(nil, @"Preference fixture refuses all edits and system actions");
    }
}
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    _loaded = YES;
}
- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    _navigationError = error;
}
@end

static bool pumpUntil(BOOL (^complete)(), NSTimeInterval seconds) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!complete() && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    return complete();
}

static bool printJSON(NSDictionary* value) {
    NSData* encoded = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if (!encoded)
        return false;
    std::fwrite(encoded.bytes, 1, encoded.length, stdout);
    std::putchar('\n');
    return true;
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSString* identifier = NSBundle.mainBundle.bundleIdentifier;
        NSRegularExpression* pattern = [NSRegularExpression
            regularExpressionWithPattern:
                @"^com\\.lexasoft\\.loopz\\.fixture\\.preferences\\.[a-f0-9]{32}$"
                                 options:0
                                   error:nil];
        if (!identifier || ![pattern numberOfMatchesInString:identifier
                                                     options:0
                                                       range:NSMakeRange(0, identifier.length)]) {
            std::fprintf(stderr, "A private preference-fixture bundle identity is required\n");
            return 2;
        }
        const bool cleanup = argc == 2 && std::string_view(argv[1]) == "--clear-private-store";
        if (!cleanup && (argc != 5 || ![@[ @"light", @"dark" ] containsObject:@(argv[4])])) {
            std::fprintf(stderr, "usage: preference-fixture interface-root envelope.json "
                                 "scenario.js light|dark\n");
            return 2;
        }
        [NSApplication.sharedApplication
            setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        auto* configuration = [WKWebViewConfiguration new];
        loopzConfigureWebView(configuration);
        // Deliberately leave websiteDataStore at its production default.
        auto* store = configuration.websiteDataStore;
        if (!store.persistent || store.identifier) {
            std::fprintf(stderr, "Expected WebKit's default persistent data store\n");
            return 2;
        }
        if (cleanup) {
            __block BOOL removed = NO;
            [store removeDataOfTypes:WKWebsiteDataStore.allWebsiteDataTypes
                       modifiedSince:NSDate.distantPast
                   completionHandler:^{
                       removed = YES;
                   }];
            if (!pumpUntil(
                    ^BOOL {
                        return removed;
                    },
                    15))
                return 1;
            __block NSArray* records = nil;
            [store fetchDataRecordsOfTypes:WKWebsiteDataStore.allWebsiteDataTypes
                         completionHandler:^(NSArray* value) {
                             records = value;
                         }];
            if (!pumpUntil(
                    ^BOOL {
                        return records != nil;
                    },
                    15) ||
                records.count)
                return 1;
            return printJSON(@{
                @"status" : @"cleared",
                @"bundleIdentifier" : identifier,
                @"recordsRemaining" : @0
            })
                       ? 0
                       : 1;
        }

        auto* fixture = [LZPreferenceFixture new];
        NSData* envelope = [NSData dataWithContentsOfFile:@(argv[2])];
        fixture.envelope =
            envelope ? [NSJSONSerialization JSONObjectWithData:envelope options:0 error:nil] : nil;
        NSString* script = [NSString stringWithContentsOfFile:@(argv[3])
                                                     encoding:NSUTF8StringEncoding
                                                        error:nil];
        if (![fixture.envelope isKindOfClass:NSDictionary.class] || !script)
            return 2;
        [configuration
            setURLSchemeHandler:[[LZResources alloc] initWithRoot:[NSURL fileURLWithPath:@(argv[1])
                                                                             isDirectory:YES]]
                   forURLScheme:@"loopz"];
        [configuration.userContentController
            addScriptMessageHandlerWithReply:fixture
                                contentWorld:WKContentWorld.pageWorld
                                        name:@"loopz"];
        [configuration.userContentController
            addUserScript:
                [[WKUserScript alloc]
                      initWithSource:
                          @"try { globalThis.__fixtureInitialPreference = { stored: "
                           "localStorage.getItem('loopz-appearance') }; } catch (error) { "
                           "globalThis.__fixtureInitialPreference = { error: String(error) }; }"
                       injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                    forMainFrameOnly:YES]];
        auto* webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1360, 860)
                                           configuration:configuration];
        webView.navigationDelegate = fixture;
        auto* window = [[NSWindow alloc] initWithContentRect:webView.frame
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        window.releasedWhenClosed = NO;
        window.contentView = webView;
        window.appearance =
            [NSAppearance appearanceNamed:[@(argv[4]) isEqual:@"dark"] ? NSAppearanceNameDarkAqua
                                                                       : NSAppearanceNameAqua];
        [webView loadRequest:[NSURLRequest
                                 requestWithURL:[NSURL URLWithString:@"loopz://app/index.html"]]];
        if (!pumpUntil(
                ^BOOL {
                    return fixture.loaded || fixture.navigationError;
                },
                15) ||
            fixture.navigationError)
            return 1;
        __block BOOL finished = NO;
        __block id result = nil;
        __block NSError* failure = nil;
        [webView callAsyncJavaScript:script
            arguments:@{}
            inFrame:nil
            inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id value, NSError* error) {
                result = value;
                failure = error;
                finished = YES;
            }];
        if (!pumpUntil(
                ^BOOL {
                    return finished;
                },
                20) ||
            failure || !result) {
            std::fprintf(stderr, "Preference scenario failed: %s\n",
                         failure.description.UTF8String);
            return 1;
        }
        if (fixture.rejectedCommands || !fixture.reads)
            return 1;
        const bool printed = printJSON(@{
            @"status" : @"passed",
            @"pid" : @(getpid()),
            @"bundleIdentifier" : identifier,
            @"persistent" : @(store.persistent),
            @"bundleVersion" : NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"],
            @"defaultDataStore" : @YES,
            @"initialNativeAppearance" : @(argv[4]),
            @"stateReads" : @(fixture.reads),
            @"rejectedCommands" : @(fixture.rejectedCommands),
            @"scenario" : result
        });
        [configuration.userContentController
            removeScriptMessageHandlerForName:@"loopz"
                                 contentWorld:WKContentWorld.pageWorld];
        [webView stopLoading];
        window.contentView = nil;
        [window close];
        return printed ? 0 : 1;
    }
}
