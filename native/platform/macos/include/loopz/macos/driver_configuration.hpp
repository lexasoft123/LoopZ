#pragma once

#include <cstdint>
#include <string>
#include <span>

namespace loopz::macos {

enum class DriverConfigurationPhase : uint32_t {
    Unknown = 0,
    Pending = 1,
    RollingBack = 2,
    Committed = 3,
    Aborted = 4,
    Quarantined = 5,
};

struct DriverConfigurationResult {
    uint64_t revision{0};
    DriverConfigurationPhase phase{DriverConfigurationPhase::Unknown};
    int32_t error{0};
};
static_assert(sizeof(DriverConfigurationResult) == 16);

// A driver-side control request remains pending until the helper durably
// accepts its exact token. Later requests for the same device replace it.
struct DriverRateRequest {
    std::string device;
    uint64_t sequence;
    uint32_t sampleRate;
    std::string origin;
};
void validateDriverRateRequests(std::span<const DriverRateRequest> requests);
std::string driverRateReceipt(const DriverRateRequest& request);

} // namespace loopz::macos
