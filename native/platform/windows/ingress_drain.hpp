#pragma once
#include <cstdint>

namespace loopz::windows {
// A private worker must service both directions even when output is late.
// After its minimum frame service, additional ingress uses only deadline slack.
inline constexpr bool ingressDrainMayContinue(uint64_t now, uint64_t deadline,
                                              uint32_t receivedFrames,
                                              uint32_t minimumFrames) noexcept {
    return receivedFrames < minimumFrames || now < deadline;
}
} // namespace loopz::windows
