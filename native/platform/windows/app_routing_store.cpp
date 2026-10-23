#include "app_routing_store.hpp"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace loopz::windows {
namespace {
[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string(message) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
};
struct LocalMemory {
    void* value{};
    ~LocalMemory() {
        LocalFree(value);
    }
};
struct Security {
    std::vector<BYTE> tokenUser;
    std::array<BYTE, SECURITY_MAX_SID_SIZE> system{}, administrators{};
    LocalMemory descriptor;
    PSID user() const {
        return reinterpret_cast<const TOKEN_USER*>(tokenUser.data())->User.Sid;
    }
    Security() {
        HANDLE impersonation{};
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &impersonation)) {
            CloseHandle(impersonation);
            throw std::runtime_error("Routing storage cannot run while impersonating a client");
        }
        if (GetLastError() != ERROR_NO_TOKEN)
            fail("Inspect routing storage thread identity");
        HANDLE raw{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
            fail("Read routing storage user token");
        Handle token(raw);
        DWORD bytes{};
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
        if (!bytes || bytes > 65536)
            fail("Size routing storage user identity");
        tokenUser.resize(bytes);
        if (!GetTokenInformation(token.value, TokenUser, tokenUser.data(), bytes, &bytes))
            fail("Read routing storage user identity");
        DWORD size = SECURITY_MAX_SID_SIZE;
        if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &size))
            fail("Create routing storage system identity");
        size = SECURITY_MAX_SID_SIZE;
        if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size))
            fail("Create routing storage administrator identity");
        LocalMemory sid;
        if (!ConvertSidToStringSidW(user(), reinterpret_cast<LPWSTR*>(&sid.value)))
            fail("Encode routing storage user identity");
        const auto text = std::wstring(L"O:") + static_cast<LPCWSTR>(sid.value) +
                          L"D:P(A;OICI;FA;;;" + static_cast<LPCWSTR>(sid.value) +
                          L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1,
                                                                  &descriptor.value, nullptr))
            fail("Create routing storage access policy");
    }
    void validate(HANDLE handle, bool directory) const {
        LocalMemory memory;
        PSID owner{};
        PACL acl{};
        if (GetSecurityInfo(handle, SE_FILE_OBJECT,
                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                            &acl, nullptr, &memory.value) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot inspect routing storage access");
        if (!owner || !EqualSid(owner, user()) || !acl || !IsValidAcl(acl) || acl->AceCount != 3)
            throw std::runtime_error("Unexpected routing storage owner or access policy");
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision{};
        if (!GetSecurityDescriptorControl(memory.value, &control, &revision) ||
            (directory && !(control & SE_DACL_PROTECTED)))
            throw std::runtime_error("Routing storage directory must have protected access");
        unsigned seen{};
        for (DWORD index = 0; index < acl->AceCount; ++index) {
            void* entry{};
            if (!GetAce(acl, index, &entry))
                fail("Read routing storage access entry");
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
                ace->Header.AceSize < sizeof(ACCESS_ALLOWED_ACE) || ace->Mask != FILE_ALL_ACCESS ||
                (ace->Header.AceFlags & (INHERIT_ONLY_ACE | NO_PROPAGATE_INHERIT_ACE)) ||
                (directory &&
                 (ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) !=
                     (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)))
                throw std::runtime_error("Unsupported routing storage access entry");
            const auto sid = const_cast<DWORD*>(&ace->SidStart);
            const auto available = ace->Header.AceSize - offsetof(ACCESS_ALLOWED_ACE, SidStart);
            if (available < 8 ||
                reinterpret_cast<const SID*>(sid)->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES ||
                GetSidLengthRequired(reinterpret_cast<const SID*>(sid)->SubAuthorityCount) >
                    available ||
                !IsValidSid(sid))
                throw std::runtime_error("Invalid routing storage access identity");
            const unsigned bit = EqualSid(sid, user())                                     ? 1u
                                 : EqualSid(sid, const_cast<BYTE*>(system.data()))         ? 2u
                                 : EqualSid(sid, const_cast<BYTE*>(administrators.data())) ? 4u
                                                                                           : 0u;
            if (!bit || (seen & bit))
                throw std::runtime_error("Unexpected routing storage access identity");
            seen |= bit;
        }
        if (seen != 7)
            throw std::runtime_error("Incomplete routing storage access policy");
    }
};
void validatePath(const std::filesystem::path& root) {
    const auto& text = root.native();
    if (!root.is_absolute() || root != root.lexically_normal() || text.size() < 4 ||
        text.size() > 240 || text[1] != L':' || text[2] != L'\\' ||
        text.find(L':', 2) != std::wstring::npos || text.find(L'\0') != std::wstring::npos ||
        text.find(L'/') != std::wstring::npos ||
        GetDriveTypeW(root.root_path().c_str()) != DRIVE_FIXED)
        throw std::invalid_argument("Routing storage requires an ordinary local fixed-drive path");
    for (const auto& component : root.relative_path()) {
        const auto& name = component.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' ||
            name.back() == L' ')
            throw std::invalid_argument("Invalid routing storage path component");
    }
}
void validateObject(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info))
        fail("Inspect routing storage object");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1))
        throw std::runtime_error("Routing storage refuses redirected or linked objects");
}
Handle openDirectory(const std::filesystem::path& path) {
    Handle result(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (result.value == INVALID_HANDLE_VALUE)
        fail("Pin routing storage directory");
    validateObject(result.value, true);
    return result;
}
} // namespace
std::filesystem::path appRoutingStoreRoot() {
    PWSTR text{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &text)))
        throw std::runtime_error("Cannot locate user routing storage");
    const auto root = std::filesystem::path(text) / L"LoopZRoutingRecovery";
    CoTaskMemFree(text);
    validatePath(root);
    return root;
}
struct AppRoutingStore::Impl {
    // Destruction closes SQLite before releasing the file and ancestor pins.
    std::vector<Handle> directories;
    std::unique_ptr<Handle> file;
    std::unique_ptr<AppRoutingJournal> database;
    explicit Impl(const std::filesystem::path& root, bool initialize) {
        validatePath(root);
        Security security;
        auto current = root.root_path();
        directories.push_back(openDirectory(current));
        for (const auto& component : root.relative_path()) {
            current /= component;
            if (current == root && initialize) {
                SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.descriptor.value,
                                               FALSE};
                if (!CreateDirectoryW(root.c_str(), &attributes) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                    fail("Create user routing storage directory");
            }
            directories.push_back(openDirectory(current));
        }
        security.validate(directories.back().value, true);
        const auto path = root / L"routing.sqlite3";
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.descriptor.value, FALSE};
        file = std::make_unique<Handle>(CreateFileW(
            path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &attributes, initialize ? OPEN_ALWAYS : OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file->value == INVALID_HANDLE_VALUE)
            fail("Pin user routing database");
        validateObject(file->value, false);
        security.validate(file->value, false);
        // Validate leftovers before SQLite can recover them. Only the current
        // user, SYSTEM and administrators can create/replace files in this root.
        // Do not pin sidecars: SQLite must be able to delete its rollback journal.
        for (const auto* suffix : {L"-journal", L"-wal", L"-shm"}) {
            const auto sidecar = path.wstring() + suffix;
            Handle existing(CreateFileW(sidecar.c_str(), GENERIC_READ | READ_CONTROL,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (existing.value == INVALID_HANDLE_VALUE) {
                if (GetLastError() != ERROR_FILE_NOT_FOUND)
                    fail("Inspect routing recovery sidecar");
                continue;
            }
            validateObject(existing.value, false);
            security.validate(existing.value, false);
        }
        database = std::make_unique<AppRoutingJournal>(path, initialize);
    }
};
AppRoutingStore::AppRoutingStore(const std::filesystem::path& root, bool initialize)
    : impl_(std::make_unique<Impl>(root, initialize)) {}
AppRoutingStore::~AppRoutingStore() = default;
AppRoutingJournal& AppRoutingStore::journal() noexcept {
    return *impl_->database;
}
} // namespace loopz::windows
