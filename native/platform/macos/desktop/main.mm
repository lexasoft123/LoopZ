#include "desktop.hpp"
#include "audio_maintenance_service.hpp"
#include "login.hpp"
#include "editing_actions.hpp"
#include "window_chrome.hpp"
#include "window_visibility.hpp"

#include <cstdio>

@interface LZApplication : NSObject <NSApplicationDelegate, WKNavigationDelegate, WKUIDelegate,
                                     LZEditingActionTarget, NSMenuItemValidation, NSWindowDelegate>
@end

@implementation LZApplication {
    NSWindow* _window;
    WKWebView* _webView;
    LZBridge* _bridge;
    LZWindowVisibility* _visibility;
    bool _terminating;
}
- (void)windowDidUpdate:(NSNotification*)notification {
    (void)notification;
    loopzAlignWindowControls(_window);
}
- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    loopzAlignWindowControls(_window);
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
    (void)notification;
    loopzAlignWindowControls(_window);
}
- (void)undoRoutingOrText:(id)sender {
    (void)sender;
    loopzPerformEditingAction(_webView, false);
}
- (void)redoRoutingOrText:(id)sender {
    (void)sender;
    loopzPerformEditingAction(_webView, true);
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(undoRoutingOrText:))
        return loopzCanPerformEditingAction(_webView, _bridge.editingContext, false);
    if (item.action == @selector(redoRoutingOrText:))
        return loopzCanPerformEditingAction(_webView, _bridge.editingContext, true);
    return YES;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    if (!loopzValidateDesktopBundle()) {
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"LoopZ could not verify its interface";
        alert.informativeText = @"Build or reinstall a signed copy before opening the editor.";
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    try {
        _bridge = [[LZBridge alloc] initWithEndpoint:loopz::macos::productionEditorEndpoint()];
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LoopZ desktop: %s\n", error.what());
        [NSApp terminate:nil];
        return;
    }
    auto* configuration = [WKWebViewConfiguration new];
    loopzConfigureWebView(configuration);
    loopzInstallVisibilityScript(configuration);
    NSURL* resources = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"interface"
                                                                        isDirectory:YES];
    [configuration setURLSchemeHandler:[[LZResources alloc] initWithRoot:resources]
                          forURLScheme:@"loopz"];
    [configuration.userContentController addScriptMessageHandlerWithReply:_bridge
                                                             contentWorld:WKContentWorld.pageWorld
                                                                     name:@"loopz"];
    _webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1360, 860)
                                  configuration:configuration];
    _webView.navigationDelegate = self;
    _webView.UIDelegate = self;
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1360, 860)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskFullSizeContentView
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"LoopZ";
    _window.titleVisibility = NSWindowTitleHidden;
    _window.titlebarAppearsTransparent = YES;
    _window.toolbarStyle = NSWindowToolbarStyleUnified;
    [configuration.userContentController
        addUserScript:[[WKUserScript alloc]
                            initWithSource:@"document.documentElement.dataset.loopzNative = 'true'"
                             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                          forMainFrameOnly:YES]];
    _window.delegate = self;
    _window.minSize = NSMakeSize(1080, 650);
    _window.contentView = _webView;
    _visibility = [[LZWindowVisibility alloc] initWithWindow:_window webView:_webView];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    loopzAlignWindowControls(_window);
    [_webView
        loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"loopz://app/index.html"]]];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)application {
    (void)application;
    return YES;
}
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    [_visibility didFinishNavigation];
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (!_bridge || _terminating)
        return NSTerminateNow;
    _terminating = true;
    [_bridge shutdown:^{
        [NSApp replyToApplicationShouldTerminate:YES];
    }];
    return NSTerminateLater;
}
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decision {
    (void)webView;
    const bool allowed = action.targetFrame.isMainFrame && loopzLocalUIURL(action.request.URL) &&
                         [action.request.URL.path isEqual:@"/index.html"];
    decision(allowed ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
    (void)webView;
    (void)configuration;
    (void)action;
    (void)features;
    return nil;
}
- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    std::fprintf(stderr, "LoopZ interface failed to load: %s\n",
                 error.localizedDescription.UTF8String);
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 3 && std::string_view(argv[1]) == "--audio-maintenance") {
            if (!loopzValidateDesktopBundle())
                return 3;
            // Match the running app's event loop while the maintenance call
            // runs off-main; service registration can deliver main-thread work.
            NSApplication* maintenanceApplication = NSApplication.sharedApplication;
            [maintenanceApplication setActivationPolicy:NSApplicationActivationPolicyAccessory];
            [maintenanceApplication finishLaunching];
            __block int code = 1;
            __block bool finished = false;
            NSString* action = @(argv[2]);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                @autoreleasepool {
                    NSDictionary* result = loopz::macos::requestAudioMaintenance(action);
                    NSData* data = [NSJSONSerialization dataWithJSONObject:result
                                                                   options:0
                                                                     error:nil];
                    const int status = data && [result[@"ok"] isEqual:@YES] ? 0 : 1;
                    if (data) {
                        std::fwrite(data.bytes, 1, data.length, stdout);
                        std::putchar('\n');
                    }
                    dispatch_async(dispatch_get_main_queue(), ^{
                        code = status;
                        finished = true;
                        CFRunLoopStop(CFRunLoopGetMain());
                    });
                }
            });
            while (!finished)
                CFRunLoopRun();
            return code;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--verify-interface")
            return loopzValidateDesktopBundle() ? 0 : 3;
        if (argc == 2 && std::string_view(argv[1]) == "--routing-status")
            return loopzPrintRoutingStatus();
        if (argc == 2 && std::string_view(argv[1]) == "--installation-status") {
            if (!loopzValidateDesktopBundle())
                return 3;
            auto* value =
                loopz::macos::installationJSON(loopz::macos::productionInstallationProbe()());
            auto* data = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
            if (!data)
                return 1;
            std::fwrite(data.bytes, 1, data.length, stdout);
            std::putchar('\n');
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--background-status") {
            if (!loopzValidateDesktopBundle())
                return 3;
            NSDictionary* status =
                @{@"version" : @1,
                  @"login" : loopz::macos::loginStartupStatus()};
            NSData* data = [NSJSONSerialization dataWithJSONObject:status options:0 error:nil];
            if (!data)
                return 1;
            std::fwrite(data.bytes, 1, data.length, stdout);
            std::putchar('\n');
            return 0;
        }
        if (argc != 1) {
            std::fprintf(
                stderr,
                "LoopZ desktop does not accept command-line routing or endpoint overrides\n");
            return 2;
        }
        (void)argv;
        NSApplication* application = NSApplication.sharedApplication;
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu* menu = [NSMenu new];
        __attribute__((objc_precise_lifetime)) LZApplication* delegate = [LZApplication new];
        NSMenuItem* applicationItem = [NSMenuItem new];
        [menu addItem:applicationItem];
        NSMenu* applicationMenu = [[NSMenu alloc] initWithTitle:@"LoopZ"];
        [applicationMenu addItemWithTitle:@"Quit LoopZ"
                                   action:@selector(terminate:)
                            keyEquivalent:@"q"];
        applicationItem.submenu = applicationMenu;
        NSMenuItem* editItem = [[NSMenuItem alloc] initWithTitle:@"Edit"
                                                          action:nil
                                                   keyEquivalent:@""];
        [menu addItem:editItem];
        editItem.submenu = loopzCreateEditingMenu(delegate);
        NSMenuItem* windowItem = [[NSMenuItem alloc] initWithTitle:@"Window"
                                                            action:nil
                                                     keyEquivalent:@""];
        [menu addItem:windowItem];
        NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
        [windowMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
        [windowMenu addItemWithTitle:@"Minimize"
                              action:@selector(performMiniaturize:)
                       keyEquivalent:@"m"];
        windowItem.submenu = windowMenu;
        application.windowsMenu = windowMenu;
        application.mainMenu = menu;
        application.delegate = delegate;
        [application run];
    }
}
