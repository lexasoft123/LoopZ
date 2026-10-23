#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace loopz::windows {
// Caller owns planar[channels * frames] and pcm[channels * frames * 2].
// Quantization matches truncation of finite floats after PCM16 saturation.
// Return the number of frames whose encoded channels are all exactly zero.
inline uint32_t encodePcm16(const float* planar, uint32_t channels, uint32_t frames,
                            unsigned char* pcm) noexcept {
    uint32_t zeroFrames = 0, frame = 0;
    const auto scalar = [&](uint32_t f, uint32_t c) noexcept {
        const auto raw = planar[static_cast<size_t>(c) * frames + f];
        const float value = std::isfinite(raw) ? raw : 0;
        const auto sample = static_cast<int>(std::clamp(value * 32768.0f, -32768.0f, 32767.0f));
        const auto word = static_cast<unsigned short>(sample);
        const auto offset = (static_cast<size_t>(f) * channels + c) * 2;
        pcm[offset] = static_cast<unsigned char>(word & 255);
        pcm[offset + 1] = static_cast<unsigned char>(word >> 8);
        return word != 0;
    };
#if defined(__SSE2__) || defined(_M_X64)
    const auto magnitudeMask = _mm_set1_epi32(0x7fffffff);
    const auto infinityBits = _mm_set1_epi32(0x7f800000);
    const auto scale = _mm_set1_ps(32768.0f);
    const auto minimum = _mm_set1_ps(-32768.0f), maximum = _mm_set1_ps(32767.0f);
    const auto quantize = [&](const float* plane) noexcept {
        const auto raw = _mm_loadu_ps(plane);
        const auto finite = _mm_cmpgt_epi32(infinityBits,
            _mm_and_si128(_mm_castps_si128(raw), magnitudeMask));
        const auto value = _mm_mul_ps(_mm_and_ps(raw, _mm_castsi128_ps(finite)), scale);
        return _mm_min_ps(maximum, _mm_max_ps(minimum, value));
    };
    const auto store = [&](uint32_t f, uint32_t c, __m128i packed) noexcept {
        const auto word = static_cast<uint64_t>(_mm_cvtsi128_si64(packed));
        std::memcpy(pcm + (static_cast<size_t>(f) * channels + c) * 2, &word, sizeof(word));
    };
    for (; frames - frame >= 4; frame += 4) {
        unsigned nonzero = 0;
        uint32_t channel = 0;
        for (; channels - channel >= 4; channel += 4) {
            auto a = quantize(planar + static_cast<size_t>(channel) * frames + frame);
            auto b = quantize(planar + static_cast<size_t>(channel + 1) * frames + frame);
            auto c = quantize(planar + static_cast<size_t>(channel + 2) * frames + frame);
            auto d = quantize(planar + static_cast<size_t>(channel + 3) * frames + frame);
            _MM_TRANSPOSE4_PS(a, b, c, d);
            const auto first = _mm_packs_epi32(_mm_cvttps_epi32(a), _mm_cvttps_epi32(b));
            const auto second = _mm_packs_epi32(_mm_cvttps_epi32(c), _mm_cvttps_epi32(d));
            store(frame, channel, first);
            store(frame + 1, channel, _mm_srli_si128(first, 8));
            store(frame + 2, channel, second);
            store(frame + 3, channel, _mm_srli_si128(second, 8));
            const auto firstZeros = static_cast<unsigned>(
                _mm_movemask_epi8(_mm_cmpeq_epi16(first, _mm_setzero_si128())));
            const auto secondZeros = static_cast<unsigned>(
                _mm_movemask_epi8(_mm_cmpeq_epi16(second, _mm_setzero_si128())));
            nonzero |= ((firstZeros & 255) != 255) | (((firstZeros >> 8) != 255) << 1) |
                       (((secondZeros & 255) != 255) << 2) | (((secondZeros >> 8) != 255) << 3);
        }
        for (; channel < channels; ++channel)
            for (unsigned offset = 0; offset < 4; ++offset)
                nonzero |= static_cast<unsigned>(scalar(frame + offset, channel)) << offset;
        for (unsigned offset = 0; offset < 4; ++offset)
            zeroFrames += (nonzero & (1U << offset)) == 0;
    }
#endif
    for (; frame < frames; ++frame) {
        bool nonzero = false;
        for (uint32_t channel = 0; channel < channels; ++channel)
            nonzero = scalar(frame, channel) || nonzero;
        zeroFrames += !nonzero;
    }
    return zeroFrames;
}
} // namespace loopz::windows
