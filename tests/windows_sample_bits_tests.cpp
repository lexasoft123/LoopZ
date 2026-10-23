#include "../native/platform/windows/driver/sample_bits.hpp"
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <initializer_list>

namespace {
void check(unsigned bits) {
    const float value = std::bit_cast<float>(bits);
    const unsigned finite = std::isfinite(value) ? bits : 0;
    assert(loopz_driver::finiteFloatBits(bits) == finite);
    const double scaled = static_cast<double>(value) * 32768;
    const short expected = !std::isfinite(value) ? 0
                           : scaled >= 32767     ? 32767
                           : scaled <= -32768    ? -32768
                                                 : static_cast<short>(scaled);
    assert(loopz_driver::floatBitsToPcm16(bits) == expected);
}
} // namespace
int main() {
    static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);
    // Every PCM16 value maps exactly to float and back, including -32768.
    for (int value = -32768; value <= 32767; ++value) {
        const auto sample = static_cast<short>(value);
        const auto bits = loopz_driver::pcm16ToFloatBits(sample);
        assert(bits == std::bit_cast<unsigned>(static_cast<float>(value) / 32768));
        assert(loopz_driver::floatBitsToPcm16(bits) == sample);
    }
    // All representable values immediately around every quantization edge.
    for (int edge = -32769; edge <= 32769; ++edge) {
        const float value = static_cast<float>(edge) / 32768;
        check(std::bit_cast<unsigned>(value));
        check(std::bit_cast<unsigned>(
            std::nextafter(value, -std::numeric_limits<float>::infinity())));
        check(
            std::bit_cast<unsigned>(std::nextafter(value, std::numeric_limits<float>::infinity())));
    }
    // Every exponent/sign combination, finite extremes, subnormals and NaNs.
    for (unsigned exponent = 0; exponent < 256; ++exponent)
        for (unsigned sign = 0; sign < 2; ++sign)
            for (unsigned mantissa : {0U, 1U, 0x3fffffU, 0x400000U, 0x7ffffeU, 0x7fffffU})
                check((sign << 31) | (exponent << 23) | mantissa);
    uint32_t state = 0x12345678;
    for (unsigned i = 0; i < 1000000; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        check(state);
    }
    assert(loopz_driver::finiteFloatBits(0x80000000U) == 0x80000000U);
    assert(loopz_driver::finiteFloatBits(1) == 1);
    assert(loopz_driver::finiteFloatBits(std::bit_cast<unsigned>(2.0f)) ==
           std::bit_cast<unsigned>(2.0f));
}
