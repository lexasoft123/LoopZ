#include <unistd.h>
// Compile the unchanged pre-negotiation implementation with a distinct class.
#define RoutingBroker PriorRoutingBroker
#include "prior_routing_wire/broker.mm"
#undef RoutingBroker
#include "routing_compatibility_fixtures.hpp"

namespace loopz::test {
PriorBroker priorBroker(macos::RoutingBrokerPolicy policy, const std::string& requirement) {
    std::shared_ptr<macos::PriorRoutingBroker> owner =
        macos::PriorRoutingBroker::anonymous(std::move(policy));
    macos::RoutingServiceEndpoint target;
    target.endpoint = owner->endpoint();
    target.serverRequirement = requirement;
    target.serverUID = geteuid();
    target.privileged = false;
    return {std::move(owner), std::move(target)};
}
} // namespace loopz::test
