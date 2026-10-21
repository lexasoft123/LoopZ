#pragma once

#include "session_host.hpp"
#include <loopz/macos/permissions.hpp>
#include <functional>

namespace loopz::macos {
// A blocking, bounded control operation. The desktop schedules it separately
// from normal editor traffic and invokes it only for an explicit user request.
using MicrophoneSetup = std::function<PermissionRequest(const std::atomic<bool>& running)>;
PermissionRequest decodeMicrophoneSetup(const SessionCommandResult& response);
MicrophoneSetup productionMicrophoneSetup();
} // namespace loopz::macos
