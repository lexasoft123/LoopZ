#include "audio_maintenance_service.hpp"
#import <ServiceManagement/ServiceManagement.h>
#import <CoreServices/CoreServices.h>
#include <xpc/xpc.h>
#include <memory>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace loopz::macos {
NSDictionary* requestAudioMaintenance(NSString* action) {
    if (![@[ @"install", @"update", @"repair", @"activate", @"status" ] containsObject:action])
        return @{@"ok" : @NO, @"detail" : @"Invalid audio maintenance action."};
    SMAppService* service =
        [SMAppService daemonServiceWithPlistName:@"com.lexasoft.loopz.setup.plist"];
    if ([action isEqual:@"status"])
        return @{@"ok" : @YES, @"registrationStatus" : @(service.status)};
    if (LSRegisterURL((__bridge CFURLRef)NSBundle.mainBundle.bundleURL, true) != noErr)
        return @{
            @"ok" : @NO,
            @"detail" : @"LoopZ could not register its app identity. Reopen the app and try again."
        };
    service = [SMAppService daemonServiceWithPlistName:@"com.lexasoft.loopz.setup.plist"];
    NSMutableData* registrationBytes = [NSMutableData data];
    for (NSString* relative in @[
             @"Contents/Library/Helpers/LoopZSetup",
             @"Contents/Library/LaunchDaemons/com.lexasoft.loopz.setup.plist"
         ]) {
        NSData* data = [NSData dataWithContentsOfFile:[NSBundle.mainBundle.bundlePath
                                                          stringByAppendingPathComponent:relative]];
        if (!data || !data.length || data.length > 4 * 1024 * 1024)
            return @{
                @"ok" : @NO,
                @"detail" : @"Reinstall the complete LoopZ app before repairing audio."
            };
        const uint64_t size = data.length;
        [registrationBytes appendBytes:&size length:sizeof(size)];
        [registrationBytes appendData:data];
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(registrationBytes.bytes, static_cast<CC_LONG>(registrationBytes.length), digest);
    NSData* fingerprint = [NSData dataWithBytes:digest length:sizeof(digest)];
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    NSString* key = @"LoopZAudioMaintenanceRegistration";
    if ((service.status == SMAppServiceStatusEnabled ||
         service.status == SMAppServiceStatusRequiresApproval) &&
        ![[defaults objectForKey:key] isEqual:fingerprint]) {
        const int lock = open("/Library/Application Support/LoopZ/.install.lock",
                              O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (lock >= 0) {
            const bool busy = flock(lock, LOCK_SH | LOCK_NB) != 0;
            close(lock);
            if (busy)
                return @{
                    @"ok" : @NO,
                    @"detail" :
                        @"Audio maintenance is already running. Wait before updating the helper."
                };
        }
        auto done = dispatch_semaphore_create(0);
        struct Unregistration {
            NSError* error{nil};
        };
        const auto result = std::make_shared<Unregistration>();
        [service unregisterWithCompletionHandler:^(NSError* error) {
            result->error = error;
            dispatch_semaphore_signal(done);
        }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)))
            return
                @{@"ok" : @NO,
                  @"detail" : @"The audio helper is still stopping. Retry shortly."};
        // macOS can report the completion before its registration status
        // catches up. Wait for the observed removal, rather than treating a
        // transient unregister error as a permanent installation failure.
        for (int attempt = 0;
             attempt < 30 && (service.status == SMAppServiceStatusEnabled ||
                              service.status == SMAppServiceStatusRequiresApproval);
             ++attempt)
            [NSThread sleepForTimeInterval:0.1];
        if (result->error && result->error.code != kSMErrorJobNotFound &&
            service.status != SMAppServiceStatusNotRegistered &&
            service.status != SMAppServiceStatusNotFound)
            return @{
                @"ok" : @NO,
                @"detail" : result->error.localizedDescription,
                @"stage" : @"unregister",
                @"registrationStatus" : @(service.status),
                @"errorDomain" : result->error.domain,
                @"errorCode" : @(result->error.code)
            };
        // Completion and status can precede macOS's permission-cache cleanup.
        // Delay before the first register attempt: an immediate attempt can
        // cache EPERM for the rest of this process (Apple forum 783539).
        [NSThread sleepForTimeInterval:2.0];
    }
    // Resolve the service again after removal; retain no stale registration
    // object across a helper executable change.
    service = [SMAppService daemonServiceWithPlistName:@"com.lexasoft.loopz.setup.plist"];
    NSError* registrationError = nil;
    if (service.status != SMAppServiceStatusEnabled &&
        service.status != SMAppServiceStatusRequiresApproval) {
        // Some macOS versions briefly retain the old registration after the
        // documented completion callback. Retry only this bounded transition.
        for (int attempt = 0; attempt < 13; ++attempt) {
            // Re-enter the main run loop after asynchronous unregistration.
            // Apple documents a macOS race when registering directly from its
            // completion queue (Developer Forums thread 783539).
            __block NSError* attemptError = nil;
            __block BOOL registered = NO;
            const auto registerService = ^{
                registered = [service registerAndReturnError:&attemptError];
            };
            if ([NSThread isMainThread])
                registerService();
            else
                dispatch_sync(dispatch_get_main_queue(), registerService);
            registrationError = attemptError;
            if (registered || service.status == SMAppServiceStatusRequiresApproval ||
                service.status == SMAppServiceStatusEnabled)
                break;
            if (attempt != 12)
                [NSThread sleepForTimeInterval:0.25];
        }
    }
    if (service.status == SMAppServiceStatusEnabled ||
        service.status == SMAppServiceStatusRequiresApproval)
        [defaults setObject:fingerprint forKey:key];
    if (service.status == SMAppServiceStatusRequiresApproval ||
        (registrationError.code == kSMErrorLaunchDeniedByUser &&
         [registrationError.domain isEqual:@"SMAppServiceErrorDomain"]))
        return @{
            @"ok" : @NO,
            @"code" : @"approval-required",
            @"detail" : @"Allow LoopZ in System Settings → General → Login Items & Extensions, "
                        @"then try again."
        };
    if (service.status != SMAppServiceStatusEnabled)
        return @{
            @"ok" : @NO,
            @"stage" : @"register",
            @"registrationStatus" : @(service.status),
            @"errorDomain" : registrationError.domain ?: @"",
            @"errorCode" : @(registrationError.code),
            @"detail" : registrationError.localizedDescription
                ?: @"Audio maintenance could not be registered. Retry the setup check."
        };
    auto connection = xpc_connection_create_mach_service("com.lexasoft.loopz.setup", nullptr,
                                                         XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    if (!connection)
        return @{@"ok" : @NO, @"detail" : @"The audio maintenance service could not be opened."};
    const char* requirement =
        "anchor apple generic and identifier \"com.lexasoft.loopz.setup\" and "
        "certificate leaf[subject.OU] = \"USJ7H3X44X\" and "
        "certificate leaf[field.1.2.840.113635.100.6.1.13] exists";
    if (xpc_connection_set_peer_code_signing_requirement(connection, requirement)) {
        xpc_connection_cancel(connection);
        return @{
            @"ok" : @NO,
            @"detail" : @"The audio maintenance service identity could not be verified."
        };
    }
    xpc_connection_set_event_handler(connection, ^(xpc_object_t){
                                     });
    xpc_connection_activate(connection);
    auto request = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(request, "version", 1);
    xpc_dictionary_set_string(request, "action", action.UTF8String);
    struct Result {
        NSDictionary* value{nil};
    };
    const auto result = std::make_shared<Result>();
    auto done = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(
        connection, request, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
        ^(xpc_object_t reply) {
            if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
                xpc_connection_get_euid(connection) == 0) {
                size_t length = 0;
                const void* bytes = xpc_dictionary_get_data(reply, "result", &length);
                if (bytes && length && length <= 65536) {
                    NSData* data = [NSData dataWithBytes:bytes length:length];
                    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
                    if ([value isKindOfClass:NSDictionary.class])
                        result->value = value;
                }
            }
            dispatch_semaphore_signal(done);
        });
    const bool timeout =
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_SEC)) != 0;
    xpc_connection_cancel(connection);
    if (timeout)
        return @{
            @"ok" : @NO,
            @"detail" : @"Audio maintenance is still running. Wait before trying again."
        };
    return result->value ?: @{
        @"ok" : @NO,
        @"detail" : @"The audio maintenance service did not respond. Retry Repair audio."
    };
}
} // namespace loopz::macos
