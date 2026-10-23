// LoopZ contributors. MIT license. Integer-only IEEE-754/PCM16 boundaries.
#pragma once
namespace loopz_driver {
static_assert(sizeof(unsigned) == 4 && sizeof(short) == 2);

// Preserve every finite float bit, including signed zero, subnormals and
// headroom above unity. Invalid samples become silence at the trust boundary.
constexpr unsigned finiteFloatBits(unsigned bits) noexcept {
    return (bits & 0x7fffffffU) < 0x7f800000U ? bits : 0;
}
// Exact signed PCM16 normalization by 32768. No floating-point instructions,
// allocation, locks or runtime/library calls are needed in a driver callback.
constexpr unsigned pcm16ToFloatBits(short sample) noexcept {
    if (!sample)
        return 0;
    const bool negative = sample < 0;
    const unsigned magnitude =
        negative ? static_cast<unsigned>(-static_cast<int>(sample)) : static_cast<unsigned>(sample);
    unsigned value = magnitude;
    unsigned highest = 0;
    if (value >= 256) {
        value >>= 8;
        highest += 8;
    }
    if (value >= 16) {
        value >>= 4;
        highest += 4;
    }
    if (value >= 4) {
        value >>= 2;
        highest += 2;
    }
    if (value >= 2)
        ++highest;
    return (negative ? 0x80000000U : 0) | ((highest + 112) << 23) |
           ((magnitude - (1U << highest)) << (23 - highest));
}
// Match the current userspace quantizer: finite values scaled by 32768,
// saturated to signed PCM16, then truncated toward zero. Nonfinite -> zero.
constexpr short floatBitsToPcm16(unsigned bits) noexcept {
    const unsigned magnitude = bits & 0x7fffffffU;
    if (magnitude >= 0x7f800000U)
        return 0;
    const bool negative = (bits >> 31) != 0;
    if (magnitude >= 0x3f800000U)
        return negative ? static_cast<short>(-32768) : static_cast<short>(32767);
    const unsigned exponent = magnitude >> 23;
    if (exponent < 112)
        return 0;
    const unsigned scaled = ((magnitude & 0x7fffffU) | 0x800000U) >> (135 - exponent);
    return static_cast<short>(negative ? -static_cast<int>(scaled) : static_cast<int>(scaled));
}
} // namespace loopz_driver
