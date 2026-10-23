#include "broker_binary.hpp"
#include "broker_security.hpp"
#include <shlobj.h>
#include <array>

namespace loopz::windows {
std::filesystem::path brokerServiceDirectory() {
    PWSTR value{};
    const auto result =
        SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &value);
    if (FAILED(result))
        throw std::runtime_error("Cannot locate service installation directory");
    struct Free {
        PWSTR value;
        ~Free() {
            CoTaskMemFree(value);
        }
    } free{value};
    const auto directory = std::filesystem::path(value) / L"LoopZDeviceBroker";
    if (!directory.is_absolute() || directory != directory.lexically_normal() ||
        GetDriveTypeW(directory.root_path().c_str()) != DRIVE_FIXED)
        throw std::runtime_error("Setup service requires a fixed local installation directory");
    return directory;
}
struct BrokerBinaryGuard::Impl {
    HANDLE directory{INVALID_HANDLE_VALUE}, binary{INVALID_HANDLE_VALUE};
    ~Impl() {
        if (binary != INVALID_HANDLE_VALUE)
            CloseHandle(binary);
        if (directory != INVALID_HANDLE_VALUE)
            CloseHandle(directory);
    }
};
BrokerBinaryGuard::BrokerBinaryGuard() : impl_(std::make_unique<Impl>()) {
    std::array<wchar_t, 32768> module{};
    const auto count =
        GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!count || count >= module.size())
        throw std::runtime_error("Cannot locate setup service executable");
    const auto directory = brokerServiceDirectory();
    const auto expected = directory / L"LoopZDeviceBroker.exe";
    if (_wcsicmp(std::wstring(module.data(), count).c_str(), expected.c_str()) != 0)
        throw std::runtime_error("Setup service must run from its protected installed location");
    impl_->directory = CreateFileW(
        directory.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl_->directory == INVALID_HANDLE_VALUE)
        brokerProtectedObjectFailure("Open installed setup directory");
    validateBrokerProtectedObject(impl_->directory, true);
    impl_->binary =
        CreateFileW(expected.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl_->binary == INVALID_HANDLE_VALUE)
        brokerProtectedObjectFailure("Open installed setup executable");
    validateBrokerProtectedObject(impl_->binary, false);
}
BrokerBinaryGuard::~BrokerBinaryGuard() = default;
} // namespace loopz::windows
