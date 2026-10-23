#pragma once
#include <cstdint>

namespace loopz::windows {
// Keep the previous worst-case allocation (31 s, 192 kHz, 64 channels),
// independent of the requested duration. Longer runs must fit this same budget.
inline constexpr uint64_t probeMaximumSamples = uint64_t{31} * 192000 * 64;
inline constexpr bool validStressProbeRecording(unsigned rate, unsigned channels,
                                                unsigned milliseconds) noexcept {
    const bool supportedRate = rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000 ||
                               rate == 176400 || rate == 192000;
    return supportedRate && channels >= 2 && channels <= 64 && channels % 2 == 0 &&
           milliseconds >= 1500 && milliseconds <= 300000;
}
inline constexpr bool validProbeRecording(unsigned rate, unsigned channels,
                                          unsigned milliseconds) noexcept {
    return validStressProbeRecording(rate, channels, milliseconds) &&
           uint64_t(rate) * (milliseconds / 1000 + 1) * channels <= probeMaximumSamples;
}
} // namespace loopz::windows
