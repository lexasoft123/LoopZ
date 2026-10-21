#include <loopz/audio_history.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>

namespace {
using namespace loopz;
constexpr uint32_t callbackFrames = 32;
constexpr uint32_t historyFrames = 16384;
constexpr uint32_t warmupReads = 64;
constexpr uint32_t measuredReads = 1000;
constexpr uint64_t originNs = 1000000000;
constexpr double nominalRate = 48000;
constexpr double physicalRate = nominalRate * 1.0002;

void measure(ResamplingFilter filter, uint32_t channels, uint32_t frames, double outputRate,
             bool corrections) {
    AudioHistory history(channels, AudioHistory::kDefaultCapacityFrames, filter);
    std::array<std::array<float, callbackFrames>, kMaximumRoutingChannels> input{};
    std::array<const float*, kMaximumRoutingChannels> inputPlanes{};
    for (uint32_t channel = 0; channel < channels; ++channel)
        inputPlanes[channel] = input[channel].data();
    for (uint32_t first = 0; first < historyFrames; first += callbackFrames) {
        for (uint32_t channel = 0; channel < channels; ++channel)
            for (uint32_t frame = 0; frame < callbackFrames; ++frame)
                input[channel][frame] = static_cast<float>(
                    .1 * std::sin(2 * std::numbers::pi_v<double> * (997 + channel * 37) *
                                  (first + frame) / nominalRate));
        const auto offset =
            corrections && (first / (16 * callbackFrames)) % 2 ? frameNs(1, physicalRate) : 0;
        if (!history.append({{inputPlanes.data(), channels}, callbackFrames}, callbackFrames,
                            originNs + frameNs(first, physicalRate) + offset, nominalRate, first,
                            true))
            throw std::runtime_error("Benchmark history append failed");
    }
    std::array<std::array<float, 64>, kMaximumRoutingChannels> output{};
    std::array<float*, kMaximumRoutingChannels> outputPlanes{};
    for (uint32_t channel = 0; channel < channels; ++channel)
        outputPlanes[channel] = output[channel].data();
    std::array<double, measuredReads> times{};
    AudioReadCursor cursor;
    double checksum = 0;
    for (uint32_t index = 0; index < warmupReads + measuredReads; ++index) {
        // Cycle retained physical timestamps through many callback boundaries.
        // A fractional start keeps both modes on their actual filter path.
        const auto first = 4096 + (index % 256) * callbackFrames;
        const auto requested = originNs + frameNs(first, physicalRate) + 5000;
        const auto begun = std::chrono::steady_clock::now();
        const auto result = history.read(requested, outputRate,
                                         {{outputPlanes.data(), channels}, frames}, frames, cursor);
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begun)
                .count();
        if (!result.online || result.missingFrames)
            throw std::runtime_error("Benchmark requested unavailable history");
        checksum += output[0][0];
        if (index >= warmupReads)
            times[index - warmupReads] = elapsed;
    }
    if (!std::isfinite(checksum))
        throw std::runtime_error("Benchmark produced nonfinite output");
    std::sort(times.begin(), times.end());
    const auto budget = frames * 1e6 / outputRate;
    const auto overBudget = std::count_if(times.begin(), times.end(), [budget](double value) {
        return value > budget;
    });
    std::printf("{\"mode\":\"%s\",\"channels\":%u,\"blockFrames\":%u,"
                "\"nominalSourceRate\":48000,\"sourceClockPpm\":200,\"outputRate\":%.0f,"
                "\"oneSampleTimestampCorrections\":%s,\"measuredReads\":%u,"
                "\"medianMicroseconds\":%.3f,\"p95Microseconds\":%.3f,"
                "\"p99Microseconds\":%.3f,\"maximumMicroseconds\":%.3f,"
                "\"blockBudgetMicroseconds\":%.3f,\"readsOverBlockBudget\":%zu,"
                "\"missingFrames\":0,\"checksum\":%.8f}\n",
                filter == ResamplingFilter::MinimumPhase ? "minimum-phase" : "linear-phase",
                channels, frames, outputRate, corrections ? "true" : "false", measuredReads,
                times[measuredReads / 2], times[949], times[989], times.back(), budget,
                static_cast<size_t>(overBudget), checksum);
    std::fflush(stdout);
}
} // namespace

int main() {
    try {
        for (const auto filter : {ResamplingFilter::LinearPhase, ResamplingFilter::MinimumPhase})
            for (const auto channels : {2U, 16U, 64U})
                for (const auto frames : {32U, 64U})
                    for (const auto rate : {48000.0, 44100.0})
                        for (const auto corrections : {false, true})
                            measure(filter, channels, frames, rate, corrections);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
