#include "app_routing_process.hpp"
#include <windows.h>
#include <appmodel.h>
#include <sddl.h>
#include <array>
#include <stdexcept>
#include <vector>

namespace loopz::windows {
namespace {
[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string(message) + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
}
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void refuseImpersonation() {
    Handle token;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.value))
        throw std::runtime_error("App routing cannot run while impersonating a client");
    if (GetLastError() != ERROR_NO_TOKEN)
        fail("Inspect app routing thread identity");
}
struct Owner {
    std::wstring sid;
    DWORD session{};
    bool operator==(const Owner&) const = default;
};
Owner owner(HANDLE process) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value))
        fail("Inspect app routing process token");
    DWORD bytes{};
    if (GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes || bytes > 65536)
        throw std::runtime_error("Invalid app routing process identity size");
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), bytes, &bytes))
        fail("Read app routing process owner");
    PSID sid = reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid;
    if (!IsValidSid(sid) || IsWellKnownSid(sid, WinAnonymousSid))
        throw std::runtime_error("Invalid app routing process owner");
    LPWSTR text{};
    if (!ConvertSidToStringSidW(sid, &text))
        fail("Encode app routing process owner");
    Owner result;
    try {
        result.sid = text;
    } catch (...) {
        LocalFree(text);
        throw;
    }
    LocalFree(text);
    if (!GetTokenInformation(token.value, TokenSessionId, &result.session, sizeof(result.session),
                             &bytes))
        fail("Read app routing process session");
    return result;
}
std::wstring reportedImage(HANDLE process) {
    std::array<wchar_t, 32768> text{};
    DWORD count = static_cast<DWORD>(text.size());
    if (!QueryFullProcessImageNameW(process, 0, text.data(), &count) || !count ||
        count >= text.size())
        fail("Read app routing executable path");
    return {text.data(), count};
}
uint64_t creation(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        fail("Read app routing process creation");
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
bool samePath(std::wstring_view a, std::wstring_view b) {
    return a.size() <= 32768 && b.size() <= 32768 &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
struct Image {
    std::wstring path;
    BY_HANDLE_FILE_INFORMATION file{};
};
Image resolve(const std::filesystem::path& path) {
    if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos ||
        path.native().size() > 32768)
        throw std::invalid_argument("App routing selection requires an absolute executable path");
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE)
        fail("Resolve selected app routing executable");
    Image result;
    if (GetFileType(file.value) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(file.value, &result.file) ||
        (result.file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        throw std::runtime_error("App routing selection is not an executable file");
    std::array<wchar_t, 32768> text{};
    const auto count =
        GetFinalPathNameByHandleW(file.value, text.data(), static_cast<DWORD>(text.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (!count || count >= text.size())
        fail("Normalize selected app routing executable");
    result.path.assign(text.data(), count);
    return result;
}
std::wstring appModelText(HANDLE process, bool package) {
    UINT32 size{};
    const auto query = [&](UINT32* count, PWSTR buffer) {
        return package ? GetPackageFullName(process, count, buffer)
                       : GetApplicationUserModelId(process, count, buffer);
    };
    auto code = query(&size, nullptr);
    if (code == (package ? APPMODEL_ERROR_NO_PACKAGE : APPMODEL_ERROR_NO_APPLICATION))
        return {};
    if (code != ERROR_INSUFFICIENT_BUFFER || size < 2 || size > 32768)
        throw std::runtime_error("Cannot resolve app routing application model identity (" +
                                 std::to_string(code) + ")");
    std::vector<wchar_t> text(size);
    code = query(&size, text.data());
    if (code != ERROR_SUCCESS || size < 2 || size > text.size() || text[size - 1] != L'\0')
        throw std::runtime_error("Cannot read app routing application model identity");
    const std::wstring result(text.data(), size - 1);
    if (result.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("Invalid app routing application model identity");
    return result;
}
std::string utf8(std::wstring_view value) {
    if (value.empty())
        return {};
    if (value.size() > 32768)
        throw std::runtime_error("App routing identity is too long");
    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        fail("Size app routing identity encoding");
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size, nullptr,
                            nullptr) != size)
        fail("Encode app routing identity");
    return result;
}
} // namespace
struct AppRoutingProcess::Impl {
    Handle process;
    RoutingProcessIdentity identity;
    Owner caller;
    Impl(uint32_t pid, const std::filesystem::path& selectedImage, std::string_view expected) {
        refuseImpersonation();
        if (!pid)
            throw std::invalid_argument("Invalid app routing process ID");
        caller = owner(GetCurrentProcess());
        process.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process.value)
            fail("Pin app routing process");
        if (WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
            throw std::runtime_error("App routing process has exited");
        const auto selected = resolve(selectedImage);
        identity.reportedImage = reportedImage(process.value);
        const auto image = resolve(identity.reportedImage);
        if (!samePath(selected.path, image.path) ||
            selected.file.dwVolumeSerialNumber != image.file.dwVolumeSerialNumber ||
            selected.file.nFileIndexHigh != image.file.nFileIndexHigh ||
            selected.file.nFileIndexLow != image.file.nFileIndexLow)
            throw std::runtime_error("App routing process does not match the selected executable");
        if (owner(process.value) != caller)
            throw std::runtime_error(
                "App routing process belongs to a different user or Windows session");
        identity.pid = pid;
        identity.created100ns = creation(process.value);
        identity.ownerSid = caller.sid;
        identity.session = caller.session;
        identity.resolvedImage = image.path;
        identity.packageFullName = appModelText(process.value, true);
        identity.applicationUserModelId = appModelText(process.value, false);
        if (!identity.packageFullName.empty() && identity.applicationUserModelId.empty())
            throw std::runtime_error("Packaged audio host has no distinct application identity");
        identity.application = "windows-routing-v1|" + utf8(identity.ownerSid) +
                               (identity.packageFullName.empty()
                                    ? "|desktop|" + utf8(identity.resolvedImage)
                                    : "|appmodel|" + utf8(identity.applicationUserModelId));
        if (identity.application.size() > 32768 ||
            (!expected.empty() && expected != identity.application))
            throw std::runtime_error(
                "App routing recovery identity does not match this application");
        validate();
    }
    void validate() const {
        refuseImpersonation();
        if (WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT ||
            GetProcessId(process.value) != identity.pid ||
            creation(process.value) != identity.created100ns)
            throw std::runtime_error("App routing process has exited or changed");
        if (owner(GetCurrentProcess()) != caller || owner(process.value) != caller ||
            !samePath(reportedImage(process.value), identity.reportedImage) ||
            appModelText(process.value, true) != identity.packageFullName ||
            appModelText(process.value, false) != identity.applicationUserModelId)
            throw std::runtime_error("App routing process identity changed");
    }
};
AppRoutingProcess::AppRoutingProcess(uint32_t pid, const std::filesystem::path& image,
                                     std::string_view expected)
    : impl_(std::make_unique<Impl>(pid, image, expected)) {}
AppRoutingProcess::~AppRoutingProcess() = default;
const RoutingProcessIdentity& AppRoutingProcess::identity() const noexcept {
    return impl_->identity;
}
void AppRoutingProcess::validate() const {
    impl_->validate();
}
uint32_t AppRoutingProcess::livePid() const {
    validate();
    return impl_->identity.pid;
}
} // namespace loopz::windows
