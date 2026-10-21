#include "wire.hpp"
#include <dispatch/dispatch.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <utility>

namespace loopz::macos {
namespace {
using namespace editor;
constexpr uint64_t kQueueLifetimeMs = 2000;
struct Peer {
    xpc_connection_t connection;
    bool alive{true}, authenticated{false}, preview{false};
    size_t outstanding{0};
};
struct Pending {
    std::shared_ptr<Peer> peer;
    xpc_object_t reply;
    std::string operation, payload;
    uint64_t deadline;
};
struct State {
    const EditorServicePolicy policy;
    std::mutex mutex;
    bool stopped{false}, listening{true};
    std::vector<std::shared_ptr<Peer>> peers;
    std::deque<std::unique_ptr<Pending>> requests;
    size_t outstanding{0}, inFlight{0};
    uint64_t completed{0}, rejected{0}, expired{0};
    explicit State(EditorServicePolicy configuration) : policy(std::move(configuration)) {}

    void remove(const std::shared_ptr<Peer>& peer) {
        std::lock_guard lock(mutex);
        peer->alive = false;
        peer->authenticated = false;
        std::erase(peers, peer);
        std::erase_if(requests, [&](const auto& request) {
            if (request->peer != peer)
                return false;
            --outstanding;
            --peer->outstanding;
            return true;
        });
        xpc_connection_cancel(peer->connection);
    }
    void reject(const std::shared_ptr<Peer>& peer, xpc_object_t reply, EditorFailure code,
                std::string_view detail) {
        {
            std::lock_guard lock(mutex);
            ++rejected;
        }
        failure(peer->connection, reply, code, detail);
    }
};

void receive(const std::shared_ptr<State>& state, const std::shared_ptr<Peer>& peer,
             xpc_object_t event) {
    if (xpc_get_type(event) == XPC_TYPE_ERROR) {
        state->remove(peer);
        return;
    }
    if (xpc_get_type(event) != XPC_TYPE_DICTIONARY) {
        state->remove(peer);
        return;
    }
    auto reply = xpc_dictionary_create_reply(event);
    if (!reply) {
        state->remove(peer);
        return;
    }
    try {
        protocol(event);
        if (xpc_connection_get_euid(peer->connection) != state->policy.clientUID)
            throw std::invalid_argument("Editor user identity rejected");
        const auto operation = text(event, "op", 32);
        dictionary(event, operation == "edit" || operation == "initialize" ||
                                  operation == "hardware-buffer"
                              ? 3
                              : 2);
        if (operation == "hello") {
            {
                std::lock_guard lock(state->mutex);
                if (state->stopped || !peer->alive)
                    return;
                peer->authenticated = true;
            }
            success(peer->connection, reply, "{\"version\":1}");
            return;
        }
        if (operation != "snapshot" && operation != "edit" && operation != "retry" &&
            operation != "initialize" && operation != "preview" && operation != "hardware-buffer")
            throw std::invalid_argument("Unknown editor operation");
        auto request = std::make_unique<Pending>();
        request->peer = peer;
        request->reply = reply;
        request->operation = operation;
        if (operation == "edit" || operation == "initialize" || operation == "hardware-buffer")
            request->payload =
                data(event, operation == "hardware-buffer" ? 8192 : kMaximumEditorRequestBytes);
        request->deadline = milliseconds() + kQueueLifetimeMs;
        bool admitted = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->stopped || !peer->alive || !peer->authenticated)
                throw std::invalid_argument("Editor connection has not completed authentication");
            if (state->outstanding < kMaximumEditorOutstanding &&
                peer->outstanding < kMaximumEditorPeerOutstanding) {
                state->requests.push_back(std::move(request));
                ++state->outstanding;
                ++peer->outstanding;
                admitted = true;
            }
        }
        if (!admitted)
            state->reject(peer, reply, EditorFailure::Busy, "Editor request queue is full");
    } catch (const std::invalid_argument& error) {
        state->reject(peer, reply, EditorFailure::Invalid, error.what());
    } catch (const std::exception& error) {
        state->reject(peer, reply, EditorFailure::Failed, error.what());
    }
}
} // namespace

