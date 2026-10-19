#include <loopz/bridge.hpp>
#include <xpc/xpc.h>
#include <sys/mman.h>
#include <dispatch/dispatch.h>

namespace loopz {
struct BridgeClient::Impl {
    xpc_connection_t connection{nullptr};
    SharedAudio* memory{nullptr};
    size_t length{0};
    // 0 connecting, 1 connected, 2 terminal. A connection interruption can
    // never be overwritten by the completion of an earlier successful reply.
    std::shared_ptr<std::atomic<uint32_t>> state{std::make_shared<std::atomic<uint32_t>>(0)};
    ~Impl() {
        if (connection)
            xpc_connection_cancel(connection);
        if (memory)
            munmap(memory, length);
    }
};
BridgeClient::BridgeClient() : impl_(std::make_unique<Impl>()) {}
BridgeClient::~BridgeClient() = default;
bool BridgeClient::connect(const char* role, std::string& error) {
    if (impl_->connection) {
        error = "bridge client is single-use";
        return false;
    }
    auto connection = xpc_connection_create_mach_service(kService, nullptr,
                                                         XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    impl_->connection = connection;
    if (!connection) {
        error = "cannot create bridge connection";
        return false;
    }
    auto state = impl_->state;
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_ERROR)
            state->store(2);
    });
    xpc_connection_activate(connection);
    // Asynchronous request with a bounded control-domain wait. A missing daemon
    // cannot hang a CoreAudio initialization thread indefinitely.
    struct Reply {
        xpc_object_t value{nullptr};
    };
    auto request = [&](const char* operation) -> xpc_object_t {
        auto semaphore = dispatch_semaphore_create(0);
        auto holder = std::make_shared<Reply>();
        auto message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(message, "role", role);
        xpc_dictionary_set_string(message, "op", operation);
        xpc_dictionary_set_uint64(message, "protocol", kProtocol);
        xpc_connection_send_message_with_reply(
            connection, message, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
            ^(xpc_object_t response) {
                holder->value = response;
                dispatch_semaphore_signal(semaphore);
            });
        if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)))
            return nullptr;
        return holder->value;
    };
    auto hello = request("hello");
    if (!hello || xpc_get_type(hello) != XPC_TYPE_DICTIONARY ||
        !xpc_dictionary_get_bool(hello, "ready")) {
        error = "bridge unavailable or role-specific code identity rejected";
        xpc_connection_cancel(connection);
        return false;
    }
    auto reply = request("attach");
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY) {
        error = "bridge unavailable or code identity rejected";
        return false;
    }
    auto memory = xpc_dictionary_get_value(reply, "audio");
    if (!memory || xpc_get_type(memory) != XPC_TYPE_SHMEM) {
        const char* why = xpc_dictionary_get_string(reply, "error");
        error = why ? why : "bridge rejected attachment";
        return false;
    }
    void* region = nullptr;
    auto size = xpc_shmem_map(memory, &region);
    if (size < sizeof(SharedAudio) || !valid(static_cast<SharedAudio*>(region))) {
        if (region)
            munmap(region, size);
        error = "bridge schema or memory size mismatch";
        return false;
    }
    impl_->memory = static_cast<SharedAudio*>(region);
    impl_->length = size;
    uint32_t connecting = 0;
    if (!state->compare_exchange_strong(connecting, 1)) {
        error = "bridge disconnected during attachment";
        return false;
    }
    return true;
}
bool BridgeClient::connected() const noexcept {
    return impl_->state->load() == 1;
}
SharedAudio* BridgeClient::audio() const noexcept {
    return impl_->memory;
}
} // namespace loopz
