#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace loopz::windows {
struct AppRoutingSession {
    uint32_t pid{}; // Zero for shared/system sessions; observation only.
    std::string endpoint;
    bool active{};
    uint32_t creatorPid{}; // Observational only; shared sessions have no exclusive PID.
    bool shared{}, systemSounds{};
};
// MTA control thread only. Throws on an incomplete active-endpoint/session
// inventory; callers must retain recovery receipts when observation fails.
std::vector<AppRoutingSession> appRoutingSessions();
// Inactive system sounds and proven-dead inactive processes cannot resume a
// stream. Unknown live/shared ownership remains conservative. Control thread.
// A caller may exclude its own exclusive inactive session only after a complete
// native inventory proves it has no render client for that endpoint. Active,
// foreign and shared sessions remain independently authoritative.
bool appRoutingSessionMayOwnStream(const AppRoutingSession&, bool localRenderClientsAbsent = false);
} // namespace loopz::windows
