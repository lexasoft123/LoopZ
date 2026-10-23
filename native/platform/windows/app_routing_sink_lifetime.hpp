#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace loopz::windows {
struct AppRoutingLocalRenderClients {
    // Native control-owner inventory only. Never inferred from session state.
    bool examined{};
    std::vector<std::string> endpoints;
};
struct AppRoutingSinkObservation {
    std::string endpoint, error;
    bool endpointsExamined{}, defaultsExamined{}, occupied{}, defaultEndpoint{};
};
// Includes inactive/unmapped PnP roots. Missing endpoint metadata cannot prove
// first use. Any ambiguous/failed PnP observation throws.
bool appRoutingSinkRootPresent(std::wstring_view ownerSid);
// MTA control thread. Identifies this account's exact hardware-backed internal
// render endpoint, then examines sessions and all six default-device roles.
// Missing identity stays unknown. Incomplete reads preserve partial positive
// evidence with error text; ambiguous hardware identity throws.
AppRoutingSinkObservation
observeAppRoutingSink(std::wstring_view ownerSid,
                      const AppRoutingLocalRenderClients& localClients = {});
} // namespace loopz::windows
