// Compile the unchanged pre-negotiation implementation with a distinct class.
#define Connection PriorConnection
#include "prior_routing_wire/connection.mm"
#undef Connection
#include "routing_compatibility_fixtures.hpp"

namespace loopz::test {
namespace {
class Peer final : public PriorPeer {
  public:
    Peer(macos::RoutingServiceEndpoint endpoint, const char* role,
         std::function<xpc_object_t(xpc_object_t)> handler)
        : connection_(std::move(endpoint), role, std::move(handler)) {}
    bool connect(std::string& error) override {
        return connection_.connect(error);
    }
    bool alive() const override {
        return connection_.alive();
    }
    uint64_t session() const override {
        return connection_.session();
    }
    xpc_object_t request(const char* operation, xpc_object_t payload, uint64_t revision) override {
        return connection_.request(operation, payload, revision);
    }

  private:
    macos::routing::PriorConnection connection_;
};
} // namespace
std::unique_ptr<PriorPeer> priorPeer(macos::RoutingServiceEndpoint endpoint, const char* role,
                                     std::function<xpc_object_t(xpc_object_t)> handler) {
    return std::make_unique<Peer>(std::move(endpoint), role, std::move(handler));
}
} // namespace loopz::test
