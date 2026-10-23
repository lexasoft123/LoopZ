#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace loopz::windows {
inline constexpr wchar_t kDeviceBrokerServiceName[] = L"LoopZDeviceBroker";
// Windows service installation metadata, separate from portable per-owner intent.
// The normal service reads its fixed SCM Parameters key; IPC never supplies a
// registry path, handle or driver package. Provisioning is installer-only.
std::wstring readBrokerInstallation();
void validateBrokerPublishedInf(std::wstring_view publishedInf);
void provisionBrokerInstallation(std::wstring_view publishedInf);
// Installer-only: grant authenticated clients query access, retaining all
// service control/configuration rights exclusively for System/Administrators.
void provisionBrokerServiceAccess(SC_HANDLE service);
// Shared validation on an already-open key. Explicit handles exist for isolated
// administrative fixtures; they are not part of the broker wire protocol.
std::wstring readBrokerInstallationKey(HKEY key);
void writeBrokerInstallationKey(HKEY key, std::wstring_view publishedInf);
} // namespace loopz::windows
