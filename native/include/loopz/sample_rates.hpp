#pragma once

#include <array>
#include <cstdint>

namespace loopz {

// Public virtual-device rates. Capture and monitor clocks can have other rates
// and are converted by the portable audio histories/resamplers.
inline constexpr std::array<uint32_t, 6> kVirtualSampleRates{44100, 48000,  88200,
                                                             96000, 176400, 192000};
inline constexpr uint32_t kAllVirtualSampleRates = (1U << kVirtualSampleRates.size()) - 1;
inline constexpr uint32_t kLegacyVirtualSampleRates = 3;

constexpr uint32_t virtualSampleRateBit(double value) noexcept {
    for (std::size_t index = 0; index < kVirtualSampleRates.size(); ++index)
        if (value == kVirtualSampleRates[index])
            return 1U << index;
    return 0;
}

constexpr bool supportedVirtualSampleRate(double value) noexcept {
    return virtualSampleRateBit(value) != 0;
}

constexpr bool validVirtualSampleRateMask(uint64_t value) noexcept {
    return (value & kLegacyVirtualSampleRates) == kLegacyVirtualSampleRates &&
           !(value & ~uint64_t(kAllVirtualSampleRates));
}

} // namespace loopz
