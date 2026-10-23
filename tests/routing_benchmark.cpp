// Opt-in offline graph throughput measurement. No devices or live audio.
#include <loopz/routing.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

int main() {
    using namespace loopz;
    for (const auto channels : {2U, 16U, 32U, 54U, 64U})
        for (const auto frames : {128U, 256U})
            for (const auto mode : {0U, 1U, 2U}) {
                RoutingConfiguration configuration;
                configuration.outputChannels = channels;
                configuration.gain = .5f;
                const auto sources = mode == 2 ? 2U : 1U;
                for (unsigned i = 0; i < sources; ++i) {
                    SourceRouting source;
                    source.id = std::to_string(i);
                    source.channels = channels;
                    for (unsigned channel = 0; channel < channels; ++channel)
                        source.connections.push_back(
                            {channel, mode == 1 ? (channel + 1) % channels : channel});
                    configuration.sources.push_back(std::move(source));
                }
                RoutingGraph graph(88200, configuration);
                std::vector<float> input(size_t(channels) * frames), output(input.size());
                std::array<const float*, 64> read{};
                std::array<float*, 64> write{};
                for (unsigned channel = 0; channel < channels; ++channel) {
                    read[channel] = input.data() + size_t(channel) * frames;
                    write[channel] = output.data() + size_t(channel) * frames;
                    for (unsigned frame = 0; frame < frames; ++frame)
                        input[size_t(channel) * frames + frame] =
                            float(.05 * std::sin((frame + channel) * .13));
                }
                const std::array<RoutingInput, 2> inputs{
                    RoutingInput{{read.data(), channels}, frames},
                    RoutingInput{{read.data(), channels}, frames}};
                std::array<double, 40> timings{};
                for (unsigned iteration = 0; iteration < 48; ++iteration) {
                    const auto start = std::chrono::steady_clock::now();
                    // Amortize clock reads without changing the configured quantum.
                    for (unsigned block = 0; block < 8; ++block)
                        if (!graph.render({inputs.data(), sources},
                                          {{write.data(), channels}, frames}, frames,
                                          uint64_t(iteration * 8 + block) * frames))
                            throw std::runtime_error("Offline graph render failed");
                    const auto elapsed = std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now() - start)
                                             .count() /
                                         8;
                    if (iteration >= 8)
                        timings[iteration - 8] = elapsed;
                }
                std::sort(timings.begin(), timings.end());
                uint64_t hash = 14695981039346656037ULL;
                for (const auto value : output) {
                    hash ^= std::bit_cast<uint32_t>(value);
                    hash *= 1099511628211ULL;
                }
                std::printf("{\"channels\":%u,\"frames\":%u,\"mode\":%u,\"medianUs\":%.3f,"
                            "\"maximumUs\":%.3f,\"outputHash\":\"%llu\"}\n",
                            channels, frames, mode, (timings[19] + timings[20]) / 2, timings.back(),
                            static_cast<unsigned long long>(hash));
            }
}
