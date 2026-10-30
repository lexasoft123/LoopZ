// Silent offline workloads shared by the baseline and staged candidate builds.
#include <loopz/routing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
bool benchmark(uint32_t channels, uint32_t frames, bool limiter) {
    loopz::RoutingConfiguration configuration;
    configuration.outputChannels = channels;
    configuration.safetyLimiter = limiter;
    for (const auto* name : {"first", "second"}) {
        loopz::SourceRouting source;
        source.id = name;
        source.channels = channels;
        source.connections = loopz::defaultChannelConnections(channels, channels);
        configuration.sources.push_back(std::move(source));
    }
    loopz::RoutingGraph graph(48000, configuration);
    std::vector<float> input(size_t(channels) * frames), output(input.size());
    std::array<const float*, 64> read{};
    std::array<float*, 64> write{};
    for (uint32_t channel = 0; channel < channels; ++channel) {
        read[channel] = input.data() + size_t(channel) * frames;
        write[channel] = output.data() + size_t(channel) * frames;
        for (uint32_t frame = 0; frame < frames; ++frame)
            input[size_t(channel) * frames + frame] =
                float(int((channel + frame) % 13) - 6) * 0.25f;
    }
    const std::array<loopz::RoutingInput, 2> sources{
        loopz::RoutingInput{std::span(read.data(), channels), frames},
        loopz::RoutingInput{std::span(read.data(), channels), frames}};
    constexpr uint32_t warmup = 8, batches = 32, iterations = 64;
    std::array<double, batches> timings{};
    uint64_t position = 0;
    for (uint32_t batch = 0; batch < warmup + batches; ++batch) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
            if (!graph.render(sources, {std::span(write.data(), channels), frames}, frames,
                              position))
                return false;
            position += frames;
        }
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
        // Verification is outside the timed batch and cannot be compiled out.
        for (size_t i = 0; i < input.size(); ++i) {
            float expected = input[i] * 2;
            if (limiter)
                expected = std::clamp(expected, -0.98f, 0.98f);
            if (!std::isfinite(output[i]) || std::abs(output[i] - expected) > 1e-6f)
                return false;
        }
        if (batch >= warmup)
            timings[batch - warmup] = elapsed / iterations;
    }
    std::sort(timings.begin(), timings.end());
    uint64_t hash = 14695981039346656037ULL;
    for (float value : output) {
        hash ^= std::bit_cast<uint32_t>(value);
        hash *= 1099511628211ULL;
    }
    std::printf("{\"channels\":%u,\"frames\":%u,\"limiter\":%s,\"verified\":true,"
                "\"medianUs\":%.6f,\"hash\":\"%llu\"}\n",
                channels, frames, limiter ? "true" : "false", (timings[15] + timings[16]) / 2,
                static_cast<unsigned long long>(hash));
    return true;
}
} // namespace

int main() {
    for (bool limiter : {false, true})
        for (uint32_t channels : {2U, 64U})
            for (uint32_t frames : {32U, 64U, 256U})
                if (!benchmark(channels, frames, limiter)) {
                    std::fprintf(stderr, "Performance workload failed audio-output validation\n");
                    return 1;
                }
}
