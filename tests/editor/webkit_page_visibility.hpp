#pragma once

#import <WebKit/WebKit.h>

// Offscreen render/transport fixtures are logically visible. Never use this
// override in the profile that exercises real WindowServer visibility.
inline void loopzConfigureOffscreenFixtureVisibility(WKWebViewConfiguration* configuration) {
    [configuration.userContentController
        addUserScript:
            [[WKUserScript alloc]
                  initWithSource:@"Object.defineProperty(document, 'hidden', {get: () => false});"
                   injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                forMainFrameOnly:YES]];
}
