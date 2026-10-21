#include "login.hpp"
#import <ServiceManagement/ServiceManagement.h>
#include <stdexcept>

namespace loopz::macos {
namespace {
SMAppService* service() {
    return [SMAppService agentServiceWithPlistName:@"com.lexasoft.loopz.login.plist"];
}
bool installed() {
    return
        [NSBundle.mainBundle.bundlePath isEqual:@"/Applications/LoopZ.app"] &&
        [NSFileManager.defaultManager
            fileExistsAtPath:[NSBundle.mainBundle.bundlePath
                                 stringByAppendingPathComponent:@"Contents/Library/LaunchAgents/"
                                                                @"com.lexasoft.loopz.login.plist"]];
}
} // namespace
NSDictionary* loginStartupStatus() {
    const bool available = installed();
    const auto status = available ? service().status : SMAppServiceStatusNotFound;
    return @{
        @"available" : @(available),
        @"enabled" : @(status == SMAppServiceStatusEnabled),
        @"requiresApproval" : @(status == SMAppServiceStatusRequiresApproval),
        @"registered" :
            @(status == SMAppServiceStatusEnabled || status == SMAppServiceStatusRequiresApproval),
        @"detail" : available
            ? @"Login startup is managed by macOS. Turning it off leaves current routing running."
            : @"Install LoopZ in Applications to choose start at login."
    };
}
void setLoginStartup(bool enabled) {
    if (!installed())
        throw std::runtime_error("Install LoopZ in Applications to change login startup.");
    auto* item = service();
    if ((enabled && item.status == SMAppServiceStatusEnabled) ||
        (!enabled && item.status == SMAppServiceStatusNotRegistered))
        return;
    if (enabled && item.status == SMAppServiceStatusRequiresApproval)
        return;
    NSError* error = nil;
    const bool changed =
        enabled ? [item registerAndReturnError:&error] : [item unregisterAndReturnError:&error];
    if (!changed)
        throw std::runtime_error(error.localizedDescription.UTF8String
                                     ?: "macOS could not update login startup.");
}
void openLoginStartupSettings() {
    [SMAppService openSystemSettingsLoginItems];
}
} // namespace loopz::macos
