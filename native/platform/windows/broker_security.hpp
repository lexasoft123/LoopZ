#pragma once
#include <windows.h>
#include <aclapi.h>
#include <cstddef>
#include <stdexcept>

namespace loopz::windows {
// Shared strict policy for machine-private state and service binaries. Callers
// open with OPEN_REPARSE_POINT and retain handles for any required lifetime.
[[noreturn]] inline void brokerProtectedObjectFailure(const char* operation) {
    throw std::runtime_error(std::string(operation) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
inline void validateBrokerProtectedObject(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info))
        brokerProtectedObjectFailure("Inspect setup database object");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1))
        throw std::runtime_error(
            "Setup database object must be an ordinary local file or directory");
    PSID owner{};
    PACL acl{};
    PSECURITY_DESCRIPTOR security{};
    const auto error = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                       &owner, nullptr, &acl, nullptr, &security);
    if (error != ERROR_SUCCESS) {
        SetLastError(error);
        brokerProtectedObjectFailure("Read setup database security");
    }
    bool secure = owner && (IsWellKnownSid(owner, WinLocalSystemSid) ||
                            IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    secure = secure && GetSecurityDescriptorControl(security, &control, &revision) &&
             (control & SE_DACL_PROTECTED) && acl && acl->AceCount == 2;
    bool system = false, administrators = false;
    if (secure)
        for (DWORD index = 0; index < acl->AceCount; ++index) {
            void* raw{};
            if (!GetAce(acl, index, &raw)) {
                secure = false;
                break;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            const auto minimum = offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8;
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceSize < minimum ||
                ace->Header.AceFlags !=
                    (directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0) ||
                ace->Mask != FILE_ALL_ACCESS) {
                secure = false;
                break;
            }
            auto sid = const_cast<DWORD*>(&ace->SidStart);
            if (!IsValidSid(sid) ||
                GetLengthSid(sid) > ace->Header.AceSize - offsetof(ACCESS_ALLOWED_ACE, SidStart)) {
                secure = false;
                break;
            }
            if (IsWellKnownSid(sid, WinLocalSystemSid) && !system)
                system = true;
            else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) && !administrators)
                administrators = true;
            else {
                secure = false;
                break;
            }
        }
    LocalFree(security);
    if (!secure || !system || !administrators)
        throw std::runtime_error(
            "Setup databases require protected SYSTEM/Administrators ownership and access");
}
} // namespace loopz::windows