struct EditorService::Impl {
    const EditorOperations operations;
    dispatch_queue_t queue;
    xpc_connection_t listener{nullptr};
    const std::shared_ptr<State> state;
    bool preview{false};
    void updatePreview() {
        bool wanted;
        {
            std::lock_guard lock(state->mutex);
            wanted = !state->stopped &&
                     std::any_of(state->peers.begin(), state->peers.end(), [](const auto& peer) {
                         return peer->alive && peer->authenticated && peer->preview;
                     });
        }
        if (preview != wanted) {
            if (operations.preview)
                operations.preview(wanted);
            preview = wanted;
        }
    }
    Impl(EditorServicePolicy policy, EditorOperations handlers, const std::string& name)
        : operations(std::move(handlers)),
          queue(dispatch_queue_create("com.lexasoft.loopz.editor.listener", DISPATCH_QUEUE_SERIAL)),
          state(std::make_shared<State>(std::move(policy))) {
        if (!queue || state->policy.clientRequirement.empty() ||
            state->policy.clientUID == UINT32_MAX || !operations.snapshot || !operations.edit ||
            !operations.retry || name.find('\0') != std::string::npos || name.size() > 255)
            throw std::invalid_argument("Invalid authenticated editor service configuration");
        listener = name.empty() ? xpc_connection_create(nullptr, queue)
                                : xpc_connection_create_mach_service(
                                      name.c_str(), queue, XPC_CONNECTION_MACH_SERVICE_LISTENER);
        if (!listener)
            throw std::runtime_error("Editor listener could not be created");
        const auto weak = std::weak_ptr<State>(state);
        const auto target = queue;
        xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
            const auto current = weak.lock();
            if (!current)
                return;
            if (xpc_get_type(event) == XPC_TYPE_ERROR) {
                std::lock_guard lock(current->mutex);
                current->listening = false;
                return;
            }
            if (xpc_get_type(event) != XPC_TYPE_CONNECTION)
                return;
            xpc_connection_t connection = static_cast<xpc_connection_t>(event);
            auto peer = std::make_shared<Peer>();
            peer->connection = connection;
            const auto weakPeer = std::weak_ptr<Peer>(peer);
            {
                std::lock_guard lock(current->mutex);
                if (current->stopped || current->peers.size() >= kMaximumEditorPeers) {
                    ++current->rejected;
                    xpc_connection_cancel(connection);
                    return;
                }
                if (xpc_connection_set_peer_code_signing_requirement(
                        connection, current->policy.clientRequirement.c_str())) {
                    ++current->rejected;
                    xpc_connection_cancel(connection);
                    return;
                }
                current->peers.push_back(peer);
                // stop() may run on the engine owner while this queue accepts
                // a peer. Activate under the same lock before exposing it to
                // cancellation; never activate an already-cancelled connection.
                xpc_connection_set_target_queue(connection, target);
                xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
                    const auto current = weak.lock();
                    const auto peer = weakPeer.lock();
                    if (current && peer)
                        receive(current, peer, message);
                });
                xpc_connection_activate(connection);
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), target, ^{
                const auto current = weak.lock();
                const auto peer = weakPeer.lock();
                if (!current || !peer)
                    return;
                bool expired;
                {
                    std::lock_guard lock(current->mutex);
                    expired = peer->alive && !peer->authenticated;
                }
                if (expired)
                    current->remove(peer);
            });
        });
        xpc_connection_activate(listener);
    }
};

