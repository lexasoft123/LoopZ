#include "connection.hpp"
#include <dispatch/dispatch.h>

#include <mutex>
#include <utility>

namespace loopz::macos::routing {
namespace {
constexpr int64_t kReplyTimeout = 3 * NSEC_PER_SEC;
}
struct Connection::State {
    struct Snapshot {
        bool terminal;
        uint64_t epoch;
        uint64_t token;
        uint32_t sampleRateMask;
    };
    std::mutex mutex;
    bool terminal{false}, paired{false};
    uint64_t epoch{0}, token{0};
    uint32_t rates{kLegacyVirtualSampleRates};
    bool rateCapabilities{false};
    const std::shared_ptr<std::atomic<uint64_t>> sequence;
    const std::function<void(bool)> availability;
    const std::function<void(uint32_t)> ratesChanged;
    Handler handler;
    State(std::shared_ptr<std::atomic<uint64_t>> counter, Handler operation,
          std::function<void(bool)> notice, std::function<void(uint32_t)> rateNotice)
        : sequence(std::move(counter)), availability(std::move(notice)),
          ratesChanged(std::move(rateNotice)), handler(std::move(operation)) {}
    Snapshot snapshot() {
        std::lock_guard lock(mutex);
        return {terminal, epoch, token, rates};
    }
    void negotiateRates(bool supported) {
        std::lock_guard lock(mutex);
        rateCapabilities = supported;
    }
    void close() {
        {
            std::lock_guard lock(mutex);
            terminal = true;
            token = 0;
            rates = kLegacyVirtualSampleRates;
        }
        if (availability)
            availability(false);
        if (ratesChanged)
            ratesChanged(kLegacyVirtualSampleRates);
    }
    void observe(xpc_object_t object) {
        protocol(object);
        const auto incoming = integer(object, "epoch");
        const auto ready = boolean(object, "paired");
        if (!incoming)
            throw std::invalid_argument("Missing routing pairing identity");
        bool changed = false;
        uint32_t incomingRates = kLegacyVirtualSampleRates;
        bool ratesDiffer = false;
        {
            std::lock_guard lock(mutex);
            if (rateCapabilities) {
                incomingRates = routing::sampleRateMask(object);
                if (!ready && incomingRates != kLegacyVirtualSampleRates)
                    throw std::invalid_argument("Unpaired routing rates must be conservative");
            }
            if (terminal || incoming < epoch)
                return;
            if (incoming == epoch && ready != paired)
                throw std::invalid_argument("Pairing changed without a new identity");
            if (incoming == epoch && incomingRates != rates)
                throw std::invalid_argument("Routing rates changed without a new identity");
            if (incoming == epoch)
                return;
            changed = ready != paired;
            epoch = incoming;
            paired = ready;
            token = 0;
            ratesDiffer = rates != incomingRates;
            rates = incomingRates;
            if (ready) {
                auto previous = sequence->load();
                do {
                    if (previous == UINT64_MAX) {
                        terminal = true;
                        throw std::overflow_error("Routing session identity exhausted");
                    }
                } while (!sequence->compare_exchange_weak(previous, previous + 1));
                token = previous + 1;
            }
        }
        if (ratesDiffer && ratesChanged)
            ratesChanged(incomingRates);
        if (changed && availability)
            availability(ready);
    }
};
struct Connection::Impl {
    const RoutingServiceEndpoint endpoint;
    const std::string role;
    const Handler handler;
    const std::function<void(bool)> availability;
    uint32_t sampleRateMask{kAllVirtualSampleRates};
    std::function<void(uint32_t)> ratesChanged;
    dispatch_queue_t queue;
    xpc_connection_t peer{nullptr};
    const std::shared_ptr<std::atomic<uint64_t>> sequence{
        std::make_shared<std::atomic<uint64_t>>(0)};
    std::shared_ptr<State> state;
    Impl(RoutingServiceEndpoint target, const char* name, Handler operation,
         std::function<void(bool)> notice)
        : endpoint(std::move(target)), role(name), handler(std::move(operation)),
          availability(std::move(notice)),
          queue(dispatch_queue_create("com.lexasoft.loopz.routing.peer", DISPATCH_QUEUE_SERIAL)) {
        if (!queue || endpoint.serverRequirement.empty() || (role != "helper" && role != "driver"))
            throw std::invalid_argument("Invalid authenticated routing endpoint");
    }
    xpc_object_t exchange(xpc_object_t message, bool observe = true, bool observeErrors = false) {
        if (!peer || !state || state->snapshot().terminal)
            throw std::runtime_error("Routing connection unavailable");
        struct Reply {
            xpc_object_t value{nullptr};
            std::string failure;
        };
        auto reply = std::make_shared<Reply>();
        const auto current = state;
        auto done = dispatch_semaphore_create(0);
        const auto connection = peer;
        const auto serverUID = endpoint.serverUID;
        xpc_connection_send_message_with_reply(peer, message, queue, ^(xpc_object_t response) {
            try {
                if (xpc_get_type(response) != XPC_TYPE_DICTIONARY ||
                    xpc_connection_get_euid(connection) != serverUID)
                    throw std::runtime_error("Routing server identity unavailable");
                protocol(response);
                if (observe && (observeErrors || boolean(response, "ok")))
                    current->observe(response);
                reply->value = response;
            } catch (const std::exception& error) {
                reply->failure = error.what();
                current->close();
            }
            dispatch_semaphore_signal(done);
        });
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kReplyTimeout)))
            throw std::runtime_error("Routing request timed out; outcome is uncertain");
        if (!reply->value || state->snapshot().terminal)
            throw std::runtime_error(reply->failure.empty()
                                         ? "Routing server disconnected or authentication failed"
                                         : reply->failure);
        protocol(reply->value);
        if (!boolean(reply->value, "ok")) {
            const auto reason = text(reply->value, "error");
            throw std::runtime_error(std::string(reason));
        }
        return reply->value;
    }
};
Connection::Connection(RoutingServiceEndpoint endpoint, const char* role, Handler handler,
                       std::function<void(bool)> availability)
    : impl_(std::make_unique<Impl>(std::move(endpoint), role, std::move(handler),
                                   std::move(availability))) {}
