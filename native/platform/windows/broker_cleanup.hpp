#pragma once
#include <span>
#include <string>
namespace loopz::windows {
class DeviceSetupLock;
// Elevated uninstall only. Service must be unregistered and every device named
// by durable desired state must already be absent. Fixed paths only; unknown or
// unsafe files abort preflight before deletion. Does not remove driver devices.
void purgeBrokerInstallation();
// Full elevated uninstall only. The caller still holds the setup lock and has
// successfully requested DIF_REMOVE for every listed, verified LoopZ instance.
// Windows may retain those nodes until reboot; that does not require retaining
// the already stopped/unregistered user-mode broker or its desired-state store.
// The list is an in-process maintenance result, never a command-line/IPC field.
void purgeBrokerInstallationAfterDeviceRemoval(const DeviceSetupLock& heldSetupLock,
                                               std::span<const std::wstring> removedInstances);
} // namespace loopz::windows
