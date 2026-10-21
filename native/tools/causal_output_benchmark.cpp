#include <loopz/causal_resampler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace loopz;
void measure(uint32_t channels, uint32_t frames) {
    CausalResampler converter(channels, 48000, 48000);
    std::array<std::array<float, kBlock>, kMaximumRoutingChannels> input{};
    std::array<const float*, kMaximumRoutingChannels> planes{};
    for (uint32_t channel = 0; channel < channels; ++channel) {
        input[channel].fill((channel + 1) * .01f);
        planes[channel] = input[channel].data();
    }
    constexpr uint32_t warmup = 64, measured = 1000;
    std::array<double, measured> times{};
    for (uint32_t block = 0; block < warmup + measured; ++block) {
        const auto first = 4800000ULL + uint64_t(block) * frames;
        const auto begun = std::chrono::steady_clock::now();
        const auto output = converter.convert(first, {{planes.data(), channels}, kBlock}, frames);
        const auto us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begun)
                .count();
        if (!output.valid || output.firstFrame != first || output.frames != frames)
            throw std::runtime_error("Invalid same-rate benchmark output");
        for (uint32_t frame = 0; frame < frames; ++frame)
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const auto expected = block == 0 && frame == 0 ? 0.f : input[channel][0];
                if (output.interleaved[frame * channels + channel] != expected)
                    throw std::runtime_error(
                        "Same-rate benchmark changed the declared sample delay");
            }
        if (block >= warmup)
            times[block - warmup] = us;
    }
    std::sort(times.begin(), times.end());
    std::printf("{\"channels\":%u,\"blockFrames\":%u,\"sampleRate\":48000,"
                "\"delayFrames\":1,\"medianMicroseconds\":%.3f,\"p99Microseconds\":%.3f,"
                "\"maximumMicroseconds\":%.3f,\"allSamplesVerified\":true}\n",
                channels, frames, times[500], times[989], times.back());
}
} // namespace
int main() {
    try {
        for (const auto channels : {2U, 8U, 64U})
            for (const auto frames : {32U, 64U})
                measure(channels, frames);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
