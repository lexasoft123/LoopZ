#pragma once
#include <map>
#include <string>
#include <string_view>
namespace loopz::windows {
enum class EndpointRole { Other, Feed, Microphone };
struct DriverEndpoint {
    EndpointRole role;
    std::string mixKey;
    // Control-thread diagnostics; neither field authorizes routing by itself.
    bool topologyVerified{};
    bool parentVerified{};
};
std::map<std::string, DriverEndpoint> loopzEndpoints(bool includeInactive = false);
// Read-only snapshot of all six Windows default roles, for install/upgrade QA.
// Missing defaults are empty strings; API failures throw instead of claiming an
// unchanged default-device policy from an incomplete snapshot.
std::map<std::string, std::string> audioDefaultEndpoints();
// Exact ROOT\LoopZAudio hardware identity through PnP ancestry; no label guesses.
std::map<std::string, EndpointRole> loopzEndpointRoles();
EndpointRole endpointRole(const std::map<std::string, EndpointRole>& roles, std::string_view uid);
} // namespace loopz::windows
