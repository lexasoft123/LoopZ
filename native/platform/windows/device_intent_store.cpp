#include "device_intent_store.hpp"
#include "broker_security.hpp"
#include "driver/audio_format.hpp"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <cstddef>
#include <stdexcept>

namespace loopz::windows {
namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void close() {
        const auto old = value;
        value = INVALID_HANDLE_VALUE;
        if (old != INVALID_HANDLE_VALUE && !CloseHandle(old))
            fail("Close setup database");
    }
};
struct Descriptor {
    PSECURITY_DESCRIPTOR value{};
    explicit Descriptor(bool directory) {
        const auto* sddl = directory ? L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
                                     : L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &value,
                                                                  nullptr))
            fail("Create setup database security");
    }
    ~Descriptor() {
        if (value)
            LocalFree(value);
    }
};
void localAbsolute(const std::filesystem::path& root) {
    if (!root.is_absolute() || root != root.lexically_normal() ||
        root.native().find(L'\0') != std::wstring::npos ||
        GetDriveTypeW(root.root_path().c_str()) != DRIVE_FIXED)
        throw std::invalid_argument(
            "Setup database directory must be an absolute local fixed-drive path");
}
std::string validatedOwner(std::wstring_view owner) {
    if (owner.empty() || owner.size() > 184 || owner.find(L'\0') != std::wstring_view::npos)
        throw std::invalid_argument("Invalid authenticated setup owner");
    const std::wstring text(owner);
    PSID sid{};
    if (!ConvertStringSidToSidW(text.c_str(), &sid))
        throw std::invalid_argument("Invalid authenticated setup owner");
    LPWSTR canonical{};
    const bool valid = IsValidSid(sid) && ConvertSidToStringSidW(sid, &canonical);
    LocalFree(sid);
    const bool equal = valid && text == canonical;
    if (canonical)
        LocalFree(canonical);
    if (!equal)
        throw std::invalid_argument("Invalid authenticated setup owner");
    std::string ascii;
    ascii.reserve(text.size());
    for (const auto character : text) {
        if (character > 127)
            throw std::invalid_argument("Invalid authenticated setup owner");
        ascii.push_back(static_cast<char>(character));
    }
    return ascii;
}
} // namespace
std::filesystem::path deviceIntentStoreRoot() {
    PWSTR path{};
    const auto result = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &path);
    if (FAILED(result))
        throw std::runtime_error("Cannot locate machine setup database directory");
    const std::filesystem::path root = std::filesystem::path(path) / L"LoopZDeviceBroker";
    CoTaskMemFree(path);
    localAbsolute(root);
    return root;
}
void provisionDeviceIntentStore(const std::filesystem::path& root) {
    localAbsolute(root);
    Descriptor descriptor(true);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
    if (!CreateDirectoryW(root.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        fail("Create protected setup database directory");
    Handle directory(CreateFileW(root.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                 nullptr));
    if (directory.value == INVALID_HANDLE_VALUE)
        fail("Open protected setup database directory");
    validateBrokerProtectedObject(directory.value, true);
}
struct DeviceIntentStore::Impl {
    Handle directory;
    std::unique_ptr<DeviceIntentDatabase> database;
    explicit Impl(const std::filesystem::path& root) {
        localAbsolute(root);
        directory.value =
            CreateFileW(root.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (directory.value == INVALID_HANDLE_VALUE)
            fail("Open setup database directory");
        validateBrokerProtectedObject(directory.value,
                                      true); // Deny rename for this store's lifetime.
        const auto path = root / L"setup.sqlite3";
        Descriptor descriptor(false);
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
        Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &attributes,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
            fail("Open protected setup database");
        validateBrokerProtectedObject(file.value, false);
        file.close();
        // The installer-provisioned protected root authorizes initializing an
        // empty database, also after a crash during its first schema transaction.
        database = std::make_unique<DeviceIntentDatabase>(path, true);
    }
};
DeviceIntentStore::DeviceIntentStore(const std::filesystem::path& root)
    : impl_(std::make_unique<Impl>(root)) {}
DeviceIntentStore::~DeviceIntentStore() = default;
uint64_t DeviceIntentStore::accept(std::wstring_view ownerSid, const DeviceIntentRequest& request) {
    if (request.action == DeviceIntentAction::Upsert &&
        !loopz_driver::AudioFormat{request.sampleRate, request.channels}.valid())
        throw std::invalid_argument("Unsupported Windows microphone format");
    return impl_->database->accept(validatedOwner(ownerSid), request);
}
DeviceIntentSnapshot DeviceIntentStore::snapshot(std::wstring_view ownerSid) const {
    return impl_->database->snapshot(validatedOwner(ownerSid));
}
std::vector<std::wstring> DeviceIntentStore::ownersWithDesiredState() const {
    std::vector<std::wstring> result;
    for (const auto& owner : impl_->database->ownersWithDesiredState()) {
        const std::wstring sid(owner.begin(), owner.end());
        if (validatedOwner(sid) != owner)
            throw std::runtime_error("Invalid stored setup owner identity");
        result.push_back(sid);
    }
    return result;
}
bool DeviceIntentStore::removalApplied(std::wstring_view ownerSid, std::string_view mixId,
                                       uint64_t revision, DeviceIntentKind kind) {
    return impl_->database->removalApplied(validatedOwner(ownerSid), mixId, revision, kind);
}
} // namespace loopz::windows
