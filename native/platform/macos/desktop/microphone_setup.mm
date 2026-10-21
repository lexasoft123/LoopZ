#include "microphone_setup.hpp"
#import <Foundation/Foundation.h>
#include <stdexcept>

namespace loopz::macos {
PermissionRequest decodeMicrophoneSetup(const SessionCommandResult& response) {
    auto* data = [NSData dataWithBytes:response.output.data() length:response.output.size()];
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    const auto invalid = [] {
        throw std::runtime_error("The microphone request returned an invalid response.");
    };
    if (![value isKindOfClass:NSDictionary.class] || [value count] != 3)
        invalid();
    for (NSString* name in @[ @"requested", @"requestCompleted" ])
        if (![value[name] isKindOfClass:NSNumber.class] ||
            CFGetTypeID((__bridge CFTypeRef)value[name]) != CFBooleanGetTypeID())
            invalid();
    NSString* name = value[@"microphone"];
    if (![name isKindOfClass:NSString.class])
        invalid();
    MicrophonePermission permission;
    if ([name isEqual:@"authorized"])
        permission = MicrophonePermission::Authorized;
    else if ([name isEqual:@"denied"])
        permission = MicrophonePermission::Denied;
    else if ([name isEqual:@"restricted"])
        permission = MicrophonePermission::Restricted;
    else if ([name isEqual:@"not-determined"])
        permission = MicrophonePermission::NotDetermined;
    else if ([name isEqual:@"unknown"])
        permission = MicrophonePermission::Unknown;
    else {
        invalid();
        return {};
    }
    const bool requested = [value[@"requested"] boolValue];
    const bool completed = [value[@"requestCompleted"] boolValue];
    if ((completed && !requested) ||
        response.exitCode != (permission == MicrophonePermission::Authorized ? 0 : 1))
        invalid();
    return {permission, requested, !requested || completed};
}

MicrophoneSetup productionMicrophoneSetup() {
    return [](const std::atomic<bool>& running) {
        // Reuse the fixed Developer ID identity/capability validation. Neither
        // the page nor the environment chooses a bundle, executable or argument.
        productionSessionHost()->validateEngine();
        return decodeMicrophoneSetup(sessionCommand(
            {"/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine", "--request-microphone"},
            // Foreground Cocoa diagnostics must not contaminate stdout's JSON.
            std::chrono::seconds(65), &running, SessionCommandOutput::StandardOutput));
    };
}
} // namespace loopz::macos
