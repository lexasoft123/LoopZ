// Opt-in microbenchmark of the exact cached FIR implementation. This measures
// computation on a prepared source window, not live audio or scheduler behavior.
#include "cached_fir.hpp"
#if defined(LOOPZ_BENCHMARK_WINDOWS_AUDIO_KERNELS)
#include "audio_kernels.hpp"
#endif
#include <algorithm>
#include <bit>
#include <string_view>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    const bool baseline = argc == 2 && std::string_view(argv[1]) == "--sse2";
    if (argc > 2 || (argc == 2 && !baseline && std::string_view(argv[1]) != "--auto"))
        throw std::invalid_argument("Use --auto or --sse2");
#if defined(LOOPZ_BENCHMARK_WINDOWS_AUDIO_KERNELS)
    if (!baseline)
        loopz::windows::prepareAudioKernels();
#endif
#if defined(__SSE2__) || defined(_M_X64)
    const char* implementation = "sse2";
#else
    const char* implementation = "portable";
#endif
    if (loopz::detail::cachedFirPairKernel())
        implementation = "avx2";
    const auto& kernel = loopz::SincKernel::prepared();
    for (const auto channels : {16u, 32u, 52u, 64u})
        for (const auto frames : {128u, 201u, 240u, 256u})
            for (const bool down : {false, true}) {
                const double sourceRate = down ? 48000 : 44100;
                const double outputRate = down ? 44100 : 48000;
                constexpr unsigned capacity = 2048;
                std::vector<float> source(size_t(channels) * capacity);
                std::vector<float> output(size_t(channels) * frames);
                std::array<float*, 64> planes{};
                for (unsigned c = 0; c < channels; ++c) {
                    planes[c] = output.data() + size_t(c) * frames;
                    for (unsigned i = 0; i < capacity; ++i)
                        source[size_t(c) * capacity + i] = float(0.1 * std::sin((i + c) * 0.03));
                }
                auto provider = [&](uint64_t position, std::span<float> values) noexcept {
                    if (position >= capacity)
                        return loopz::ResampleFrame{false, 0};
                    for (unsigned c = 0; c < channels; ++c)
                        values[c] = source[size_t(c) * capacity + position];
                    return loopz::ResampleFrame{true, channels};
                };
                std::array<double, 40> timings{};
                double checksum = 0;
                for (unsigned iteration = 0; iteration < 48; ++iteration) {
                    const auto begin = std::chrono::steady_clock::now();
                    const auto missing = loopz::CachedFir::read(
                        {0, 1024.125L, sourceRate, outputRate}, {{planes.data(), channels}, frames},
                        frames, kernel,
                        loopz::SincKernel::kCutoff * std::min(1.0, outputRate / sourceRate),
                        provider, {});
                    const auto elapsed = std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now() - begin)
                                             .count();
                    if (!missing || *missing)
                        throw std::runtime_error(
                            "Benchmark did not exercise a complete cached FIR read");
                    if (iteration >= 8)
                        timings[iteration - 8] = elapsed;
                    checksum += output.front() + output.back();
                }
                std::sort(timings.begin(), timings.end());
                uint64_t hash = 14695981039346656037ULL;
                for (const auto value : output) {
                    hash ^= std::bit_cast<uint32_t>(value);
                    hash *= 1099511628211ULL;
                }
                std::printf("{\"channels\":%u,\"frames\":%u,\"sourceRate\":%.0f,"
                            "\"outputRate\":%.0f,\"medianUs\":%.3f,\"maximumUs\":%.3f,"
                            "\"checksum\":%.12g,\"kernel\":\"%s\",\"outputHash\":\"%llu\"}\n",
                            channels, frames, sourceRate, outputRate,
                            (timings[19] + timings[20]) / 2, timings.back(), checksum,
                            implementation, static_cast<unsigned long long>(hash));
            }
}
