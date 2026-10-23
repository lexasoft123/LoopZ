#include "app_routing_preferences.hpp"
#include <windows.h>
#include <stdexcept>

namespace loopz::windows {
namespace {
std::string encode(std::wstring_view value) {
    if (value.empty())
        return {};
    if (value.size() > 32768 || value.find(L'\0') != std::wstring_view::npos)
        throw std::runtime_error("Invalid app output preference");
    const auto count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0 || count > 32768)
        throw std::runtime_error("Invalid app output preference encoding");
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count, nullptr,
                            nullptr) != count)
        throw std::runtime_error("Cannot encode app output preference");
    return result;
}
std::wstring decode(std::string_view value) {
    if (value.empty())
        return {};
    if (value.size() > 32768 || value.find('\0') != std::string_view::npos)
        throw std::runtime_error("Invalid stored app output preference");
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
        throw std::runtime_error("Invalid stored app output preference encoding");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count)
        throw std::runtime_error("Cannot decode stored app output preference");
    return result;
}
} // namespace
ProcessRoutingPreferences::ProcessRoutingPreferences(AppAudioPolicy& policy, uint32_t pid,
                                                     const std::filesystem::path& image,
                                                     std::string_view expected)
    : policy_(policy), process_(pid, image, expected) {
    if (!policy_.available())
        throw std::runtime_error(policy_.error());
}
const RoutingProcessIdentity& ProcessRoutingPreferences::processIdentity() const noexcept {
    return process_.identity();
}
std::string ProcessRoutingPreferences::application() const {
    process_.validate();
    return process_.identity().application;
}
std::array<std::string, 3> ProcessRoutingPreferences::read() {
    std::array<std::string, 3> result;
    for (unsigned role = 0; role < 3; ++role) {
        const auto pid = process_.livePid();
        std::wstring value;
        std::string error;
        if (!policy_.get(pid, role, value, error))
            throw std::runtime_error(error);
        process_.validate();
        result[role] = encode(value);
    }
    return result;
}
void ProcessRoutingPreferences::write(unsigned role, std::string_view value) {
    if (role >= 3)
        throw std::invalid_argument("Invalid app output role");
    const auto decoded = decode(value);
    const auto pid = process_.livePid();
    std::string error;
    if (!policy_.set(pid, role, decoded, error))
        throw std::runtime_error(error);
    process_.validate();
}
} // namespace loopz::windows
