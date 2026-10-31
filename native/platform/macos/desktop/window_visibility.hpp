#pragma once

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

void loopzInstallVisibilityScript(WKWebViewConfiguration* configuration);

// Owns only view scheduling. It never edits routing, stops the Engine or changes
// the editor connection's authenticated control contract.
@interface LZWindowVisibility : NSObject
- (instancetype)initWithWindow:(NSWindow*)window webView:(WKWebView*)webView;
- (void)didFinishNavigation;
@end
