// Offline integer codec measurement only. No devices, kernel load or live audio.
#include "windows_packed_samples_benchmark_api.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

int main() {
    for (const auto count : {64U, 1024U, 3072U, 6588U, 7808U})
        for (const auto offset : {0U, 1U})
            for (const bool inplace : {false, true})
                for (const bool invalid : {false, true}) {
                    const auto bytes = count * 4, prefix = 16 + offset;
                    std::vector<unsigned char> source(bytes + 48, 173), output(source.size(), 219);
                    std::vector<unsigned> expected(count);
                    for (unsigned n = 0; n < count; ++n) {
                        unsigned bits = 0x3c000000U + (n * 7919U % 0x1000000U);
                        if (n % 2)
                            bits |= 0x80000000U;
                        if (invalid && n % 31 == 0)
                            bits = 0x7f800000U | n;
                        if (n % 127 == 1)
                            bits = 0x80000000U;
                        if (n % 127 == 2)
                            bits = 1;
                        expected[n] = (bits & 0x7f800000U) == 0x7f800000U ? 0 : bits;
                        std::memcpy(source.data() + prefix + n * 4, &bits, 4);
                    }
                    const auto original = source;
                    std::array<double, 40> timings{};
                    for (unsigned iteration = 0; iteration < 48; ++iteration) {
                        if (inplace)
                            source = original;
                        auto* destination =
                            inplace ? source.data() + prefix : output.data() + prefix;
                        const auto start = std::chrono::steady_clock::now();
                        for (unsigned repeat = 0; repeat < 8; ++repeat)
                            if (!loopzPackedConvert(source.data() + prefix, 4, bytes, destination,
                                                    4, bytes, count))
                                throw std::runtime_error("Codec benchmark rejected valid buffers");
                        const auto elapsed = std::chrono::duration<double, std::micro>(
                                                 std::chrono::steady_clock::now() - start)
                                                 .count() /
                                             8;
                        if (iteration >= 8)
                            timings[iteration - 8] = elapsed;
                    }
                    const auto& result = inplace ? source : output;
                    const auto guard = inplace ? 173 : 219;
                    if (!std::all_of(result.begin(), result.begin() + prefix,
                                     [guard](auto value) {
                                         return value == guard;
                                     }) ||
                        !std::all_of(result.begin() + prefix + bytes, result.end(),
                                     [guard](auto value) {
                                         return value == guard;
                                     }))
                        throw std::runtime_error("Codec benchmark changed a buffer guard");
                    unsigned long long hash = 14695981039346656037ULL;
                    for (unsigned n = 0; n < count; ++n) {
                        unsigned bits;
                        std::memcpy(&bits, result.data() + prefix + n * 4, 4);
                        if (bits != expected[n])
                            throw std::runtime_error(
                                "Codec benchmark output differs from IEEE reference");
                        hash ^= bits;
                        hash *= 1099511628211ULL;
                    }
                    std::sort(timings.begin(), timings.end());
                    std::printf("{\"samples\":%u,\"offset\":%u,\"inplace\":%s,\"invalid\":%s,"
                                "\"medianUs\":%.6f,\"maximumUs\":%.6f,\"outputHash\":\"%llu\"}\n",
                                count, offset, inplace ? "true" : "false",
                                invalid ? "true" : "false", (timings[19] + timings[20]) / 2,
                                timings.back(), hash);
                }
}
