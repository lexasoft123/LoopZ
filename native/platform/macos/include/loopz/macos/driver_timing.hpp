#pragma once

#include <cstdint>

namespace loopz::macos {
// Read-only CFPropertyList dictionary, advertised through the HAL custom
// property list. It reports WriteMix callback periods, not a caller's IOProc
// buffer setting. No audio mapping/protocol changes are required.
inline constexpr uint32_t kLoopZDevicePropertyIOTiming = 0x6c7a746d; // 'lztm'
inline constexpr uint32_t kLoopZDeviceIOTimingVersion = 1;
// Additional diagnostic property. Keep the legacy dictionary/version intact:
// existing Engines must remain compatible with a newer installed driver.
inline constexpr uint32_t kLoopZDevicePropertyCallbackTiming = 0x6c7a6362; // 'lzcb'
inline constexpr uint32_t kLoopZDeviceCallbackTimingVersion = 1;
} // namespace loopz::macos
