#include "login.hpp"
#import <ServiceManagement/ServiceManagement.h>
#include <mutex>
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
// The editor state is polled every ~100 ms. Each SMAppService.status read makes
// macOS look the background item up, so reuse a recent answer.
std::mutex statusLock;
SMAppServiceStatus cachedStatus = SMAppServiceStatusNotFound;
CFAbsoluteTime cachedAt = 0;
void invalidateStatus() {
    std::lock_guard guard(statusLock);
    cachedAt = 0;
}
SMAppServiceStatus currentStatus() {
    std::lock_guard guard(statusLock);
    const auto now = CFAbsoluteTimeGetCurrent();
    if (cachedAt == 0 || now - cachedAt > 2.0 || now < cachedAt) {
        cachedStatus = service().status;
        cachedAt = now;
    }
    return cachedStatus;
}
} // namespace
NSDictionary* loginStartupStatus() {
    const bool available = installed();
    const auto status = available ? currentStatus() : SMAppServiceStatusNotFound;
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
    invalidateStatus();
    if (!changed)
        throw std::runtime_error(error.localizedDescription.UTF8String
                                     ?: "macOS could not update login startup.");
}
void openLoginStartupSettings() {
    [SMAppService openSystemSettingsLoginItems];
}
} // namespace loopz::macos
