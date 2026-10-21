#include "wire.hpp"
#include <dispatch/dispatch.h>
#include <unistd.h>

#include <atomic>
#include <utility>

namespace loopz::macos {
namespace {
constexpr int64_t kReplyTimeout = 3 * NSEC_PER_SEC;
std::string productionRequirement(const char* identifier) {
    return "identifier \"" + std::string(identifier) +
           "\" and anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists"
           " and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
           " and certificate leaf[subject.OU] = \"USJ7H3X44X\"";
}
uint32_t user() {
    if (geteuid() == 0)
        throw std::runtime_error("The editor connection requires an ordinary user session");
    return geteuid();
}
struct ClientState {
    std::atomic<bool> closed{false}, authenticated{false};
};
} // namespace

EditorServicePolicy productionEditorPolicy() {
    return {productionRequirement("com.lexasoft.loopz.editor"), user()};
}
EditorServiceEndpoint productionEditorEndpoint() {
    return {kEditorServiceName, productionRequirement("com.lexasoft.loopz.engine"), user(),
            nullptr};
}

struct EditorServiceClient::Impl {
    const EditorServiceEndpoint endpoint;
    const dispatch_queue_t queue;
    xpc_connection_t peer{nullptr};
    std::shared_ptr<ClientState> state;
    explicit Impl(EditorServiceEndpoint target)
        : endpoint(std::move(target)),
          queue(dispatch_queue_create("com.lexasoft.loopz.editor.client", DISPATCH_QUEUE_SERIAL)) {
        if (!queue || endpoint.serverRequirement.empty() || endpoint.serverUID == UINT32_MAX ||
            (!endpoint.endpoint &&
             (endpoint.serviceName.empty() || endpoint.serviceName.size() > 255 ||
              endpoint.serviceName.find('\0') != std::string::npos)))
            throw std::invalid_argument("Invalid authenticated editor endpoint");
    }

    std::string exchange(const char* operation, std::string_view payload = {}) {
        using namespace editor;
        const bool hello = std::string_view(operation) == "hello";
        if (!peer || !state || state->closed.load() ||
            (!hello &&
             (!state->authenticated.load() || xpc_connection_get_euid(peer) != endpoint.serverUID)))
            throw EditorServiceError(EditorFailure::Unavailable,
                                     "Editor connection is unavailable");
        auto request = message(operation);
        if (!payload.empty())
            xpc_dictionary_set_data(request, "payload", payload.data(), payload.size());
        struct Reply {
            std::string payload, failure;
            std::optional<EditorFailure> code;
        };
        auto reply = std::make_shared<Reply>();
        auto done = dispatch_semaphore_create(0);
        const auto current = state;
        const auto connection = peer;
        const auto uid = endpoint.serverUID;
        xpc_connection_send_message_with_reply(peer, request, queue, ^(xpc_object_t response) {
            try {
                if (current->closed.load() || xpc_connection_get_euid(connection) != uid)
                    throw std::runtime_error("Editor server identity is unavailable");
                protocol(response);
                const auto flag = xpc_dictionary_get_value(response, "ok");
                if (!flag || xpc_get_type(flag) != XPC_TYPE_BOOL)
                    throw std::invalid_argument("Editor reply is missing its result flag");
                if (xpc_bool_get_value(flag)) {
                    dictionary(response, 3);
                    reply->payload = data(response, kMaximumEditorReplyBytes);
                } else {
                    dictionary(response, 4);
                    reply->code = failureCode(text(response, "code", 32));
                    reply->failure = text(response, "detail");
                }
            } catch (const std::exception& error) {
                reply->failure = error.what();
                current->closed.store(true);
            }
            dispatch_semaphore_signal(done);
        });
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kReplyTimeout))) {
            current->closed.store(true);
            xpc_connection_cancel(peer);
            if (!hello && std::string_view(operation) != "snapshot")
                throw UncertainEditorOutcome();
            throw EditorServiceError(EditorFailure::Unavailable,
                                     "Editor server did not reply in time");
        }
        if (reply->code)
            throw EditorServiceError(*reply->code, reply->failure);
        if (current->closed.load() || reply->payload.empty()) {
            if (!hello && std::string_view(operation) != "snapshot")
                throw UncertainEditorOutcome();
            throw EditorServiceError(EditorFailure::Unavailable,
                                     reply->failure.empty()
                                         ? "Editor server disconnected or authentication failed"
                                         : reply->failure);
        }
        return reply->payload;
    }
};

EditorServiceClient::EditorServiceClient(EditorServiceEndpoint endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
EditorServiceClient::~EditorServiceClient() {
    disconnect();
}
void EditorServiceClient::disconnect() noexcept {
    if (impl_->state) {
        impl_->state->authenticated.store(false);
        impl_->state->closed.store(true);
    }
    if (impl_->peer) {
        xpc_connection_cancel(impl_->peer);
        impl_->peer = nullptr;
        dispatch_sync(impl_->queue, ^{
                      });
    }
    impl_->state.reset();
}
bool EditorServiceClient::connected() const noexcept {
    return impl_->state && impl_->state->authenticated.load() && !impl_->state->closed.load();
}
void EditorServiceClient::connect() {
    disconnect();
    auto state = std::make_shared<ClientState>();
    impl_->state = state;
    auto peer = impl_->endpoint.endpoint
                    ? xpc_connection_create_from_endpoint(impl_->endpoint.endpoint)
                    : xpc_connection_create_mach_service(impl_->endpoint.serviceName.c_str(),
                                                         impl_->queue, 0);
    impl_->peer = peer;
    if (!peer)
        throw EditorServiceError(EditorFailure::Unavailable,
                                 "Editor connection could not be created");
    xpc_connection_set_target_queue(peer, impl_->queue);
    if (xpc_connection_set_peer_code_signing_requirement(
            peer, impl_->endpoint.serverRequirement.c_str())) {
        disconnect();
        throw EditorServiceError(EditorFailure::Unavailable,
                                 "Editor server code identity could not be pinned");
    }
    const auto weak = std::weak_ptr<ClientState>(state);
    xpc_connection_set_event_handler(peer, ^(xpc_object_t) {
        if (const auto current = weak.lock())
            current->closed.store(true);
    });
    xpc_connection_activate(peer);
    try {
        (void)impl_->exchange("hello");
        state->authenticated.store(true);
    } catch (...) {
        disconnect();
        throw;
    }
}
std::string EditorServiceClient::snapshot() {
    return impl_->exchange("snapshot");
}
void EditorServiceClient::preview() {
    impl_->exchange("preview");
}
std::string EditorServiceClient::edit(std::string_view request) {
    if (request.empty() || request.size() > kMaximumEditorRequestBytes)
        throw std::invalid_argument("Editor request is empty or exceeds 1 MiB");
    return impl_->exchange("edit", request);
}
void EditorServiceClient::retry() {
    (void)impl_->exchange("retry");
}
std::string EditorServiceClient::initialize(std::string_view request) {
    if (request.empty() || request.size() > kMaximumEditorRequestBytes)
        throw std::invalid_argument("Editor request is empty or exceeds 1 MiB");
    return impl_->exchange("initialize", request);
}
std::string EditorServiceClient::hardwareBuffer(std::string_view request) {
    if (request.empty() || request.size() > 8192)
        throw std::invalid_argument("Invalid hardware buffer request size");
    return impl_->exchange("hardware-buffer", request);
}
} // namespace loopz::macos
