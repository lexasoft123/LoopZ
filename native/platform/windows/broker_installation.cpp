#include "broker_installation.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace loopz::windows {
namespace {
constexpr wchar_t serviceKey[] = L"SYSTEM\\CurrentControlSet\\Services\\LoopZDeviceBroker";
constexpr wchar_t parametersKey[] =
    L"SYSTEM\\CurrentControlSet\\Services\\LoopZDeviceBroker\\Parameters";
struct Key {
    HKEY value{};
    ~Key() {
        if (value)
            RegCloseKey(value);
    }
};
void check(LSTATUS result, const char* operation) {
    if (result != ERROR_SUCCESS)
        throw std::runtime_error(std::string(operation) + " (Windows error " +
                                 std::to_string(result) + ")");
}
void packageName(std::wstring_view name) {
    if (name.size() < 8 || name.size() > 32 || !name.starts_with(L"oem") ||
        !name.ends_with(L".inf") ||
        name.substr(3, name.size() - 7).find_first_not_of(L"0123456789") != std::wstring_view::npos)
        throw std::invalid_argument("Invalid installed microphone package name");
}
void secure(HKEY key) {
    PSID owner{};
    PACL acl{};
    PSECURITY_DESCRIPTOR descriptor{};
    check(GetSecurityInfo(key, SE_REGISTRY_KEY,
                          OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                          &acl, nullptr, &descriptor),
          "Read setup service configuration security");
    struct Free {
        PSECURITY_DESCRIPTOR value;
        ~Free() {
            LocalFree(value);
        }
    } free{descriptor};
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    bool valid = owner &&
                 (IsWellKnownSid(owner, WinLocalSystemSid) ||
                  IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) &&
                 GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                 (control & SE_DACL_PROTECTED) && acl && acl->AceCount == 2;
    bool system = false, admin = false;
    if (valid)
        for (DWORD index = 0; index < acl->AceCount; ++index) {
            void* raw{};
            if (!GetAce(acl, index, &raw)) {
                valid = false;
                break;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
                ace->Header.AceSize < offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8 ||
                ace->Mask != KEY_ALL_ACCESS) {
                valid = false;
                break;
            }
            auto sid = const_cast<DWORD*>(&ace->SidStart);
            if (!IsValidSid(sid) ||
                GetLengthSid(sid) > ace->Header.AceSize - offsetof(ACCESS_ALLOWED_ACE, SidStart)) {
                valid = false;
                break;
            }
            if (IsWellKnownSid(sid, WinLocalSystemSid) && !system)
                system = true;
            else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) && !admin)
                admin = true;
            else {
                valid = false;
                break;
            }
        }
    if (!valid || !system || !admin)
        throw std::runtime_error(
            "Setup service configuration requires protected SYSTEM/Administrators access");
}
} // namespace
std::wstring readBrokerInstallationKey(HKEY key) {
    secure(key);
    std::array<wchar_t, 34> value{};
    DWORD type{}, bytes = static_cast<DWORD>(sizeof(value));
    check(RegQueryValueExW(key, L"DriverInf", nullptr, &type, reinterpret_cast<BYTE*>(value.data()),
                           &bytes),
          "Read installed microphone package");
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > sizeof(value) ||
        bytes % sizeof(wchar_t) || value[bytes / sizeof(wchar_t) - 1] != 0)
        throw std::runtime_error("Invalid setup service package value");
    std::wstring result(value.data(), bytes / sizeof(wchar_t) - 1);
    packageName(result);
    return result;
}
void writeBrokerInstallationKey(HKEY key, std::wstring_view publishedInf) {
    packageName(publishedInf);
    secure(key); // Never repair or write through an insecure pre-existing key.
    const std::wstring value(publishedInf);
    check(RegSetValueExW(key, L"DriverInf", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))),
          "Record installed microphone package");
    check(RegFlushKey(key), "Flush setup service installation configuration");
}
void validateBrokerPublishedInf(std::wstring_view publishedInf) {
    packageName(publishedInf);
}
void provisionBrokerServiceAccess(SC_HANDLE service) {
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCRC;;;AU)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot prepare setup service client access");
    struct Free {
        PSECURITY_DESCRIPTOR value;
        ~Free() {
            LocalFree(value);
        }
    } free{descriptor};
    if (!SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, descriptor))
        throw std::runtime_error("Cannot provision setup service client access");
}
std::wstring readBrokerInstallation() {
    Key key;
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, parametersKey, 0,
                        KEY_QUERY_VALUE | READ_CONTROL | KEY_WOW64_64KEY, &key.value),
          "Open setup service configuration");
    return readBrokerInstallationKey(key.value);
}
void provisionBrokerInstallation(std::wstring_view publishedInf) {
    packageName(publishedInf);
    Key service;
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, serviceKey, 0, KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY,
                        &service.value),
          "Open installed setup service");
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot prepare setup service configuration security");
    struct Free {
        PSECURITY_DESCRIPTOR value;
        ~Free() {
            LocalFree(value);
        }
    } free{descriptor};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    Key key;
    check(RegCreateKeyExW(service.value, L"Parameters", 0, nullptr, 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE | READ_CONTROL | KEY_WOW64_64KEY,
                          &attributes, &key.value, nullptr),
          "Provision setup service configuration");
    writeBrokerInstallationKey(key.value, publishedInf);
}
} // namespace loopz::windows
