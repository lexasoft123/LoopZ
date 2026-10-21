#pragma once

#include <CoreAudio/CoreAudio.h>
@class NSNotificationCenter;

namespace loopz::macos {
// Native boundary injection, used to exercise reset/re-registration failures
// without resetting the system service or opening any device.
struct RecoveryEventBackend {
    decltype(&AudioObjectAddPropertyListenerBlock) add;
    decltype(&AudioObjectRemovePropertyListenerBlock) remove;
    NSNotificationCenter* workspaceCenter;
};
} // namespace loopz::macos
