#include "audio_maintenance_service.hpp"
#include "audio_maintenance.hpp"
#include <xpc/xpc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <string_view>
#include <stdexcept>

namespace loopz::macos {
void serveAudioMaintenance() {
    if (geteuid() != 0)
        throw std::runtime_error("Audio maintenance needs administrator authorization.");
    auto queue = dispatch_queue_create("com.lexasoft.loopz.setup.requests", DISPATCH_QUEUE_SERIAL);
    auto work = dispatch_queue_create("com.lexasoft.loopz.setup.work", DISPATCH_QUEUE_SERIAL);
    auto listener = xpc_connection_create_mach_service("com.lexasoft.loopz.setup", queue,
                                                       XPC_CONNECTION_MACH_SERVICE_LISTENER);
    const char* requirement =
        "anchor apple generic and identifier \"com.lexasoft.loopz.editor\" and "
        "certificate leaf[subject.OU] = \"USJ7H3X44X\" and "
        "certificate leaf[field.1.2.840.113635.100.6.1.13] exists";
    if (!listener)
        throw std::runtime_error("Audio maintenance could not verify its app identity.");
    const auto busy = std::make_shared<std::atomic<bool>>(false);
    const auto generation = std::make_shared<std::atomic<uint64_t>>(0);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION)
            return;
        auto peer = static_cast<xpc_connection_t>(event);
        if (xpc_connection_set_peer_code_signing_requirement(peer, requirement)) {
            xpc_connection_cancel(peer);
            return;
        }
        xpc_connection_set_target_queue(peer, queue);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
            if (xpc_get_type(message) != XPC_TYPE_DICTIONARY)
                return;
            auto reply = xpc_dictionary_create_reply(message);
            if (!reply)
                return;
            const auto respond = ^(NSDictionary* result) {
                NSData* data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
                xpc_dictionary_set_data(reply, "result", data.bytes, data.length);
                xpc_connection_send_message(peer, reply);
            };
            struct stat console{};
            const char* action = xpc_dictionary_get_string(message, "action");
            auto version = xpc_dictionary_get_value(message, "version");
            const bool valid =
                !stat("/dev/console", &console) && console.st_uid >= 501 &&
                xpc_connection_get_euid(peer) == console.st_uid &&
                xpc_dictionary_get_count(message) == 2 && version &&
                xpc_get_type(version) == XPC_TYPE_UINT64 &&
                xpc_dictionary_get_uint64(message, "version") == 1 && action &&
                (std::string_view(action) == "install" || std::string_view(action) == "update" ||
                 std::string_view(action) == "repair" || std::string_view(action) == "activate");
            if (!valid) {
                respond(@{@"ok" : @NO, @"detail" : @"Audio maintenance request was rejected."});
                return;
            }
            if (busy->exchange(true)) {
                respond(@{@"ok" : @NO, @"detail" : @"Audio maintenance is already running."});
                return;
            }
            generation->fetch_add(1);
            const bool activate = std::string_view(action) == "activate";
            dispatch_async(work, ^{
                @autoreleasepool {
                    try {
                        struct stat activeConsole{};
                        if (stat("/dev/console", &activeConsole) ||
                            activeConsole.st_uid != console.st_uid)
                            throw std::runtime_error(
                                "The active user changed. Retry audio maintenance.");
                        verifyAudioPayload("/Applications/LoopZ.app");
                        respond(maintainAudioComponents("/Applications/LoopZ.app", activate,
                                                        console.st_uid));
                    } catch (const std::exception& error) {
                        respond(@{@"ok" : @NO, @"detail" : @(error.what())});
                    }
                    // Exit only after the reply is sent. The next request loads the
                    // helper from the current signed app, including after updates.
                    const auto idleGeneration = generation->fetch_add(1) + 1;
                    busy->store(false);
                    xpc_connection_send_barrier(peer, ^{
                        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), queue, ^{
                            if (!busy->load() && generation->load() == idleGeneration)
                                exit(0);
                        });
                    });
                }
            });
        });
        xpc_connection_activate(peer);
    });
    xpc_connection_activate(listener);
    dispatch_main();
}
} // namespace loopz::macos
