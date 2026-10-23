#include "../native/platform/windows/float32.hpp"
#include "../native/platform/windows/pcm16.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <vector>
void verify() {
    constexpr std::array<unsigned, 16> words{0U,          0x80000000U, 1U,          0x80000001U,
                                             0x3f000001U, 0xbf000001U, 0x40000001U, 0xc0000001U,
                                             0x7f7fffffU, 0xff7fffffU, 0x7f800000U, 0xff800000U,
                                             0x7f800001U, 0xff800001U, 0x7fc12345U, 0xffc12345U};
    for (unsigned channels = 0; channels <= 64; ++channels)
        for (unsigned frames : {0U, 1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U, 240U, 441U, 480U}) {
            std::vector<float> input(static_cast<size_t>(channels) * frames + 1);
            for (size_t i = 0; i < input.size(); ++i)
                input[i] = std::bit_cast<float>(words[i % words.size()]);
            std::vector<unsigned char> actual(static_cast<size_t>(channels) * frames * 4 + 2, 211),
                expected(actual.size(), 211);
            unsigned zeros = 0;
            for (unsigned f = 0; f < frames; ++f) {
                bool nonzero = false;
                for (unsigned c = 0; c < channels; ++c) {
                    unsigned bits = words[(1 + static_cast<size_t>(c) * frames + f) % words.size()];
                    if ((bits & 0x7f800000U) == 0x7f800000U)
                        bits = 0;
                    std::memcpy(expected.data() + 1 + (static_cast<size_t>(f) * channels + c) * 4,
                                &bits, 4);
                    nonzero = nonzero || ((bits & 0x7fffffffU) != 0);
                }
                zeros += !nonzero;
            }
            const auto actualZeros = loopz::windows::encodeFloat32(input.data() + 1, channels,
                                                                   frames, actual.data() + 1);
            if (actualZeros != zeros) {
                std::fprintf(stderr, "channels=%u frames=%u zeros=%u expected=%u equal=%u\n",
                             channels, frames, actualZeros, zeros, actual == expected);
#if defined(__SSE2__) || defined(_M_X64)
                std::fprintf(stderr, "mxcsr=%x\n", _mm_getcsr());
#endif
                for (unsigned f = 0; f < frames; ++f) {
                    unsigned raw;
                    std::memcpy(&raw, actual.data() + 1 + f * channels * 4, 4);
                    std::fprintf(stderr, "%x ", raw);
                }
            }
            assert(actualZeros == zeros);
            assert(actual == expected);
            std::vector<float> decoded(static_cast<size_t>(channels) * (frames + 1) + frames + 1,
                                       123.0f);
            for (unsigned f = 0; f < frames; ++f) {
                const auto nonzero = loopz::windows::decodeFloat32Frame(
                    actual.data() + 1 + static_cast<size_t>(f) * channels * 4, channels,
                    decoded.data() + 1 + f, frames + 1);
                unsigned expectedNonzero = 0;
                for (unsigned c = 0; c < channels; ++c) {
                    unsigned bits;
                    std::memcpy(&bits,
                                expected.data() + 1 + (static_cast<size_t>(f) * channels + c) * 4,
                                4);
                    expectedNonzero |= bits & 0x7fffffffU;
                    assert(std::bit_cast<unsigned>(
                               decoded[1 + static_cast<size_t>(c) * (frames + 1) + f]) == bits);
                }
                assert(nonzero == expectedNonzero);
            }
            assert(decoded.front() == 123.0f);
            for (unsigned c = 0; c < channels; ++c)
                assert(decoded[1 + static_cast<size_t>(c) * (frames + 1) + frames] == 123.0f);
        }
}

void belowPcm16Step() {
    constexpr unsigned frames = 441, channels = 2;
    std::vector<float> input(frames * channels), decoded(input.size());
    std::vector<unsigned char> floating(input.size() * 4), pcm(input.size() * 2);
    for (unsigned c = 0; c < channels; ++c)
        for (unsigned f = 0; f < frames; ++f)
            input[c * frames + f] =
                1e-6f *
                static_cast<float>(std::sin(6.283185307179586 * (443 + c * 37) * f / 48000));
    const auto floatZeros =
        loopz::windows::encodeFloat32(input.data(), channels, frames, floating.data());
    assert(floatZeros < frames / 2);
    for (unsigned f = 0; f < frames; ++f)
        loopz::windows::decodeFloat32Frame(floating.data() + f * channels * 4, channels,
                                           decoded.data() + f, frames);
    for (unsigned i = 0; i < input.size(); ++i)
        assert(std::bit_cast<unsigned>(input[i]) == std::bit_cast<unsigned>(decoded[i]));
    // Negative control: a hidden PCM16 boundary destroys the whole marker.
    assert(loopz::windows::encodePcm16(input.data(), channels, frames, pcm.data()) == frames);
    for (auto byte : pcm)
        assert(byte == 0);
}
int main() {
    belowPcm16Step();
    verify();
#if defined(__SSE2__) || defined(_M_X64)
    const auto previous = _mm_getcsr();
    _mm_setcsr(previous | 0x8040U); // DAZ/FTZ cannot discard float payload bits.
    verify();
    _mm_setcsr(previous);
#endif
}
