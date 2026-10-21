#include <loopz/audio_scene.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace loopz;
constexpr uint32_t channels = 64, frames = 64;
class Source final : public AudioSource {
  public:
    Source(bool online, float value) : online_(online), value_(value) {}
    uint32_t channels() const noexcept override {
        return ::channels;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t count,
                         AudioReadCursor&) const noexcept override {
        for (uint32_t channel = 0; channel < ::channels; ++channel)
            std::fill_n(output.channels[channel], count, value_ * (channel + 1));
        return {online_ ? 0U : count, online_};
    }

  private:
    bool online_;
    float value_;
};
class Sink final : public AudioSink {
  public:
    std::array<std::array<float, frames>, ::channels> samples{};
    uint32_t channels() const noexcept override {
        return ::channels;
    }
    bool requested() const noexcept override {
        return true;
    }
    void idle() noexcept override {}
    bool publish(uint64_t, uint32_t, RoutingInput input, uint32_t count) noexcept override {
        for (uint32_t channel = 0; channel < ::channels; ++channel)
            std::copy_n(input.channels[channel], count, samples[channel].begin());
        return true;
    }
};
void measure(bool offline) {
    RouteDocument document;
    std::array<std::shared_ptr<Sink>, kMaximumVirtualDevices> sinks;
    auto live = std::make_shared<Source>(true, .001f);
    auto silent = std::make_shared<Source>(!offline, 0);
    for (uint32_t index = 0; index < kMaximumVirtualDevices; ++index) {
        VirtualDeviceDefinition device;
        device.id = std::to_string(index);
        device.name = device.id;
        device.routing.outputChannels = channels;
        device.routing.sources = {{"pass", channels, index ? 0.f : 1.f, true, false,
                                   defaultChannelConnections(channels, channels)}};
        device.bindings = {{"pass", SourceKind::PassThrough, ""}};
        if (index) {
            device.routing.sources.push_back(
                {"link", channels, 1, true, false, defaultChannelConnections(channels, channels)});
            device.bindings.push_back({"link", SourceKind::VirtualDevice, "0"});
        }
        document.devices.push_back(std::move(device));
        sinks[index] = std::make_shared<Sink>();
    }
    AudioScene scene(
        document, {48000, frames, 192, 64},
        [&](const auto& device, const auto&, const auto&) {
            return ResolvedAudioSource{device.id == "0" ? live : silent, {}};
        },
        [&](const auto& device) {
            return sinks.at(std::stoul(device.id));
        });
    constexpr uint32_t warmup = 64, measured = 500;
    std::array<double, measured> times{};
    for (uint32_t block = 0; block < warmup + measured; ++block) {
        const auto begun = std::chrono::steady_clock::now();
        const auto result = scene.render(4800000ULL + uint64_t(block) * frames, frames);
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begun)
                .count();
        if (!result.validBlock || result.activeDevices != kMaximumVirtualDevices ||
            result.failedGraphs || result.failedSinks)
            throw std::runtime_error("Benchmark graph did not render every device");
        for (uint32_t device = 0; device < kMaximumVirtualDevices; ++device) {
            AudioMeterSnapshot source, output;
            if (!scene.sourceMeters(device, 0, source) || source.frames != frames ||
                !scene.deviceMeters(device, output) || output.frames != frames)
                throw std::runtime_error("Benchmark meter lost its valid block");
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const auto expected = .001f * (channel + 1);
                const auto sourceExpected = device ? 0.f : expected;
                if (std::fabs(source.peak[channel] - sourceExpected) > .00001f ||
                    std::fabs(source.rms[channel] - sourceExpected) > .00001f ||
                    std::fabs(output.peak[channel] - expected) > .00001f ||
                    std::fabs(output.rms[channel] - expected) > .00001f)
                    throw std::runtime_error("Benchmark meter changed");
                for (const auto sample : sinks[device]->samples[channel])
                    if (std::fabs(sample - expected) > .00001f)
                        throw std::runtime_error("Benchmark output changed");
            }
        }
        if (block >= warmup)
            times[block - warmup] = elapsed;
    }
    std::sort(times.begin(), times.end());
    std::printf("{\"scenario\":\"%s\",\"devices\":32,\"channels\":64,\"blockFrames\":64,"
                "\"sampleRate\":48000,\"medianMicroseconds\":%.3f,\"p99Microseconds\":%.3f,"
                "\"maximumMicroseconds\":%.3f,\"allSamplesAndMetersVerified\":true}\n",
                offline ? "offline-pass-through" : "online-silent-pass-through", times[250],
                times[494], times.back());
}
} // namespace
int main() {
    try {
        measure(true);
        measure(false);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
