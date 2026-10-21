#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#include <loopz/macos/permissions.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>

namespace loopz::macos {
MicrophonePermission microphonePermission() noexcept {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusNotDetermined:
        return MicrophonePermission::NotDetermined;
    case AVAuthorizationStatusAuthorized:
        return MicrophonePermission::Authorized;
    case AVAuthorizationStatusDenied:
        return MicrophonePermission::Denied;
    case AVAuthorizationStatusRestricted:
        return MicrophonePermission::Restricted;
    }
    return MicrophonePermission::Unknown;
}

const char* permissionName(MicrophonePermission permission) noexcept {
    switch (permission) {
    case MicrophonePermission::NotDetermined:
        return "not-determined";
    case MicrophonePermission::Authorized:
        return "authorized";
    case MicrophonePermission::Denied:
        return "denied";
    case MicrophonePermission::Restricted:
        return "restricted";
    case MicrophonePermission::Unknown:
        return "unknown";
    }
    return "unknown";
}

PermissionRequest requestMicrophonePermission(std::chrono::seconds timeout) {
    if (![NSThread isMainThread] || timeout <= std::chrono::seconds::zero())
        throw std::invalid_argument(
            "Permission setup requires the main thread and a positive timeout");
    const auto initial = microphonePermission();
    if (initial != MicrophonePermission::NotDetermined)
        return {initial, false, true};
    auto* application = [NSApplication sharedApplication];
    const auto previousPolicy = application.activationPolicy;
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application finishLaunching];
    [application activate];
    struct Reply {
        std::atomic<bool> granted{false};
        std::atomic<bool> completed{false};
    };
    const auto reply = std::make_shared<Reply>();
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
                                 reply->granted.store(granted, std::memory_order_relaxed);
                                 reply->completed.store(true, std::memory_order_release);
                             }];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!reply->completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        @autoreleasepool {
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
    }
    const bool completed = reply->completed.load(std::memory_order_acquire);
    const auto status = completed && reply->granted.load(std::memory_order_relaxed)
                            ? MicrophonePermission::Authorized
                            : microphonePermission();
    [application setActivationPolicy:previousPolicy];
    return {status, true, completed};
}
} // namespace loopz::macos
