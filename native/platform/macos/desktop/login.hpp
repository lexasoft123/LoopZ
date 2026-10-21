#pragma once
#import <Foundation/Foundation.h>

namespace loopz::macos {
NSDictionary* loginStartupStatus();
void setLoginStartup(bool enabled);
void openLoginStartupSettings();
} // namespace loopz::macos
