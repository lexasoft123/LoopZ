#pragma once

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <loopz/macos/editor_service.hpp>
#include "session.hpp"
#include "microphone_setup.hpp"
#include "installation_setup.hpp"

@interface LZResources : NSObject <WKURLSchemeHandler>
- (instancetype)initWithRoot:(NSURL*)root;
@end

@interface LZBridge : NSObject <WKScriptMessageHandlerWithReply>
- (instancetype)initWithEndpoint:(loopz::macos::EditorServiceEndpoint)endpoint;
// Internal dependency injection for isolated integration fixtures. Production
// entry points retain the fixed endpoint and production session host.
- (instancetype)initWithEndpoint:(loopz::macos::EditorServiceEndpoint)endpoint
                     sessionHost:(std::unique_ptr<loopz::macos::SessionHost>)sessionHost;
- (instancetype)initWithEndpoint:(loopz::macos::EditorServiceEndpoint)endpoint
                     sessionHost:(std::unique_ptr<loopz::macos::SessionHost>)sessionHost
                 microphoneSetup:(loopz::macos::MicrophoneSetup)microphoneSetup;
- (instancetype)initWithEndpoint:(loopz::macos::EditorServiceEndpoint)endpoint
                     sessionHost:(std::unique_ptr<loopz::macos::SessionHost>)sessionHost
                 microphoneSetup:(loopz::macos::MicrophoneSetup)microphoneSetup
               installationProbe:(loopz::macos::InstallationProbe)installationProbe;
- (void)shutdown:(dispatch_block_t)completion;
- (NSDictionary*)editingContext;
@end

bool loopzLocalUIURL(NSURL* url);
// Share production preferences with isolated WebKit integration fixtures.
void loopzConfigureWebView(WKWebViewConfiguration* configuration);
bool loopzValidateDesktopBundle();
int loopzPrintRoutingStatus();