Connection::~Connection() {
    disconnect();
}
void Connection::disconnect() noexcept {
    if (impl_->state)
        impl_->state->close();
    if (impl_->peer) {
        xpc_connection_cancel(impl_->peer);
        impl_->peer = nullptr;
        // Called by its control owner, never from this callback queue. Late
        // blocks see terminal state; currently running operation contexts drain.
        dispatch_sync(impl_->queue, ^{
                      });
    }
    if (impl_->ratesChanged)
        impl_->ratesChanged(kLegacyVirtualSampleRates);
    if (impl_->availability)
        impl_->availability(false);
    impl_->state.reset();
}
bool Connection::connect(std::string& error) {
    disconnect();
    auto state = std::make_shared<State>(impl_->sequence, impl_->handler, impl_->availability,
                                         impl_->ratesChanged);
    impl_->state = state;
    auto connection =
        impl_->endpoint.endpoint
            ? xpc_connection_create_from_endpoint(impl_->endpoint.endpoint)
            : xpc_connection_create_mach_service(
                  impl_->endpoint.serviceName.c_str(), impl_->queue,
                  impl_->endpoint.privileged ? XPC_CONNECTION_MACH_SERVICE_PRIVILEGED : 0);
    impl_->peer = connection;
    if (!connection) {
        error = "Cannot create routing connection";
        return false;
    }
    xpc_connection_set_target_queue(connection, impl_->queue);
    if (xpc_connection_set_peer_code_signing_requirement(
            connection, impl_->endpoint.serverRequirement.c_str())) {
        error = "Cannot pin routing server identity";
        disconnect();
        return false;
    }
    const auto weak = std::weak_ptr<State>(state);
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        const auto state = weak.lock();
        if (!state)
            return;
        if (xpc_get_type(event) == XPC_TYPE_ERROR) {
            state->close();
            return;
        }
        if (state->snapshot().terminal)
            return;
        try {
            protocol(event);
            const auto operation = text(event, "op");
            if (operation == "pair") {
                dictionary(event, xpc_dictionary_get_value(event, "sampleRateMask") ? 5 : 4);
                state->observe(event);
                return;
            }
            auto reply = xpc_dictionary_create_reply(event);
            if (!reply || !state->handler)
                throw std::invalid_argument("Unexpected routing server command");
            const auto before = state->snapshot();
            if (!before.token || integer(event, "epoch") != before.epoch)
                throw std::invalid_argument("Stale routing server command");
            auto result = state->handler(event);
            if (!result || xpc_get_type(result) != XPC_TYPE_DICTIONARY)
                throw std::runtime_error("Invalid driver handler result");
            const auto after = state->snapshot();
            if (after.token != before.token || after.terminal)
                throw std::runtime_error("Routing session changed during command");
            xpc_dictionary_set_uint64(reply, "protocol", kDeviceAudioProtocol);
            xpc_dictionary_set_uint64(reply, "epoch", after.epoch);
            xpc_dictionary_set_bool(reply, "ok", true);
            xpc_dictionary_set_int64(reply, "result", resultCode(result, "result"));
            if (const auto configuration = xpc_dictionary_get_value(result, "configuration"))
                xpc_dictionary_set_value(reply, "configuration", configuration);
            if (const auto rates = xpc_dictionary_get_value(result, "rates"))
                xpc_dictionary_set_value(reply, "rates", rates);
            xpc_connection_send_message(connection, reply);
        } catch (const std::exception&) {
            state->close();
            xpc_connection_cancel(connection);
        }
    });
    xpc_connection_activate(connection);
    try {
        bool rateCapabilities = false;
        for (const char* operation : {"hello", "attach"}) {
            auto message = routing::message(operation);
            xpc_dictionary_set_string(message, "role", impl_->role.c_str());
            if (std::string_view(operation) == "attach" && rateCapabilities)
                xpc_dictionary_set_uint64(message, "sampleRateMask", impl_->sampleRateMask);
            auto reply = impl_->exchange(message, std::string_view(operation) != "hello");
            if (std::string_view(operation) == "hello") {
                if (xpc_dictionary_get_value(reply, "rateCapabilitiesVersion")) {
                    if (integer(reply, "rateCapabilitiesVersion") != 1)
                        throw std::invalid_argument("Unsupported routing rate negotiation");
                    rateCapabilities = true;
                }
                state->negotiateRates(rateCapabilities);
            }
        }
        if (state->snapshot().terminal)
            throw std::runtime_error("Routing connection interrupted during attachment");
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        disconnect();
        return false;
    }
}
bool Connection::alive() const noexcept {
    return impl_->state && !impl_->state->snapshot().terminal;
}
uint64_t Connection::session() const noexcept {
    return alive() ? impl_->state->snapshot().token : 0;
}
uint32_t Connection::sampleRateMask() const noexcept {
    if (!impl_->state)
        return kLegacyVirtualSampleRates;
    const auto snapshot = impl_->state->snapshot();
    return !snapshot.terminal && snapshot.token ? snapshot.sampleRateMask
                                                : kLegacyVirtualSampleRates;
}
void Connection::configureSampleRates(uint32_t mask, std::function<void(uint32_t)> changed) {
    if (impl_->peer || !validVirtualSampleRateMask(mask))
        throw std::invalid_argument("Configure valid routing rates before connecting");
    impl_->sampleRateMask = mask;
    impl_->ratesChanged = std::move(changed);
}
xpc_object_t Connection::request(const char* operation, xpc_object_t payload, uint64_t revision) {
    if (!impl_->state)
        throw std::runtime_error("Routing transport unavailable");
    const auto before = impl_->state->snapshot();
    if (!before.token || before.terminal)
        throw std::runtime_error("Routing driver not paired");
    auto object = routing::message(operation);
    xpc_dictionary_set_string(object, "role", impl_->role.c_str());
    xpc_dictionary_set_uint64(object, "epoch", before.epoch);
    if (payload)
        xpc_dictionary_set_value(object, "payload", payload);
    else
        xpc_dictionary_set_uint64(object, "revision", revision);
    auto result = impl_->exchange(object, true, true);
    const auto after = impl_->state->snapshot();
    if (after.token != before.token || after.terminal)
        throw std::runtime_error("Routing pairing changed during request");
    return result;
}
} // namespace loopz::macos::routing
