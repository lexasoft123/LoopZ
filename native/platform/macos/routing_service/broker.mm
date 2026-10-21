#include "wire.hpp"
#include <loopz/macos/transport_message.hpp>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace loopz::macos {
namespace {
using namespace routing;
constexpr size_t kMaximumPeers = 16, kMaximumRequests = 16, kMaximumLeases = 128;
constexpr int64_t kRelayTimeout = 2 * NSEC_PER_SEC;
struct Peer {
    xpc_connection_t connection;
    uint32_t uid;
    std::string role;
    bool attached{false};
    bool rateCapabilities{false};
    uint32_t sampleRateMask{kLegacyVirtualSampleRates};
};
struct Relay {
    xpc_object_t reply;
    uint64_t helper, driver, epoch;
    bool replied{false};
};
struct Lease {
    RemoteDeviceTransport mapping;
    DeviceAudioView view;
    std::array<uint64_t, 2> identity;
};
struct BrokerState {
    const RoutingBrokerPolicy policy;
    const dispatch_queue_t queue;
    bool stopped{false};
    uint64_t nextPeer{0}, nextRequest{0}, epoch{1}, helper{0}, driver{0};
    std::optional<uint32_t> owner;
    std::map<uint64_t, Peer> peers;
    std::map<uint64_t, Relay> requests;
    std::vector<Lease> leases;
    BrokerState(RoutingBrokerPolicy configuration, dispatch_queue_t target)
        : policy(std::move(configuration)), queue(target) {
        leases.reserve(kMaximumLeases);
    }
    bool paired() const {
        return helper && driver && !stopped;
    }
    uint32_t sampleRateMask() const {
        return paired() ? peers.at(helper).sampleRateMask & peers.at(driver).sampleRateMask &
                              kAllVirtualSampleRates
                        : kLegacyVirtualSampleRates;
    }
    xpc_object_t response(xpc_object_t request, const char* error = nullptr) {
        auto reply = xpc_dictionary_create_reply(request);
        if (!reply)
            return nullptr;
        decorate(reply, error);
        return reply;
    }
    void decorate(xpc_object_t reply, const char* error = nullptr) {
        xpc_dictionary_set_uint64(reply, "protocol", kDeviceAudioProtocol);
        xpc_dictionary_set_uint64(reply, "epoch", epoch);
        xpc_dictionary_set_bool(reply, "paired", paired());
        xpc_dictionary_set_bool(reply, "ok", !error);
        xpc_dictionary_set_uint64(reply, "sampleRateMask", sampleRateMask());
        if (error)
            xpc_dictionary_set_string(reply, "error", error);
    }
    void send(uint64_t peer, xpc_object_t reply) {
        const auto found = peers.find(peer);
        if (reply && found != peers.end())
            xpc_connection_send_message(found->second.connection, reply);
    }
    void changed() {
        if (epoch == UINT64_MAX) {
            stop();
            return;
        }
        ++epoch;
        for (const auto id : {helper, driver})
            if (id) {
                // Older peers require exactly four fields in pair notices.
                auto notice = message("pair");
                xpc_dictionary_set_uint64(notice, "epoch", epoch);
                xpc_dictionary_set_bool(notice, "paired", paired());
                if (peers.at(id).rateCapabilities)
                    xpc_dictionary_set_uint64(notice, "sampleRateMask", sampleRateMask());
                send(id, notice);
            }
        // No response from an older pairing may authorize the new one.
        for (auto& [_, relay] : requests) {
            if (!relay.replied) {
                decorate(relay.reply, "Routing pairing changed; request outcome is uncertain");
                send(relay.helper, relay.reply);
                relay.replied = true;
                relay.reply = nullptr;
            }
        }
        // A helper replacement cannot reset the bound on messages still queued
        // in a live driver. A dead driver connection releases its own slots.
        std::erase_if(requests, [&](const auto& item) {
            return !peers.contains(item.second.driver);
        });
    }
    void retireLeases() {
        for (const auto& device : leases) {
            auto& header = device.view.header();
            header.retired.store(1);
            header.helperOnline.store(0);
            header.driverOnline.store(0);
        }
        leases.clear();
    }
    void remove(uint64_t id) {
        const auto found = peers.find(id);
        if (found == peers.end())
            return;
        const bool relevant = helper == id || driver == id;
        if (helper == id) {
            helper = 0;
            owner.reset();
            retireLeases();
        }
        if (driver == id)
            driver = 0;
        xpc_connection_cancel(found->second.connection);
        peers.erase(found);
        if (relevant)
            changed();
    }
    void checkConsole() {
        if (owner && policy.consoleUser() != owner)
            remove(helper);
        std::erase_if(leases, [](const auto& device) {
            return device.view.header().retired.load() != 0;
        });
    }
    void stop() {
        if (stopped)
            return;
        stopped = true;
        retireLeases();
        for (auto& [_, peer] : peers)
            xpc_connection_cancel(peer.connection);
        peers.clear();
        requests.clear();
        helper = driver = 0;
        owner.reset();
    }
    void lease(xpc_object_t payload) {
        auto snapshot = DeviceTransportSnapshot::decode(payload);
        std::vector<Lease> staged;
        staged.reserve(snapshot.devices.size());
        for (auto& device : snapshot.devices) {
            const auto view = device.view();
            if (!(sampleRateMask() & virtualSampleRateBit(view.sampleRate())))
                throw std::invalid_argument("Device rate is unsupported by the routing peers");
            const auto identity = view.mappingIdentity();
            if (view.header().retired.load())
                throw std::invalid_argument("Retired device mapping cannot be submitted");
            const auto existing = std::find_if(leases.begin(), leases.end(), [&](const auto& item) {
                return item.identity == identity;
            });
            if (existing != leases.end() && existing->mapping.id() != device.id())
                throw std::invalid_argument("Device mapping identity changed owners");
            if (existing == leases.end())
                staged.push_back({std::move(device), view, identity});
        }
        if (leases.size() + staged.size() > kMaximumLeases)
            throw std::runtime_error("Routing mapping lease limit reached");
        // All validation/allocation has completed; pre-reserved moves cannot
        // partially forget the mappings a disconnected helper might still use.
        for (auto& device : staged)
            leases.push_back(std::move(device));
    }
};
void handle(const std::shared_ptr<BrokerState>& state, uint64_t id, xpc_object_t event) {
    const auto item = state->peers.find(id);
    if (item == state->peers.end() || state->stopped)
        return;
    if (xpc_get_type(event) == XPC_TYPE_ERROR) {
        state->remove(id);
        return;
    }
    if (xpc_get_type(event) != XPC_TYPE_DICTIONARY) {
        state->remove(id);
        return;
    }
    auto reply = state->response(event);
    if (!reply)
        return;
    try {
        protocol(event);
        state->checkConsole();
        const auto current = state->peers.find(id);
        if (current == state->peers.end())
            return;
        auto& peer = current->second;
        peer.uid = xpc_connection_get_euid(peer.connection);
        const auto role = text(event, "role");
        const auto operation = text(event, "op");
        const auto console = state->policy.consoleUser();
        if ((role == "helper" && (!console || *console != peer.uid)) ||
            (role == "driver" && peer.uid != state->policy.driverUID &&
             !(state->policy.allowRootDriver && peer.uid == 0)) ||
            (role != "helper" && role != "driver"))
            throw std::invalid_argument("Routing role or active console user rejected");
        if (operation == "hello") {
            dictionary(event, 3);
            if (!peer.role.empty())
                throw std::invalid_argument("Routing role is already bound");
            const auto& requirement = role == "helper" ? state->policy.helperRequirement
                                                       : state->policy.driverRequirement;
            if (xpc_connection_set_peer_code_signing_requirement(peer.connection,
                                                                 requirement.c_str()))
                throw std::runtime_error("Cannot bind routing role code identity");
            peer.role = role;
            state->decorate(reply);
            xpc_dictionary_set_uint64(reply, "rateCapabilitiesVersion", 1);
            state->send(id, reply);
            return;
        }
        if (peer.role != role)
            throw std::invalid_argument("Complete routing role negotiation first");
        if (operation == "attach") {
            const auto rateCapabilities = xpc_dictionary_get_value(event, "sampleRateMask");
            dictionary(event, rateCapabilities ? 4 : 3);
            const auto rates =
                rateCapabilities ? routing::sampleRateMask(event) : kLegacyVirtualSampleRates;
            auto& slot = role == "helper" ? state->helper : state->driver;
            if (peer.attached || (slot && slot != id))
                throw std::runtime_error("Routing role already has an owner");
            peer.attached = true;
            peer.rateCapabilities = rateCapabilities != nullptr;
            peer.sampleRateMask = rates;
            slot = id;
            if (role == "helper")
                state->owner = peer.uid;
            state->changed();
            state->decorate(reply);
            state->send(id, reply);
            return;
        }
        dictionary(event, 5);
        if (!peer.attached || role != "helper" || id != state->helper || !state->paired() ||
            integer(event, "epoch") != state->epoch)
            throw std::runtime_error("Routing pairing unavailable or stale");
        if (operation != "apply" && operation != "status" && operation != "cancel" &&
            operation != "rates" && operation != "ack_rates")
            throw std::invalid_argument("Unsupported routing operation");
        if (state->requests.size() >= kMaximumRequests || state->nextRequest == UINT64_MAX)
            throw std::runtime_error("Routing request limit reached");
        auto command = message(std::string(operation).c_str());
        xpc_dictionary_set_uint64(command, "epoch", state->epoch);
        if (operation == "apply") {
            const auto payload = xpc_dictionary_get_value(event, "payload");
            state->lease(payload);
            xpc_dictionary_set_value(command, "payload", payload);
        } else if (operation == "ack_rates") {
            const auto payload = xpc_dictionary_get_value(event, "payload");
            (void)rateRequests(payload);
            xpc_dictionary_set_value(command, "payload", payload);
        } else {
            const auto revision = integer(event, "revision");
            if (operation == "cancel" && !revision)
                throw std::invalid_argument("Cancellation requires a nonzero revision");
            if (operation == "rates" && revision)
                throw std::invalid_argument("Rate query revision must be zero");
            xpc_dictionary_set_uint64(command, "revision", revision);
        }
        const auto request = ++state->nextRequest;
        state->requests.emplace(request, Relay{reply, state->helper, state->driver, state->epoch});
        const auto weak = std::weak_ptr<BrokerState>(state);
        const auto isStatus = operation == "status";
        const auto isRates = operation == "rates";
        const auto relayQueue = state->queue;
        xpc_connection_send_message_with_reply(
            state->peers.at(state->driver).connection, command, state->queue,
            ^(xpc_object_t response) {
                dispatch_async(relayQueue, ^{
                    const auto state = weak.lock();
                    if (!state || state->stopped)
                        return;
                    const auto found = state->requests.find(request);
                    if (found == state->requests.end())
                        return;
                    auto relay = found->second;
                    state->requests.erase(found);
                    if (relay.replied)
                        return;
                    try {
                        protocol(response);
                        dictionary(response, isStatus || isRates ? 5 : 4);
                        if (!boolean(response, "ok") || integer(response, "epoch") != relay.epoch ||
                            state->epoch != relay.epoch || state->driver != relay.driver ||
                            state->helper != relay.helper)
                            throw std::runtime_error("Stale driver response");
                        const auto status = resultCode(response, "result");
                        xpc_dictionary_set_int64(relay.reply, "result", status);
                        if (isStatus) {
                            const auto value = xpc_dictionary_get_value(response, "configuration");
                            (void)configuration(value);
                            xpc_dictionary_set_value(relay.reply, "configuration", value);
                        } else if (isRates) {
                            const auto value = xpc_dictionary_get_value(response, "rates");
                            (void)rateRequests(value);
                            xpc_dictionary_set_value(relay.reply, "rates", value);
                        }
                        state->decorate(relay.reply);
                    } catch (const std::exception&) {
                        state->decorate(relay.reply,
                                        "Driver response unavailable; outcome is uncertain");
                    }
                    state->send(relay.helper, relay.reply);
                });
            });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRelayTimeout), state->queue, ^{
            const auto state = weak.lock();
            if (!state || state->stopped)
                return;
            const auto found = state->requests.find(request);
            if (found == state->requests.end())
                return;
            auto& relay = found->second;
            if (relay.replied)
                return;
            state->decorate(relay.reply, "Driver request timed out; outcome is uncertain");
            state->send(relay.helper, relay.reply);
            relay.replied = true;
            relay.reply = nullptr;
        });
    } catch (const std::exception& error) {
        state->decorate(reply, error.what());
        state->send(id, reply);
    }
}
} // namespace
struct RoutingBroker::Impl {
    const dispatch_queue_t queue;
    xpc_connection_t listener{nullptr};
    dispatch_source_t timer{nullptr};
    std::shared_ptr<BrokerState> state;
    Impl(RoutingBrokerPolicy policy, bool anonymous, const std::string& name)
        : queue(dispatch_queue_create("com.lexasoft.loopz.routing.broker", DISPATCH_QUEUE_SERIAL)) {
        if (!queue || policy.helperRequirement.empty() || policy.driverRequirement.empty() ||
            !policy.consoleUser)
            throw std::invalid_argument("Invalid routing broker authentication policy");
        state = std::make_shared<BrokerState>(std::move(policy), queue);
        listener = anonymous ? xpc_connection_create(nullptr, queue)
                             : xpc_connection_create_mach_service(
                                   name.c_str(), queue, XPC_CONNECTION_MACH_SERVICE_LISTENER);
        const auto requirement = "(" + state->policy.helperRequirement + ") or (" +
                                 state->policy.driverRequirement + ")";
        if (!listener ||
            xpc_connection_set_peer_code_signing_requirement(listener, requirement.c_str()))
            throw std::runtime_error("Cannot create authenticated routing broker listener");
        const auto weak = std::weak_ptr<BrokerState>(state);
        const auto serial = queue;
        xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
            if (xpc_get_type(event) != XPC_TYPE_CONNECTION)
                return;
            dispatch_async(serial, ^{
                const auto state = weak.lock();
                if (!state || state->stopped)
                    return;
                auto peer = (xpc_connection_t)event;
                if (state->peers.size() >= kMaximumPeers || state->nextPeer == UINT64_MAX) {
                    xpc_connection_cancel(peer);
                    return;
                }
                const auto id = ++state->nextPeer;
                state->peers.emplace(id, Peer{peer, xpc_connection_get_euid(peer), "", false});
                xpc_connection_set_target_queue(peer, serial);
                xpc_connection_set_event_handler(peer, ^(xpc_object_t incoming) {
                    dispatch_async(serial, ^{
                        if (const auto state = weak.lock())
                            handle(state, id, incoming);
                    });
                });
                xpc_connection_activate(peer);
            });
        });
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        if (!timer)
            throw std::bad_alloc();
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                                  100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{
            dispatch_async(serial, ^{
                if (const auto state = weak.lock()) {
                    try {
                        state->checkConsole();
                    } catch (const std::exception&) {
                        state->stop();
                    }
                }
            });
        });
        dispatch_resume(timer);
        xpc_connection_activate(listener);
    }
    ~Impl() {
        stop();
    }
    void stop() {
        const auto current = state;
        if (current)
            dispatch_sync(queue, ^{
                current->stop();
            });
        if (timer) {
            dispatch_source_cancel(timer);
            timer = nullptr;
        }
        if (listener) {
            xpc_connection_cancel(listener);
            listener = nullptr;
        }
        state.reset();
    }
};
RoutingBroker::RoutingBroker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::unique_ptr<RoutingBroker> RoutingBroker::anonymous(RoutingBrokerPolicy policy) {
    return std::unique_ptr<RoutingBroker>(
        new RoutingBroker(std::make_unique<Impl>(std::move(policy), true, kRoutingServiceName)));
}
std::unique_ptr<RoutingBroker> RoutingBroker::machService(RoutingBrokerPolicy policy,
                                                          std::string serviceName) {
    return std::unique_ptr<RoutingBroker>(
        new RoutingBroker(std::make_unique<Impl>(std::move(policy), false, serviceName)));
}
RoutingBroker::~RoutingBroker() = default;
xpc_endpoint_t RoutingBroker::endpoint() const {
    if (!impl_->listener)
        throw std::logic_error("Routing broker is stopped");
    return xpc_endpoint_create(impl_->listener);
}
void RoutingBroker::stop() {
    impl_->stop();
}
} // namespace loopz::macos
