#include <loopz/routing.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

int main() {
    constexpr uint32_t frames = 64;
    constexpr uint32_t warmupBlocks = 10;
    constexpr uint32_t measuredBlocks = 100;
    struct Case {
        const char* name;
        uint32_t count, channels;
        bool silentPeer;
    };
    for (const auto [name, count, channels, silentPeer] :
         {Case{"mix", 3, 2, false}, Case{"mix", 16, 64, false}, Case{"mix", 32, 64, false},
          Case{"live-plus-silent", 2, 64, true}}) {
        loopz::RoutingConfiguration configuration;
        configuration.outputChannels = channels;
        for (uint32_t index = 0; index < count; ++index)
            configuration.sources.push_back(
                {std::to_string(index), channels, silentPeer ? (index == 0 ? 1.0f : 0.0f) : 0.01f,
                 true, false, loopz::defaultChannelConnections(channels, channels)});
        loopz::RoutingGraph graph(48000, configuration);
        std::array<std::array<float, frames>, 64> input{}, output{};
        std::array<const float*, 64> inputChannels{};
        std::array<float*, 64> outputChannels{};
        for (uint32_t channel = 0; channel < channels; ++channel) {
            input[channel].fill(0.1f);
            inputChannels[channel] = input[channel].data();
            outputChannels[channel] = output[channel].data();
        }
        std::vector<loopz::RoutingInput> sources(count, {{inputChannels.data(), channels}, frames});
        std::array<double, measuredBlocks> times{};
        for (uint32_t block = 0; block < warmupBlocks + measuredBlocks; ++block) {
            const auto start = std::chrono::steady_clock::now();
            if (!graph.render(sources, {{outputChannels.data(), channels}, frames}, frames,
                              block * frames))
                return 1;
            const auto elapsed =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                    .count();
            const auto expected = silentPeer ? 0.1f : count * 0.001f;
            for (uint32_t channel = 0; channel < channels; ++channel)
                for (uint32_t frame = 0; frame < frames; ++frame)
                    if (std::fabs(output[channel][frame] - expected) > 0.00001f)
                        return 1;
            if (block >= warmupBlocks)
                times[block - warmupBlocks] = elapsed;
        }
        std::sort(times.begin(), times.end());
        std::printf("{\"scenario\":\"%s\",\"sources\":%u,\"channels\":%u,\"blockFrames\":64,"
                    "\"medianMicroseconds\":%.3f,\"p99Microseconds\":%.3f,"
                    "\"maximumMicroseconds\":%.3f}\n",
                    name, count, channels, times[50], times[98], times[99]);
    }
}
