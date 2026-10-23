#include "../native/platform/windows/driver/packed_samples.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

using namespace loopz_driver;
int main() {
    constexpr unsigned count = 65536;
    std::vector<unsigned char> pcm(count * 2 + 2, 213), floating(count * 4 + 2, 177),
        restored(pcm.size(), 213), inplace(floating.size(), 177);
    for (unsigned i = 0; i < count; ++i) {
        const short value = static_cast<short>(static_cast<int>(i) - 32768);
        std::memcpy(pcm.data() + 1 + i * 2, &value, 2);
    }
    assert(convertPackedSamples(pcm.data() + 1, 2, count * 2, floating.data() + 1, 4, count * 4,
                                count));
    assert(floating.front() == 177 && floating.back() == 177);
    for (unsigned i = 0; i < count; ++i) {
        const float expected = static_cast<float>(static_cast<int>(i) - 32768) / 32768.0f;
        assert(readSampleBits(floating.data() + 1 + i * 4, 4) == std::bit_cast<unsigned>(expected));
    }
    assert(convertPackedSamples(floating.data() + 1, 4, count * 4, restored.data() + 1, 2,
                                count * 2, count));
    assert(restored == pcm);
    std::copy(pcm.begin() + 1, pcm.end() - 1, inplace.begin() + 1);
    assert(convertPackedSamples(inplace.data() + 1, 2, count * 2, inplace.data() + 1, 4, count * 4,
                                count));
    assert(inplace == floating);
    assert(convertPackedSamples(inplace.data() + 1, 4, count * 4, inplace.data() + 1, 2, count * 2,
                                count));
    assert(std::equal(pcm.begin() + 1, pcm.end() - 1, inplace.begin() + 1));

    // Independent IEEE reference, including nonfinite values and headroom.
    unsigned random = 123;
    for (unsigned i = 0; i < 1000000; ++i) {
        random = random * 1664525U + 1013904223U;
        unsigned char source[6]{211}, output[6]{233, 233, 233, 233, 233, 233};
        std::memcpy(source + 1, &random, 4);
        const float value = std::bit_cast<float>(random);
        const double scaled = static_cast<double>(value) * 32768.0;
        const short expected =
            !std::isfinite(value) ? 0 : static_cast<short>(std::clamp(scaled, -32768.0, 32767.0));
        assert(convertPackedSamples(source + 1, 4, 4, output + 1, 2, 2, 1));
        short actual;
        std::memcpy(&actual, output + 1, 2);
        assert(actual == expected && output[0] == 233 && output[3] == 233);
        assert(convertPackedSamples(source + 1, 4, 4, output + 1, 4, 4, 1));
        assert(readSampleBits(output + 1, 4) == (std::isfinite(value) ? random : 0));
        assert(output[0] == 233 && output[5] == 233);
    }
    // Full float packets exercise the hot transport path, independent of the
    // scalar helpers. Keep guards and original finite bits at every alignment.
    for (const unsigned samples : {0U, 1U, 2U, 3U, 17U, 122U, 2048U, 6588U, 7808U})
        for (const unsigned offset : {0U, 1U, 3U})
            for (const bool sameBuffer : {false, true}) {
                const auto prefix = 16 + offset, bytes = samples * 4;
                std::vector<unsigned char> input(prefix + bytes + 19, 173),
                    output(input.size(), 219);
                constexpr unsigned patterns[]{0,          0x80000000, 1,          0x80000001,
                                              0x3d4ccccd, 0xbd4ccccd, 0x7f7fffff, 0xff7fffff,
                                              0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345};
                for (unsigned n = 0; n < samples; ++n) {
                    const auto bits = patterns[n % (sizeof(patterns) / sizeof(patterns[0]))];
                    std::memcpy(input.data() + prefix + n * 4, &bits, 4);
                }
                const auto original = input;
                auto* destination = sameBuffer ? input.data() + prefix : output.data() + prefix;
                assert(convertPackedSamples(input.data() + prefix, 4, bytes, destination, 4, bytes,
                                            samples));
                const auto& result = sameBuffer ? input : output;
                const auto guard = sameBuffer ? 173 : 219;
                assert(std::all_of(result.begin(), result.begin() + prefix, [guard](auto value) {
                    return value == guard;
                }));
                assert(
                    std::all_of(result.begin() + prefix + bytes, result.end(), [guard](auto value) {
                        return value == guard;
                    }));
                for (unsigned n = 0; n < samples; ++n) {
                    unsigned actual, reference;
                    std::memcpy(&reference, original.data() + prefix + n * 4, 4);
                    if ((reference & 0x7f800000U) == 0x7f800000U)
                        reference = 0;
                    std::memcpy(&actual, result.data() + prefix + n * 4, 4);
                    assert(actual == reference);
                }
                if (!sameBuffer)
                    assert(input == original);
            }
    assert(convertPackedSamples(nullptr, 4, 0, nullptr, 4, 0, 0));
    const auto before = floating;
    assert(!convertPackedSamples(pcm.data(), 2, 1, floating.data(), 4, count * 4, 1));
    assert(!convertPackedSamples(pcm.data(), 2, 2, floating.data(), 4, 3, 1));
    assert(!convertPackedSamples(pcm.data(), 3, 3, floating.data(), 4, 4, 1));
    assert(!convertPackedSamples(nullptr, 2, 2, floating.data(), 4, 4, 1));
    assert(!convertPackedSamples(pcm.data(), 2, 2, nullptr, 4, 4, 1));
    assert(!convertPackedSamples(floating.data(), 4, count * 4 - 1, floating.data(), 4, count * 4,
                                 count));
    assert(!convertPackedSamples(floating.data(), 4, count * 4, floating.data(), 4, count * 4 - 1,
                                 count));
    assert(!convertPackedSamples(nullptr, 4, 4, floating.data(), 4, 4, 1));
    assert(!convertPackedSamples(floating.data(), 4, 4, nullptr, 4, 4, 1));
    assert(floating == before);
    assert(convertPackedSamples(nullptr, 2, 0, nullptr, 4, 0, 0));
}
