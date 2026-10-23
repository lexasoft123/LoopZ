#include "../native/platform/windows/pcm16.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#define LOOPZ_NO_INLINE __declspec(noinline)
#else
#define LOOPZ_NO_INLINE __attribute__((noinline))
#endif
LOOPZ_NO_INLINE uint32_t reference(const float* input, uint32_t channels, uint32_t frames,
                                    unsigned char* output) noexcept {
    uint32_t zeros = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        bool allZero = true;
        for (uint32_t c = 0; c < channels; ++c) {
            const auto raw = input[static_cast<size_t>(c) * frames + f];
            const float v = std::isfinite(raw) ? raw : 0;
            const auto sample = static_cast<int>(std::clamp(v * 32768.0f, -32768.0f, 32767.0f));
            const auto word = static_cast<unsigned short>(sample);
            output[(static_cast<size_t>(f) * channels + c) * 2] = static_cast<unsigned char>(word & 255);
            output[(static_cast<size_t>(f) * channels + c) * 2 + 1] = static_cast<unsigned char>(word >> 8);
            allZero = allZero && word == 0;
        }
        zeros += allZero;
    }
    return zeros;
}
LOOPZ_NO_INLINE uint32_t optimized(const float* input, uint32_t channels, uint32_t frames,
                                    unsigned char* output) noexcept {
    return loopz::windows::encodePcm16(input, channels, frames, output);
}
int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--benchmark" ||
                      std::string_view(argv[1]) == "--benchmark-reference")) {
        constexpr uint32_t channels = 64, frames = 1920;
        std::vector<float> input(channels * frames);
        std::vector<unsigned char> output(input.size() * 2);
        for (unsigned i = 0; i < input.size(); ++i)
            input[i] = .05f * std::sin(static_cast<float>(i) * .017f);
        const auto operation = std::string_view(argv[1]) == "--benchmark" ? optimized : reference;
        uint32_t zeros = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 200; ++i)
            zeros += operation(input.data(), channels, frames, output.data());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        assert(zeros == 0 && std::any_of(output.begin(), output.end(),
            [](unsigned char value) { return value != 0; }));
        std::printf("{\"channels\":64,\"frames\":1920,\"calls\":200,\"nsPerCall\":%.1f}\n",
            std::chrono::duration<double, std::nano>(elapsed).count() / 200);
        return 0;
    }
    const std::array<uint32_t, 20> patterns{0, 0x80000000, 0x3d4ccccd, 0xbd4ccccd,
        0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345, 1, 0x80000001,
        0x7f7fffff, 0xff7fffff, 0x00800000, 0x80800000, 0x3f800000, 0xbf800000,
        0x37ffffff, 0x38000000, 0xb7ffffff, 0xb8000000};
    uint32_t bits = 0x12345678;
    unsigned cases = 0;
    for (uint32_t channels : {1U, 2U, 3U, 4U, 6U, 8U, 16U, 32U, 64U})
        for (uint32_t frames : {0U, 1U, 3U, 4U, 7U, 63U, 256U, 441U, 1920U})
            for (unsigned mode = 0; mode < 5; ++mode)
                for (unsigned offset : {0U, 1U, 3U}) {
                    std::vector<float> input(static_cast<size_t>(channels) * frames + offset + 1);
                    std::vector<unsigned char> actual(static_cast<size_t>(channels) * frames * 2 + offset + 16, 0xa5);
                    auto expected = actual;
                    for (uint32_t c = 0; c < channels; ++c)
                        for (uint32_t f = 0; f < frames; ++f) {
                            bits = bits * 1664525U + 1013904223U;
                            input[offset + static_cast<size_t>(c) * frames + f] = mode == 0
                                ? std::bit_cast<float>(patterns[(f + c) % patterns.size()])
                                : mode == 1 ? std::bit_cast<float>(bits)
                                : mode == 2 ? 0
                                : mode == 3 ? .00001f
                                : (c == channels - 1 && f % 7 == 0 ? -1.0f / 32768 : 0);
                        }
                    const auto a = optimized(input.data() + offset, channels, frames, actual.data() + offset);
                    const auto b = reference(input.data() + offset, channels, frames, expected.data() + offset);
                    assert(a == b && actual == expected);
                    ++cases;
                }
    std::printf("Windows PCM16: %u exact interleaved-byte/zero-frame cases passed\n", cases);
}
