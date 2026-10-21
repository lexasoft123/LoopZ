#include "connection.hpp"
#include <loopz/macos/transport_message.hpp>
#include <dispatch/dispatch.h>

#include <chrono>
#include <algorithm>
#include <utility>

namespace loopz::macos {
namespace {
using namespace routing;
class ReconnectingPeer {
  public:
    Connection connection;
    std::string error;
    ReconnectingPeer(RoutingServiceEndpoint endpoint, const char* role,
                     Connection::Handler handler = {}, std::function<void(bool)> availability = {})
        : connection(std::move(endpoint), role, std::move(handler), std::move(availability)) {
        retries_.setDesired({{"connection", {}}}, 0);
    }
    void poll(uint64_t now) {
        if (connection.alive())
            return;
        if (connected_) {
            retries_.disconnected(now);
            connected_ = false;
        }
        for (const auto& attempt : retries_.attempts(now)) {
            connected_ = connection.connect(error);
            retries_.complete(attempt, connected_, now);
            if (connected_)
                error.clear();
        }
    }

  private:
    GraphRecovery retries_;
    bool connected_{false};
};
xpc_object_t driverHandler(const RoutingDriverOperations& operations, xpc_object_t message) {
    dictionary(message, 4);
    const auto operation = text(message, "op");
    auto response = xpc_dictionary_create(nullptr, nullptr, 0);
    int32_t result = 0;
    if (operation == "apply") {
        const auto payload = xpc_dictionary_get_value(message, "payload");
        if (!payload || xpc_get_type(payload) != XPC_TYPE_DICTIONARY)
            throw std::invalid_argument("Missing native device snapshot");
        result = operations.apply(payload);
    } else if (operation == "status") {
        const auto status = operations.status(integer(message, "revision"));
        xpc_dictionary_set_value(response, "configuration", encode(status));
    } else if (operation == "cancel") {
        const auto revision = integer(message, "revision");
        if (!revision)
            throw std::invalid_argument("Missing cancellation revision");
        result = operations.cancel(revision);
    } else if (operation == "rates") {
        if (integer(message, "revision"))
            throw std::invalid_argument("Rate query revision must be zero");
        const auto requests =
            operations.rates ? operations.rates() : std::vector<DriverRateRequest>{};
        xpc_dictionary_set_value(response, "rates", encodeRateRequests(requests));
    } else if (operation == "ack_rates") {
        const auto requests = rateRequests(xpc_dictionary_get_value(message, "payload"));
        result = operations.acknowledgeRates ? operations.acknowledgeRates(requests)
                                             : (requests.empty() ? 0 : -1);
    } else
        throw std::invalid_argument("Unknown native driver operation");
    xpc_dictionary_set_int64(response, "result", result);
    return response;
}
} // namespace
struct RoutingDriverLink::Impl {
    ReconnectingPeer peer;
    explicit Impl(RoutingServiceEndpoint endpoint) : peer(std::move(endpoint), "helper") {}
};
RoutingDriverLink::RoutingDriverLink(RoutingServiceEndpoint endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
RoutingDriverLink::~RoutingDriverLink() = default;
void RoutingDriverLink::poll(uint64_t now) {
    impl_->peer.poll(now);
}
uint64_t RoutingDriverLink::session() const noexcept {
    return impl_->peer.connection.session();
}
uint32_t RoutingDriverLink::sampleRateMask() const noexcept {
    return impl_->peer.connection.sampleRateMask();
}
int32_t RoutingDriverLink::submit(std::span<const DeviceTransport> devices, uint64_t revision) {
    const auto mask = sampleRateMask();
    if (std::any_of(devices.begin(), devices.end(), [mask](const auto& device) {
            return !device.region ||
                   !(mask & virtualSampleRateBit(device.region->view().sampleRate()));
        }))
        return -1;
    return routing::resultCode(
        impl_->peer.connection.request("apply", encodeDeviceTransports(devices, revision), 0),
        "result");
}
DriverConfigurationResult RoutingDriverLink::status(uint64_t revision) {
    const auto response = impl_->peer.connection.request("status", nullptr, revision);
    if (const auto error = routing::resultCode(response, "result"))
        throw std::runtime_error("Native driver status failed: " + std::to_string(error));
    return routing::configuration(xpc_dictionary_get_value(response, "configuration"));
}
int32_t RoutingDriverLink::cancel(uint64_t revision) {
    return routing::resultCode(impl_->peer.connection.request("cancel", nullptr, revision),
                               "result");
}
std::vector<DriverRateRequest> RoutingDriverLink::rateRequests() {
    const auto response = impl_->peer.connection.request("rates", nullptr, 0);
    if (routing::resultCode(response, "result"))
        throw std::runtime_error("Native device rate query failed");
    return routing::rateRequests(xpc_dictionary_get_value(response, "rates"));
}
void RoutingDriverLink::acknowledgeRates(std::span<const DriverRateRequest> requests) {
    const auto response =
        impl_->peer.connection.request("ack_rates", routing::encodeRateRequests(requests), 0);
    if (routing::resultCode(response, "result"))
        throw std::runtime_error("Native device rate acknowledgement failed");
}
void RoutingDriverLink::invalidate() noexcept {
    impl_->peer.connection.disconnect();
}
std::string RoutingDriverLink::lastError() const {
    return impl_->peer.error;
}

struct RoutingDriverService::Impl {
    const dispatch_queue_t queue;
    dispatch_source_t timer{nullptr};
    struct Owner {
        std::unique_ptr<ReconnectingPeer> peer;
    };
    std::shared_ptr<Owner> owner;
    Impl(RoutingServiceEndpoint endpoint, RoutingDriverOperations operations)
        : queue(dispatch_queue_create("com.lexasoft.loopz.routing.driver-control",
                                      DISPATCH_QUEUE_SERIAL)) {
        if (!queue || !operations.apply || !operations.status || !operations.cancel ||
            !operations.availability)
            throw std::invalid_argument("Missing native routing driver operations");
        owner = std::make_shared<Owner>();
        owner->peer = std::make_unique<ReconnectingPeer>(
            std::move(endpoint), "driver",
            [operations](xpc_object_t command) {
                return driverHandler(operations, command);
            },
            operations.availability);
        owner->peer->connection.configureSampleRates(operations.sampleRateMask,
                                                     operations.sampleRatesChanged);
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        if (!timer)
            throw std::bad_alloc();
        const auto weak = std::weak_ptr<Owner>(owner);
        dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC,
                                  20 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{
            const auto owner = weak.lock();
            if (!owner || !owner->peer)
                return;
            try {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
                owner->peer->poll(static_cast<uint64_t>(now));
            } catch (const std::exception& error) {
                owner->peer->error = error.what();
            }
        });
        dispatch_resume(timer);
    }
    ~Impl() {
        stop();
    }
    void stop() {
        if (timer) {
            dispatch_source_cancel(timer);
            timer = nullptr;
        }
        const auto current = owner;
        dispatch_sync(queue, ^{
            if (current)
                current->peer.reset();
        });
        owner.reset();
    }
};
RoutingDriverService::RoutingDriverService(RoutingServiceEndpoint endpoint,
                                           RoutingDriverOperations operations)
    : impl_(std::make_unique<Impl>(std::move(endpoint), std::move(operations))) {}
RoutingDriverService::~RoutingDriverService() = default;
void RoutingDriverService::stop() {
    impl_->stop();
}
} // namespace loopz::macos