EditorService::EditorService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::unique_ptr<EditorService> EditorService::anonymous(EditorServicePolicy policy,
                                                        EditorOperations operations) {
    return std::unique_ptr<EditorService>(
        new EditorService(std::make_unique<Impl>(std::move(policy), std::move(operations), "")));
}
std::unique_ptr<EditorService> EditorService::machService(EditorServicePolicy policy,
                                                          EditorOperations operations,
                                                          std::string name) {
    if (name.empty())
        throw std::invalid_argument("Editor Mach service name is missing");
    return std::unique_ptr<EditorService>(
        new EditorService(std::make_unique<Impl>(std::move(policy), std::move(operations), name)));
}
EditorService::~EditorService() {
    stop();
}
xpc_endpoint_t EditorService::endpoint() const {
    return xpc_endpoint_create(impl_->listener);
}
EditorServiceStatus EditorService::status() const {
    const auto& state = impl_->state;
    std::lock_guard lock(state->mutex);
    return {state->listening && !state->stopped,
            state->peers.size(),
            state->outstanding,
            state->inFlight,
            state->completed,
            state->rejected,
            state->expired};
}
void EditorService::poll() {
    const auto& state = impl_->state;
    impl_->updatePreview();
    std::unique_ptr<Pending> request;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopped || state->requests.empty())
            return;
        request = std::move(state->requests.front());
        state->requests.pop_front();
        ++state->inFlight;
    }
    bool expired = false;
    try {
        bool alive;
        {
            std::lock_guard lock(state->mutex);
            alive = request->peer->alive;
        }
        if (alive) {
            if (milliseconds() >= request->deadline) {
                expired = true;
                failure(request->peer->connection, request->reply, EditorFailure::Expired,
                        "Editor request expired before execution");
            } else {
                std::string result;
                if (request->operation == "snapshot")
                    result = impl_->operations.snapshot();
                else if (request->operation == "edit")
                    result = impl_->operations.edit(request->payload);
                else if (request->operation == "initialize") {
                    if (!impl_->operations.initialize)
                        throw EditorServiceError(EditorFailure::Unavailable,
                                                 "First-run setup is not available");
                    result = impl_->operations.initialize(request->payload);
                } else if (request->operation == "hardware-buffer") {
                    if (!impl_->operations.hardwareBuffer)
                        throw EditorServiceError(EditorFailure::Unavailable,
                                                 "Engine buffer settings are not available");
                    result = impl_->operations.hardwareBuffer(request->payload);
                } else if (request->operation == "preview") {
                    {
                        std::lock_guard lock(state->mutex);
                        request->peer->preview = true;
                    }
                    impl_->updatePreview();
                    result = "{\"version\":1,\"requested\":true}";
                } else {
                    impl_->operations.retry();
                    result = "{\"version\":1,\"requested\":true}";
                }
                success(request->peer->connection, request->reply, result);
            }
        }
    } catch (const EditorServiceError& error) {
        failure(request->peer->connection, request->reply, error.code, error.what());
    } catch (const StaleEditorRevision& error) {
        failure(request->peer->connection, request->reply, EditorFailure::Conflict, error.what());
    } catch (const std::invalid_argument& error) {
        failure(request->peer->connection, request->reply, EditorFailure::Invalid, error.what());
    } catch (const std::exception& error) {
        failure(request->peer->connection, request->reply, EditorFailure::Failed, error.what());
    }
    {
        std::lock_guard lock(state->mutex);
        --state->outstanding;
        --state->inFlight;
        --request->peer->outstanding;
        ++state->completed;
        if (expired)
            ++state->expired;
    }
}
void EditorService::stop() noexcept {
    const auto& state = impl_->state;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopped)
            return;
        state->stopped = true;
        for (const auto& peer : state->peers) {
            peer->alive = false;
            xpc_connection_cancel(peer->connection);
        }
        state->peers.clear();
        for (const auto& request : state->requests)
            --request->peer->outstanding;
        state->outstanding -= state->requests.size();
        state->requests.clear();
    }
    xpc_connection_cancel(impl_->listener);
    dispatch_sync(impl_->queue, ^{
                  });
    try {
        impl_->updatePreview();
    } catch (...) {
        // Destruction still cancels the peer; runtime teardown closes inputs.
    }
}
} // namespace loopz::macos
