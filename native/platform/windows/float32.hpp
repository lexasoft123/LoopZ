#pragma once
#include "driver/sample_bits.hpp"
#include <bit>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
namespace loopz::windows {
static_assert(sizeof(float) == 4 && std::endian::native == std::endian::little);
inline unsigned decodeFloat32Frame(const unsigned char* packed, unsigned channels, float* planar,
                                   unsigned stride) noexcept {
    unsigned nonzero = 0;
    for (unsigned channel = 0; channel < channels; ++channel) {
        unsigned bits{};
        std::memcpy(&bits, packed + channel * 4, 4);
        bits = loopz_driver::finiteFloatBits(bits);
        nonzero |= bits & 0x7fffffffU;
        planar[static_cast<size_t>(channel) * stride] = std::bit_cast<float>(bits);
    }
    return nonzero;
}
// Planar float -> interleaved IEEE float bytes. Preserve finite bits and
// headroom; invalid samples become zero. Count frames with only signed zeros.
inline uint32_t encodeFloat32(const float* planar, uint32_t channels, uint32_t frames,
                              unsigned char* packed) noexcept {
    uint32_t zeros = 0, frame = 0;
    const auto scalar = [&](uint32_t f, uint32_t c) {
        const auto bits = loopz_driver::finiteFloatBits(
            std::bit_cast<unsigned>(planar[static_cast<size_t>(c) * frames + f]));
        std::memcpy(packed + (static_cast<size_t>(f) * channels + c) * 4, &bits, 4);
        return (bits & 0x7fffffffU) != 0;
    };
#if defined(__SSE2__) || defined(_M_X64)
    const auto magnitude = _mm_set1_epi32(0x7fffffff);
    const auto infinity = _mm_set1_epi32(0x7f800000);
    const auto load = [&](const float* source) {
        const auto raw = _mm_loadu_ps(source);
        const auto finite =
            _mm_cmpgt_epi32(infinity, _mm_and_si128(_mm_castps_si128(raw), magnitude));
        return _mm_and_ps(raw, _mm_castsi128_ps(finite));
    };
    for (; frames - frame >= 4; frame += 4) {
        uint32_t nonzero = 0, channel = 0;
        for (; channels - channel >= 4; channel += 4) {
            auto a = load(planar + static_cast<size_t>(channel) * frames + frame);
            auto b = load(planar + static_cast<size_t>(channel + 1) * frames + frame);
            auto c = load(planar + static_cast<size_t>(channel + 2) * frames + frame);
            auto d = load(planar + static_cast<size_t>(channel + 3) * frames + frame);
            _MM_TRANSPOSE4_PS(a, b, c, d);
            const __m128 rows[]{a, b, c, d};
            for (unsigned offset = 0; offset < 4; ++offset) {
                std::memcpy(packed + (static_cast<size_t>(frame + offset) * channels + channel) * 4,
                            &rows[offset], 16);
                const auto zero = _mm_cmpeq_epi8(
                    _mm_and_si128(_mm_castps_si128(rows[offset]), magnitude), _mm_setzero_si128());
                nonzero |= static_cast<uint32_t>(_mm_movemask_epi8(zero) != 65535) << offset;
            }
        }
        for (; channel < channels; ++channel)
            for (unsigned offset = 0; offset < 4; ++offset)
                nonzero |= static_cast<uint32_t>(scalar(frame + offset, channel)) << offset;
        for (unsigned offset = 0; offset < 4; ++offset)
            zeros += (nonzero & (1U << offset)) == 0;
    }
#endif
    for (; frame < frames; ++frame) {
        bool nonzero = false;
        for (uint32_t channel = 0; channel < channels; ++channel)
            nonzero = scalar(frame, channel) || nonzero;
        zeros += !nonzero;
    }
    return zeros;
}
} // namespace loopz::windows
