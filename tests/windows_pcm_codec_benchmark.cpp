// Offline mixed-width codec measurement. No devices, kernel load or live audio.
#include "windows_packed_samples_benchmark_api.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
unsigned reference(unsigned bits, unsigned sourceBytes, unsigned destinationBytes) {
    float value;
    if (sourceBytes == 2) {
        const auto sample = bits < 32768 ? static_cast<int>(bits) : static_cast<int>(bits) - 65536;
        value = static_cast<float>(sample) / 32768.0f;
    } else {
        value = std::bit_cast<float>(bits);
    }
    if (destinationBytes == 4)
        return !std::isfinite(value) ? 0 : std::bit_cast<unsigned>(value);
    const auto result =
        !std::isfinite(value)
            ? 0
            : static_cast<int>(std::clamp(double(value) * 32768, -32768.0, 32767.0));
    return static_cast<unsigned short>(result);
}
void measure(unsigned count, unsigned offset, bool inplace, bool edgeCases, unsigned sourceBytes,
             unsigned destinationBytes) {
    const auto inputBytes = count * sourceBytes, outputBytes = count * destinationBytes;
    const auto prefix = 16 + offset, capacity = count * 4;
    std::vector<unsigned char> source(prefix + capacity + 32, 173), output(source.size(), 219);
    std::vector<unsigned> expected(count);
    constexpr unsigned edges[]{0,          0x80000000, 1,          0x80000001,
                               0x3d4ccccd, 0xbd4ccccd, 0x7f7fffff, 0xff7fffff,
                               0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345};
    for (unsigned n = 0; n < count; ++n) {
        unsigned bits =
            sourceBytes == 2 ? n * 7919U % 65536U : 0x3c000000U + (n * 7919U % 0x1000000U);
        if (sourceBytes == 4 && n % 2)
            bits |= 0x80000000U;
        if (edgeCases && n % 7 == 0)
            bits = sourceBytes == 2 ? (n % 3 == 0   ? 0
                                       : n % 3 == 1 ? 32768U
                                                    : 32767U)
                                    : edges[(n / 7) % std::size(edges)];
        expected[n] = reference(bits, sourceBytes, destinationBytes);
        for (unsigned byte = 0; byte < sourceBytes; ++byte)
            source[prefix + n * sourceBytes + byte] =
                static_cast<unsigned char>(bits >> (byte * 8));
    }
    const auto original = source;
    std::array<double, 40> timings{};
    for (unsigned iteration = 0; iteration < 48; ++iteration) {
        if (inplace)
            source = original;
        auto* destination = inplace ? source.data() + prefix : output.data() + prefix;
        const auto start = std::chrono::steady_clock::now();
        const bool accepted = loopzPackedConvert(source.data() + prefix, sourceBytes, inputBytes,
                                                 destination, destinationBytes, outputBytes, count);
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
        if (!accepted)
            throw std::runtime_error("Codec benchmark rejected valid buffers");
        if (iteration >= 8)
            timings[iteration - 8] = elapsed;
    }
    const auto& result = inplace ? source : output;
    const auto untouched = inplace ? original : std::vector<unsigned char>(source.size(), 219);
    if (!std::equal(result.begin(), result.begin() + prefix, untouched.begin()) ||
        !std::equal(result.begin() + prefix + outputBytes, result.end(),
                    untouched.begin() + prefix + outputBytes) ||
        (!inplace && source != original))
        throw std::runtime_error("Codec benchmark changed bytes outside its output");
    unsigned long long hash = 14695981039346656037ULL;
    for (unsigned n = 0; n < count; ++n) {
        unsigned bits = 0;
        for (unsigned byte = 0; byte < destinationBytes; ++byte)
            bits |= static_cast<unsigned>(result[prefix + n * destinationBytes + byte])
                    << (byte * 8);
        if (bits != expected[n])
            throw std::runtime_error("Codec benchmark output differs from independent reference");
        hash ^= bits;
        hash *= 1099511628211ULL;
    }
    std::sort(timings.begin(), timings.end());
    std::printf("{\"samples\":%u,\"sourceBytes\":%u,\"destinationBytes\":%u,\"offset\":%u,"
                "\"inplace\":%s,\"edgeCases\":%s,\"medianUs\":%.6f,\"maximumUs\":%.6f,"
                "\"outputHash\":\"%llu\"}\n",
                count, sourceBytes, destinationBytes, offset, inplace ? "true" : "false",
                edgeCases ? "true" : "false", (timings[19] + timings[20]) / 2, timings.back(),
                hash);
}
} // namespace

int main() {
    for (const auto sourceBytes : {2U, 4U})
        for (const auto destinationBytes : {2U, 4U})
            for (const auto count : {64U, 1024U, 3072U, 6588U, 7808U, 15360U})
                for (const auto offset : {0U, 1U})
                    for (const bool inplace : {false, true})
                        for (const bool edgeCases : {false, true})
                            measure(count, offset, inplace, edgeCases, sourceBytes,
                                    destinationBytes);
}
