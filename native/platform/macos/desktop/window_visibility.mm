#include "window_visibility.hpp"

void loopzInstallVisibilityScript(WKWebViewConfiguration* configuration) {
    [configuration.userContentController
        addUserScript:[[WKUserScript alloc] initWithSource:@"window.__loopzNativeVisible = false;"
                                             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                          forMainFrameOnly:YES]];
}

@implementation LZWindowVisibility {
    __weak NSWindow* _window;
    __weak WKWebView* _webView;
    bool _published;
    bool _visible;
}
- (instancetype)initWithWindow:(NSWindow*)window webView:(WKWebView*)webView {
    self = [super init];
    if (self) {
        _window = window;
        _webView = webView;
        for (NSNotificationName name in @[
                 NSWindowDidChangeOcclusionStateNotification, NSWindowDidMiniaturizeNotification,
                 NSWindowDidDeminiaturizeNotification, NSWindowDidUpdateNotification
             ]) {
            [NSNotificationCenter.defaultCenter addObserver:self
                                                   selector:@selector(visibilityChanged:)
                                                       name:name
                                                     object:window];
        }
        for (NSNotificationName name in @[
                 NSApplicationDidHideNotification, NSApplicationDidUnhideNotification,
                 NSApplicationDidChangeOcclusionStateNotification
             ]) {
            [NSNotificationCenter.defaultCenter addObserver:self
                                                   selector:@selector(visibilityChanged:)
                                                       name:name
                                                     object:NSApp];
        }
    }
    return self;
}
- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}
- (void)publishVisibility:(BOOL)force {
    NSWindow* window = _window;
    WKWebView* webView = _webView;
    if (!window || !webView)
        return;
    const bool visible = !NSApp.hidden && window.visible && !window.miniaturized &&
                         (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    if (!force && _published && visible == _visible)
        return;
    _published = true;
    _visible = visible;
    NSString* script =
        [NSString stringWithFormat:@"window.__loopzNativeVisible = %@;"
                                    "window.dispatchEvent(new Event('loopz-visibilitychange'));",
                                   visible ? @"true" : @"false"];
    [webView evaluateJavaScript:script completionHandler:nil];
}
- (void)visibilityChanged:(NSNotification*)notification {
    (void)notification;
    [self publishVisibility:NO];
}
- (void)didFinishNavigation {
    // A reload starts with polling suspended, even when the previous page was
    // visible. Always synchronize the newly loaded page with the real window.
    [self publishVisibility:YES];
}
@end
