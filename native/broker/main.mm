#include <loopz/timeline.hpp>
#include <xpc/xpc.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pwd.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

int main(int argc, char** argv) {
    if ((argc != 3 && argc != 5) || std::strcmp(argv[1], "--engine-requirement")) {
        std::fprintf(stderr,
                     "usage: loopz-broker --engine-requirement <pinned code requirement>\n");
        return 2;
    }
    if (geteuid() != 0) {
        std::fprintf(stderr, "broker must be launched in the system domain\n");
        return 2;
    }
    const auto* audioUser = getpwnam("_coreaudiod");
    if (!audioUser)
        return 1;
    const auto driverUid = audioUser->pw_uid;
    auto* memory = mmap(nullptr, sizeof(loopz::SharedAudio), PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_SHARED, -1, 0);
    if (memory == MAP_FAILED)
        return 1;
    auto* audio = new (memory) loopz::SharedAudio{};
    auto shared = xpc_shmem_create(memory, sizeof(loopz::SharedAudio));
    auto queue = dispatch_queue_create("com.lexasoft.loopz.broker", DISPATCH_QUEUE_SERIAL);
    auto listener = xpc_connection_create_mach_service(loopz::kService, queue,
                                                       XPC_CONNECTION_MACH_SERVICE_LISTENER);
    // Modern macOS isolates each HAL plug-in in Apple's signed remote driver
    // service. Older systems host it in coreaudiod itself. Authenticate only
    // those two concrete Apple identities, with the same _coreaudiod UID gate.
    const std::string driverRequirement =
        "anchor apple and (identifier \"com.apple.audio.coreaudiod\" or identifier "
        "\"com.apple.audio.Core-Audio-Driver-Service.helper\")";
    if (argc == 5 && std::strcmp(argv[3], "--control-requirement"))
        return 2;
    const std::string helperRequirement = argv[2];
    const std::string controlRequirement = argc == 5 ? argv[4] : argv[2];
    const std::string rootControlRequirement =
        "(" + controlRequirement + ") or (" + helperRequirement + ")";
    std::string requirement = "(" + driverRequirement + ") or (" + helperRequirement + ") or (" +
                              controlRequirement + ")";
    if (!shared || !listener ||
        xpc_connection_set_peer_code_signing_requirement(listener, requirement.c_str()))
        return 1;
    __block xpc_connection_t helper = nullptr;
    __block xpc_connection_t driver = nullptr;
    __block uid_t owner = static_cast<uid_t>(-1);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t incoming) {
        if (xpc_get_type(incoming) != XPC_TYPE_CONNECTION)
            return;
        xpc_connection_t peer = (xpc_connection_t)incoming;
        __block uint32_t boundRole = 0;
        xpc_connection_set_target_queue(peer, queue);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
            if (xpc_get_type(event) == XPC_TYPE_ERROR) {
                // A new helper gets a fresh broker mapping, never a region in which
                // the old helper might still be finishing its final callback/block.
                if (peer == helper) {
                    audio->helperOnline.store(0);
                    _exit(0);
                }
                if (peer == driver)
                    driver = nullptr;
                return;
            }
            if (xpc_get_type(event) != XPC_TYPE_DICTIONARY)
                return;
            auto reply = xpc_dictionary_create_reply(event);
            if (!reply)
                return;
            uid_t console = static_cast<uid_t>(-1);
            gid_t group;
            auto user = SCDynamicStoreCopyConsoleUser(nullptr, &console, &group);
            if (user)
                CFRelease(user);
            const char* role = xpc_dictionary_get_string(event, "role");
            const auto uid = xpc_connection_get_euid(peer);
            const bool isHelper = role && !std::strcmp(role, "helper") && console > 0 &&
                                  console != static_cast<uid_t>(-1) && uid == console;
            const bool isControl =
                role && !std::strcmp(role, "control") &&
                (uid == 0 || (console > 0 && console != static_cast<uid_t>(-1) && uid == console));
            const bool isDriver =
                role && !std::strcmp(role, "driver") && (uid == driverUid || uid == 0);
            const uint32_t requestedRole = isHelper ? 1 : isDriver ? 2 : isControl ? 3 : 0;
            const char* operation = xpc_dictionary_get_string(event, "op");
            const char* error = nullptr;
            if (xpc_dictionary_get_uint64(event, "protocol") != loopz::kProtocol)
                error = "protocol mismatch";
            else if (!requestedRole)
                error = "connection role or active console identity rejected";
            else if (operation && !std::strcmp(operation, "hello") && !boundRole) {
                // No shared memory on the first message. XPC verifies the exact role
                // identity on the subsequent attach message, closing the OR-listener
                // identity/claimed-role ambiguity.
                if (xpc_connection_set_peer_code_signing_requirement(
                        peer, isHelper    ? helperRequirement.c_str()
                              : isControl ? (uid == 0 ? rootControlRequirement.c_str()
                                                      : helperRequirement.c_str())
                                          : driverRequirement.c_str()))
                    error = "cannot bind role-specific code identity";
                else {
                    boundRole = requestedRole;
                    xpc_dictionary_set_bool(reply, "ready", true);
                }
                if (error)
                    xpc_dictionary_set_string(reply, "error", error);
                xpc_connection_send_message(peer, reply);
                return;
            } else if (!operation || std::strcmp(operation, "attach") || boundRole != requestedRole)
                error = "complete role negotiation before attachment";
            else if (isHelper && helper && helper != peer)
                error = "one helper already owns this feasibility device";
            else if (isDriver && driver && driver != peer)
                error = "one driver already attached";
            else if (owner != static_cast<uid_t>(-1) && owner != console)
                error = "console session changed; restart broker before attaching";
            if (error)
                xpc_dictionary_set_string(reply, "error", error);
            else if (isControl) {
                xpc_dictionary_set_bool(reply, "status", true);
                xpc_dictionary_set_uint64(reply, "protocol", loopz::kProtocol);
                xpc_dictionary_set_bool(reply, "driverAttached", driver != nullptr);
                xpc_dictionary_set_bool(reply, "helperAttached", helper != nullptr);
                xpc_dictionary_set_int64(reply, "driverPID",
                                         driver ? xpc_connection_get_pid(driver) : 0);
            } else {
                if (isHelper) {
                    helper = peer;
                    owner = console;
                }
                if (isDriver)
                    driver = peer;
                xpc_dictionary_set_value(reply, "audio", shared);
            }
            xpc_connection_send_message(peer, reply);
        });
        xpc_connection_activate(peer);
    });
    // A console change exits the prototype broker. Its launchd restart creates a
    // fresh region, so old-user mappings never become the next user's audio.
    auto timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
                              NSEC_PER_MSEC * 50);
    dispatch_source_set_event_handler(timer, ^{
        if (owner == static_cast<uid_t>(-1))
            return;
        uid_t current;
        gid_t group;
        auto name = SCDynamicStoreCopyConsoleUser(nullptr, &current, &group);
        if (name)
            CFRelease(name);
        if (current != owner) {
            audio->helperOnline.store(0);
            _exit(0);
        }
    });
    dispatch_resume(timer);
    xpc_connection_activate(listener);
    dispatch_main();
}
