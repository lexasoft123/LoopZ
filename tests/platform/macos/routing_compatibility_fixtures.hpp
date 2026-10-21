#pragma once

#include <loopz/macos/routing_service.hpp>

namespace loopz::test {
struct PriorBroker {
    std::shared_ptr<void> owner;
    macos::RoutingServiceEndpoint target;
};
PriorBroker priorBroker(macos::RoutingBrokerPolicy policy, const std::string& requirement);

class PriorPeer {
  public:
    virtual ~PriorPeer() = default;
    virtual bool connect(std::string& error) = 0;
    virtual bool alive() const = 0;
    virtual uint64_t session() const = 0;
    virtual xpc_object_t request(const char* operation, xpc_object_t payload,
                                 uint64_t revision) = 0;
};
std::unique_ptr<PriorPeer> priorPeer(macos::RoutingServiceEndpoint endpoint, const char* role,
                                     std::function<xpc_object_t(xpc_object_t)> handler = {});
} // namespace loopz::test
