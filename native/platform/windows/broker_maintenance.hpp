#pragma once
#include <string_view>
namespace loopz::windows {
// Elevated maintenance only; serialized with driver setup. Uses the current
// executable's embedded payload and the fixed service/store/installation paths.
// publishedInf is the installed package receipt, never an IPC request field.
struct BrokerMaintenanceResult {
    unsigned migratedDevices{};
    bool rebootRequired{};
};
BrokerMaintenanceResult installOrRepairBrokerService(std::wstring_view publishedInf);
void stopInstalledBrokerService();
// Registration only. Retains binaries and durable intent so the encompassing
// driver uninstaller can finish device removal before purging owned files/data.
void unregisterBrokerService();
} // namespace loopz::windows
