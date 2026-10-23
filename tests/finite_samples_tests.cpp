#include "../native/core/finite_samples.hpp"
#include <algorithm>
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
LOOPZ_NO_INLINE void scalar(float* values, uint32_t frames) noexcept {
    for (uint32_t frame = 0; frame < frames; ++frame)
        if (!std::isfinite(values[frame])) values[frame] = 0;
}
LOOPZ_NO_INLINE void optimized(float* values, uint32_t frames) noexcept {
    loopz::detail::sanitizeFinite(values, frames);
}
int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--benchmark" ||
                      std::string_view(argv[1]) == "--benchmark-reference")) {
        std::array<float, 64 * 256> values{};
        for (unsigned i = 0; i < values.size(); ++i)
            values[i] = .05f * std::sin(static_cast<float>(i) * .017f);
        const auto operation = std::string_view(argv[1]) == "--benchmark" ? optimized : scalar;
        for (unsigned i = 0; i < 100; ++i) operation(values.data(), static_cast<uint32_t>(values.size()));
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 4000; ++i) operation(values.data(), static_cast<uint32_t>(values.size()));
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        assert(values[1] != 0);
        std::printf("{\"frames\":16384,\"calls\":4000,\"nsPerCall\":%.1f}\n",
            std::chrono::duration<double, std::nano>(elapsed).count() / 4000);
        return 0;
    }
    const std::array<uint32_t, 16> patterns{0, 0x80000000, 0x3d4ccccd, 0xbd4ccccd,
        0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345, 1, 0x80000001,
        0x7f7fffff, 0xff7fffff, 0x00800000, 0x80800000, 0x3f800000, 0xbf800000};
    unsigned cases = 0;
    for (uint32_t frames : {0U, 1U, 2U, 3U, 4U, 7U, 16U, 63U, 128U, 255U, 256U, 1025U, 4097U})
        for (uint32_t offset : {0U, 1U, 3U})
            for (unsigned phase = 0; phase < patterns.size(); ++phase) {
                std::vector<float> actual(frames + offset + 8), expected(actual.size());
                for (size_t i = 0; i < actual.size(); ++i)
                    actual[i] = expected[i] = std::bit_cast<float>(patterns[(i + phase) % patterns.size()]);
                for (size_t i = 0; i < actual.size(); ++i) {
                    const auto value = actual[i];
                    assert(loopz::detail::finiteSampleBits(value) ==
                           std::bit_cast<uint32_t>(std::isfinite(value) ? value : 0.0f));
                }
                scalar(expected.data() + offset, frames);
                optimized(actual.data() + offset, frames);
                for (size_t i = 0; i < actual.size(); ++i)
                    assert(std::bit_cast<uint32_t>(actual[i]) == std::bit_cast<uint32_t>(expected[i]));
                ++cases;
            }
    // Include arbitrary float encodings, not just hand-picked exceptional values.
    std::array<float, 257> actual{}, expected{};
    uint32_t bits = 0x12345678;
    for (unsigned block = 0; block < 256; ++block) {
        for (unsigned i = 0; i < actual.size(); ++i) {
            bits = bits * 1664525U + 1013904223U;
            actual[i] = expected[i] = std::bit_cast<float>(bits);
        }
        for (const auto value : actual)
            assert(loopz::detail::finiteSampleBits(value) ==
                   std::bit_cast<uint32_t>(std::isfinite(value) ? value : 0.0f));
        scalar(expected.data(), static_cast<uint32_t>(expected.size()));
        optimized(actual.data(), static_cast<uint32_t>(actual.size()));
        for (unsigned i = 0; i < actual.size(); ++i)
            assert(std::bit_cast<uint32_t>(actual[i]) == std::bit_cast<uint32_t>(expected[i]));
    }
    std::printf("Finite containment: %u exact cases and 65,792 arbitrary encodings passed\n", cases);
}
