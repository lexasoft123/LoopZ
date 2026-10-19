#include <loopz/bridge.hpp>
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>

namespace loopz {
BridgeStatus inspectBridge(std::string& error) {
    auto connection = xpc_connection_create_mach_service(kService, nullptr,
                                                         XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    if (!connection) {
        error = "cannot create status connection";
        return {};
    }
    xpc_connection_set_event_handler(connection, ^(xpc_object_t){
                                     });
    xpc_connection_activate(connection);
    struct Reply {
        xpc_object_t value{nullptr};
    };
    auto request = [&](const char* operation) -> xpc_object_t {
        auto holder = std::make_shared<Reply>();
        auto done = dispatch_semaphore_create(0);
        auto message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(message, "role", "control");
        xpc_dictionary_set_string(message, "op", operation);
        xpc_dictionary_set_uint64(message, "protocol", kProtocol);
        xpc_connection_send_message_with_reply(
            connection, message, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
            ^(xpc_object_t reply) {
                holder->value = reply;
                dispatch_semaphore_signal(done);
            });
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)))
            return nullptr;
        return holder->value;
    };
    auto hello = request("hello");
    if (!hello || xpc_get_type(hello) != XPC_TYPE_DICTIONARY ||
        !xpc_dictionary_get_bool(hello, "ready")) {
        error = "status handshake rejected or unavailable";
        xpc_connection_cancel(connection);
        return {};
    }
    auto reply = request("attach");
    BridgeStatus status;
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY ||
        !xpc_dictionary_get_bool(reply, "status"))
        error = "status query rejected or unavailable";
    else
        status = {static_cast<uint32_t>(xpc_dictionary_get_uint64(reply, "protocol")),
                  xpc_dictionary_get_bool(reply, "driverAttached"),
                  xpc_dictionary_get_bool(reply, "helperAttached")};
    xpc_connection_cancel(connection);
    return status;
}
} // namespace loopz
