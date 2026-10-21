#pragma once

#include <chrono>

namespace loopz::macos {

enum class MicrophonePermission { NotDetermined, Authorized, Denied, Restricted, Unknown };
MicrophonePermission microphonePermission() noexcept;
const char* permissionName(MicrophonePermission permission) noexcept;

struct PermissionRequest {
    MicrophonePermission status;
    bool requested;
    bool completed;
};

// Main/control thread only. Does not open any audio device. First-time setup
// activates the helper and pumps its main loop while macOS owns the consent UI.
PermissionRequest requestMicrophonePermission(std::chrono::seconds timeout);

} // namespace loopz::macos
