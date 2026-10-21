#pragma once
#import <Foundation/Foundation.h>
namespace loopz::macos {
NSDictionary* requestAudioMaintenance(NSString* action);
void serveAudioMaintenance();
} // namespace loopz::macos
