#pragma once
#include <windows.h>
#include <string>

namespace loopz::windows {
// Control thread only, after reading a complete bounded request from a local
// message-mode server pipe created with PIPE_REJECT_REMOTE_CLIENTS. The last
// message's token supplies ownership; request fields never supply an owner.
// Returns with the server identity restored, including on exceptions. Nested
// impersonation is rejected rather than silently discarding an existing token.
std::wstring brokerClientOwner(HANDLE serverPipe);
} // namespace loopz::windows
