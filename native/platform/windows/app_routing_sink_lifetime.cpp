#include "app_routing_sink_lifetime.hpp"
#include "app_routing_sessions.hpp"
#include "driver_identity.hpp"
#include "endpoint_roles.hpp"
#include <stdexcept>
#include <algorithm>
#include <windows.h>
#include <cfgmgr32.h>
namespace loopz::windows {
bool appRoutingSinkRootPresent(std::wstring_view ownerSid) {
    auto instance = driverInstance(driverAppRoutingSinkKey(ownerSid));
    DEVINST node{};
    const auto result = CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_PHANTOM);
    if (result == CR_NO_SUCH_DEVNODE)
        return false;
    if (result != CR_SUCCESS)
        throw std::runtime_error(
            "Cannot establish internal app endpoint absence; recovery history was not initialized");
    return true;
}
AppRoutingSinkObservation observeAppRoutingSink(std::wstring_view ownerSid,
                                                const AppRoutingLocalRenderClients& localClients) {
    AppRoutingSinkObservation result;
    const auto key = driverAppRoutingSinkKey(ownerSid);
    for (const auto& [uid, endpoint] : loopzEndpoints(true)) {
        if (endpoint.mixKey != key)
            continue;
        if (endpoint.role != EndpointRole::Feed || !result.endpoint.empty() ||
            (!endpoint.parentVerified && !endpoint.topologyVerified))
            throw std::runtime_error("Ambiguous application routing sink hardware identity");
        result.endpoint = uid;
    }
    if (result.endpoint.empty())
        return result; // Absence of a mapped endpoint cannot authorize root removal.
    try {
        const bool localRenderClientsAbsent =
            localClients.examined &&
            std::find(localClients.endpoints.begin(), localClients.endpoints.end(),
                      result.endpoint) == localClients.endpoints.end();
        for (const auto& session : appRoutingSessions())
            if (session.endpoint == result.endpoint &&
                appRoutingSessionMayOwnStream(session, localRenderClientsAbsent))
                result.occupied = true;
        result.endpointsExamined = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    try {
        for (const auto& [role, uid] : audioDefaultEndpoints()) {
            (void)role;
            result.defaultEndpoint = result.defaultEndpoint || uid == result.endpoint;
        }
        result.defaultsExamined = true;
    } catch (const std::exception& error) {
        if (!result.error.empty())
            result.error += "; ";
        result.error += error.what();
    }
    return result;
}
} // namespace loopz::windows
