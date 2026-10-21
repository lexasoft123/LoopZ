#include <loopz/sample_rates.hpp>
#include <loopz/macos/driver_configuration.hpp>
#include <loopz/route_document.hpp>

#include <stdexcept>
#include <unordered_set>
#include <cctype>

namespace loopz::macos {
namespace {
bool validOrigin(const std::string& origin) {
    if (origin.size() != 36)
        return false;
    for (size_t index = 0; index < origin.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? origin[index] != '-'
                      : !std::isxdigit(static_cast<unsigned char>(origin[index])))
            return false;
    }
    return true;
}
} // namespace
void validateDriverRateRequests(std::span<const DriverRateRequest> requests) {
    if (requests.size() > kMaximumVirtualDevices)
        throw std::invalid_argument("Too many device rate requests");
    std::unordered_set<std::string> devices;
    for (const auto& request : requests) {
        if (request.device.empty() || request.device.size() > 4096 ||
            request.device.find('\0') != std::string::npos || !request.sequence ||
            !supportedVirtualSampleRate(request.sampleRate) ||
            !devices.insert(request.device).second)
            throw std::invalid_argument("Invalid or duplicate device rate request");
        if (!validOrigin(request.origin))
            throw std::invalid_argument("Invalid native rate request origin");
    }
}
std::string driverRateReceipt(const DriverRateRequest& request) {
    return request.origin + "/" + std::to_string(request.sequence);
}
} // namespace loopz::macos
