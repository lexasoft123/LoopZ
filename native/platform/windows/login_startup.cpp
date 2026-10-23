#include "login_startup.hpp"
#include <windows.h>
#include <array>
#include <filesystem>
#include <stdexcept>
namespace loopz::windows {
namespace {
constexpr wchar_t key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
std::wstring command() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size())
        throw std::runtime_error("Cannot locate LoopZ");
    const auto app = std::filesystem::path(path.data()).parent_path() / L"LoopZ.exe";
    return L"\"" + app.wstring() + L"\" --background";
}
} // namespace
bool loginStartupEnabled() {
    std::array<wchar_t, 32768> value{};
    DWORD bytes = static_cast<DWORD>(sizeof(value));
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"LoopZ", RRF_RT_REG_SZ, nullptr, value.data(),
                     &bytes) != ERROR_SUCCESS)
        return false;
    return value.data() == command();
}
void setLoginStartup(bool enabled) {
    HKEY registry{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &registry,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot update login startup");
    LSTATUS result;
    if (enabled) {
        const auto value = command();
        result = RegSetValueExW(registry, L"LoopZ", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else
        result = RegDeleteValueW(registry, L"LoopZ");
    RegCloseKey(registry);
    if (result != ERROR_SUCCESS && !(result == ERROR_FILE_NOT_FOUND && !enabled))
        throw std::runtime_error("Cannot update login startup");
}
} // namespace loopz::windows
