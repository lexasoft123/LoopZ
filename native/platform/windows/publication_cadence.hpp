#pragma once
#include <cstdint>
#include <limits>

namespace loopz::windows {
// Every advertised rate has integral frames at both 100 and 300 batches/s.
// Derive each deadline from the origin: accumulating rounded 1/300-second
// periods would lose one nanosecond every three batches.
inline constexpr uint32_t publicationBatchFrames(uint32_t rate, bool smallBatches) noexcept {
    const uint32_t frequency = smallBatches ? 300 : 100;
    return rate && rate % frequency == 0 ? rate / frequency : 0;
}
inline constexpr uint64_t publicationBatchTime(uint64_t origin, uint64_t batch,
                                               bool smallBatches) noexcept {
    const uint64_t frequency = smallBatches ? 300 : 100;
    constexpr auto limit = std::numeric_limits<uint64_t>::max();
    const auto seconds = batch / frequency;
    const auto remainder = batch % frequency * 1000000000 / frequency;
    if (seconds > (limit - origin) / 1000000000)
        return limit;
    const auto whole = origin + seconds * 1000000000;
    return remainder > limit - whole ? limit : whole + remainder;
}
} // namespace loopz::windows
