// Offline, opt-in benchmark. Preparation and result serialization are outside
// the timed read; this does not measure installed audio or scheduling.
#include "unity_convolution.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>

int main() {
    const auto& plan = loopz::UnityConvolutionPlan::prepared();
    const auto& kernel = loopz::SincKernel::prepared();
    for (unsigned channels : {1u, 2u, 7u, 16u, 32u, 64u})
        for (unsigned frames : {128u, 201u, 240u, 256u})
            for (double phase : {.00021, .37, .99979}) {
                constexpr unsigned capacity = 1024;
                std::vector<float> input(size_t(capacity) * channels);
                std::vector<float> output(size_t(frames) * channels);
                std::array<float*, 64> planes{};
                for (unsigned c = 0; c < channels; ++c) {
                    planes[c] = output.data() + size_t(c) * frames;
                    for (unsigned f = 0; f < capacity; ++f)
                        input[size_t(f) * channels + c] =
                            float(.05 * std::sin(f * (.031 + c * .007)));
                }
                auto provider = [&](uint64_t position, std::span<float> values) noexcept {
                    if (position >= capacity)
                        return loopz::ResampleFrame{false, 0};
                    std::copy_n(input.data() + size_t(position) * channels, channels,
                                values.data());
                    return loopz::ResampleFrame{true, channels};
                };
                std::array<double, 32> times{};
                for (unsigned iteration = 0; iteration < 40; ++iteration) {
                    const auto begin = std::chrono::steady_clock::now();
                    const auto missing = plan.read(256, phase, {{planes.data(), channels}, frames},
                                                   frames, kernel, provider, {});
                    const auto elapsed = std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now() - begin)
                                             .count();
                    if (missing)
                        throw std::runtime_error("Benchmark source window incomplete");
                    if (iteration >= 8)
                        times[iteration - 8] = elapsed;
                }
                std::sort(times.begin(), times.end());
                uint64_t hash = 14695981039346656037ULL;
                for (auto value : output) {
                    if (!std::isfinite(value))
                        throw std::runtime_error("Benchmark output nonfinite");
                    hash ^= std::bit_cast<uint32_t>(value);
                    hash *= 1099511628211ULL;
                }
                std::printf("{\"channels\":%u,\"frames\":%u,\"phase\":%.8f,"
                            "\"medianUs\":%.3f,\"hash\":\"%llu\"}\n",
                            channels, frames, phase, (times[15] + times[16]) / 2,
                            static_cast<unsigned long long>(hash));
            }
}
