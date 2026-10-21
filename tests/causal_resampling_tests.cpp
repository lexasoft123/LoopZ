#include <loopz/causal_resampler.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>

namespace {
thread_local bool trackHeap = false;
thread_local unsigned heapEvents = 0;
} // namespace
void* operator new(size_t bytes) {
    if (trackHeap)
        ++heapEvents;
    if (auto* result = std::malloc(bytes ? bytes : 1))
        return result;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) {
    return ::operator new(bytes);
}
void operator delete(void* value) noexcept {
    if (trackHeap && value)
        ++heapEvents;
    std::free(value);
}
void operator delete[](void* value) noexcept {
    ::operator delete(value);
}
void operator delete(void* value, size_t) noexcept {
    ::operator delete(value);
}
void operator delete[](void* value, size_t) noexcept {
    ::operator delete(value);
}

namespace {
using namespace loopz;
struct Input {
    std::vector<float> data;
    std::array<const float*, 64> planes{};
    uint32_t channels;
    explicit Input(uint32_t count) : data(count * kBlock), channels(count) {
        for (uint32_t channel = 0; channel < count; ++channel)
            planes[channel] = data.data() + channel * kBlock;
    }
    float& at(uint32_t channel, uint32_t frame) {
        return data[channel * kBlock + frame];
    }
    RoutingInput view() const {
        return {{planes.data(), channels}, kBlock};
    }
};
template <typename Operation> auto realtime(Operation operation) {
    heapEvents = 0;
    trackHeap = true;
    auto result = operation();
    trackHeap = false;
    assert(heapEvents == 0);
    return result;
}
uint64_t ceiling(uint64_t value, uint32_t to, uint32_t from) {
    return value / from * to + ((value % from) * to + from - 1) / from;
}
double coordinate(uint64_t outputFrame, uint32_t sourceRate, uint32_t outputRate, uint64_t origin,
                  uint32_t delay) {
    const auto product = (outputFrame % outputRate) * sourceRate;
    const auto integral = (outputFrame / outputRate) * sourceRate + product / outputRate;
    return (integral >= origin ? double(integral - origin) : -double(origin - integral)) +
           double(product % outputRate) / outputRate - delay;
}
double tone(double frame, uint32_t rate, double frequency) {
    return 0.25 * std::sin(2 * std::numbers::pi_v<double> * frequency * frame / rate);
}
struct Signal {
    uint64_t first;
    std::vector<float> samples;
};
Signal render(uint32_t channels, uint32_t sourceRate, uint32_t outputRate, uint32_t total,
              uint32_t partition, double frequency, bool impulse = false, uint64_t origin = 0) {
    CausalResampler converter(channels, sourceRate, outputRate);
    Input input(channels);
    Signal signal{ceiling(origin, outputRate, sourceRate), {}};
    uint64_t expected = signal.first;
    constexpr std::array<uint32_t, 7> varied{1, 17, 64, 3, 128, 11, 256};
    uint32_t part = 0;
    for (uint32_t offset = 0; offset < total;) {
        const auto frames =
            std::min(total - offset, partition ? partition : varied[part++ % varied.size()]);
        for (uint32_t channel = 0; channel < channels; ++channel)
            for (uint32_t frame = 0; frame < frames; ++frame)
                input.at(channel, frame) = impulse
                                               ? (offset + frame == 0 ? 0.5f : 0)
                                               : static_cast<float>(tone(offset + frame, sourceRate,
                                                                         frequency + channel * 37));
        const auto block = realtime([&] {
            return converter.convert(origin + offset, input.view(), frames);
        });
        assert(block.valid && block.firstFrame == expected);
        assert(block.interleaved.size() == static_cast<size_t>(block.frames) * channels);
        assert(std::all_of(block.interleaved.begin(), block.interleaved.end(), [](float value) {
            return std::isfinite(value);
        }));
        expected += block.frames;
        signal.samples.insert(signal.samples.end(), block.interleaved.begin(),
                              block.interleaved.end());
        offset += frames;
    }
    assert(expected == ceiling(origin + total, outputRate, sourceRate));
    return signal;
}
void quality() {
    struct Case {
        uint32_t source, output;
        double frequency;
        bool stopband;
    };
    for (const auto value : {Case{48000, 48000, 20000, false}, Case{48000, 44100, 20000, false},
                             Case{44100, 48000, 19000, false}, Case{96000, 48000, 20000, false},
                             Case{192000, 8000, 3200, false}, Case{8000, 192000, 3200, false},
                             Case{48000, 44100, 22100, true}, Case{96000, 48000, 24100, true},
                             Case{192000, 8000, 4050, true}}) {
        const auto delay = CausalResampler::delayFrames(value.source, value.output);
        const auto total = std::max(4096U, delay * 6 + 256);
        const auto signal = render(2, value.source, value.output, total, 64, value.frequency);
        double minimumSnr = 300, maximumGain = -300;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            double error = 0, energy = 0, reference = 0;
            uint32_t measured = 0;
            for (uint32_t frame = 0; frame < signal.samples.size() / 2; ++frame) {
                const auto position =
                    coordinate(signal.first + frame, value.source, value.output, 0, delay);
                if (position < 2 * delay)
                    continue;
                const auto expected = tone(position, value.source, value.frequency + channel * 37);
                const auto actual = signal.samples[frame * 2 + channel];
                error += (actual - expected) * (actual - expected);
                energy += actual * actual;
                reference += expected * expected;
                ++measured;
            }
            assert(measured > 100);
            minimumSnr = std::min(minimumSnr, 10 * std::log10(reference / std::max(error, 1e-30)));
            maximumGain =
                std::max(maximumGain, 10 * std::log10(std::max(energy, 1e-30) / reference));
        }
        std::printf("causal %u -> %u, %.0f Hz: delay %u source frames, SNR %.2f dB, gain %.5f dB\n",
                    value.source, value.output, value.frequency, delay, minimumSnr, maximumGain);
        assert(value.stopband ? maximumGain < -90 : minimumSnr > 90);
    }
}
void partitioningAndWideChannels() {
    for (uint32_t channels : {2U, 8U, 64U})
        for (auto rates :
             {std::pair{48000U, 48000U}, std::pair{48000U, 44100U}, std::pair{44100U, 48000U}}) {
            const uint64_t origin = (uint64_t{1} << 55) + 1234;
            const auto whole =
                render(channels, rates.first, rates.second, 4096, 256, 1000, false, origin);
            const auto split =
                render(channels, rates.first, rates.second, 4096, 0, 1000, false, origin);
            assert(whole.first == split.first && whole.samples.size() == split.samples.size());
            for (size_t index = 0; index < whole.samples.size(); ++index)
                assert(std::abs(whole.samples[index] - split.samples[index]) < 0.000002f);
        }
    for (auto rates : {std::pair{8000U, 192000U}, std::pair{192000U, 8000U}}) {
        const auto whole = render(2, rates.first, rates.second, 4096, 256, 1000);
        const auto split = render(2, rates.first, rates.second, 4096, 0, 1000);
        assert(whole.first == split.first && whole.samples.size() == split.samples.size());
        for (size_t index = 0; index < whole.samples.size(); ++index)
            assert(std::abs(whole.samples[index] - split.samples[index]) < 0.000002f);
    }
}
void firstImpulseAndReset() {
    for (const auto rates : {std::pair{48000U, 48000U}, std::pair{48000U, 44100U},
                             std::pair{44100U, 48000U}, std::pair{192000U, 8000U}})
        for (uint64_t origin : {uint64_t{0}, uint64_t{1000000000031}}) {
            const auto delay = CausalResampler::delayFrames(rates.first, rates.second);
            const auto signal =
                render(2, rates.first, rates.second, delay * 4 + 256, 0, 0, true, origin);
            size_t peak = 0;
            for (size_t frame = 0; frame < signal.samples.size() / 2; ++frame)
                if (std::abs(signal.samples[frame * 2]) > std::abs(signal.samples[peak * 2]))
                    peak = frame;
            const auto expected = ceiling(origin + delay, rates.second, rates.first);
            const auto observed = signal.first + peak;
            assert(observed >= expected - 1 && observed <= expected + 1);
            assert(std::abs(signal.samples[peak * 2]) >
                   0.2 * std::min(1.0, double(rates.second) / rates.first));
        }
    CausalResampler converter(2, 48000, 44100);
    Input input(2);
    std::fill(input.data.begin(), input.data.end(), 0.25f);
    for (uint32_t block = 0; block < 8; ++block)
        assert(converter.convert(block * 256, input.view(), 256).valid);
    converter.reset();
    std::fill(input.data.begin(), input.data.end(), 0);
    for (uint64_t first : {uint64_t{0}, uint64_t{10000}, uint64_t{20000}}) {
        const auto block = realtime([&] {
            return converter.convert(first, input.view(), 256);
        });
        assert(block.valid &&
               std::all_of(block.interleaved.begin(), block.interleaved.end(), [](float value) {
                   return value == 0;
               }));
    }
    auto bad = input.view();
    bad.capacityFrames = 1;
    assert(!converter.convert(0, bad, 64).valid);
    assert(!converter.convert(UINT64_MAX, input.view(), 64).valid);
    assert(converter.convert(0, input.view(), 64).valid);
}
void sameRateExactDelayAndWrap() {
    for (const auto channels : {2U, 8U, 64U})
        for (const auto rate : {44100U, 48000U, 192000U}) {
            CausalResampler converter(channels, rate, rate);
            Input input(channels);
            std::array<float, 64> previous{};
            uint64_t first = (uint64_t{1} << 55) + 1234;
            constexpr uint32_t sizes[]{1, 17, 64, 3, 128, 11, 256};
            for (uint32_t block = 0; block < 40; ++block) {
                if (block == 13) {
                    first += 5000;
                    previous.fill(0);
                }
                if (block == 21) {
                    converter.reset();
                    previous.fill(0);
                }
                const auto frames = sizes[block % 7];
                for (uint32_t channel = 0; channel < channels; ++channel)
                    for (uint32_t frame = 0; frame < frames; ++frame) {
                        const auto code = (first + frame + channel * 37) % 257;
                        input.at(channel, frame) =
                            code == 0   ? NAN
                            : code == 1 ? INFINITY
                                        : static_cast<float>(static_cast<int>(code) - 128) / 64;
                    }
                const auto result = realtime([&] {
                    return converter.convert(first, input.view(), frames);
                });
                assert(result.valid && result.firstFrame == first && result.frames == frames);
                for (uint32_t frame = 0; frame < frames; ++frame)
                    for (uint32_t channel = 0; channel < channels; ++channel) {
                        assert(result.interleaved[frame * channels + channel] == previous[channel]);
                        const auto sample = input.at(channel, frame);
                        previous[channel] = std::isfinite(sample) ? sample : 0.f;
                    }
                first += frames;
            }
        }
}
} // namespace
int main() {
    assert(CausalResampler::delayFrames(48000, 48000) == 1);
    assert(CausalResampler::delayFrames(48000, 44100) == 113);
    assert(CausalResampler::delayFrames(44100, 48000) == 104);
    assert(CausalResampler::delayFrames(8000, 48000) ==
           CausalResampler::kMaximumUpsamplingDelayFrames);
    quality();
    partitioningAndWideChannels();
    firstImpulseAndReset();
    sameRateExactDelayAndWrap();
    std::puts("Causal output conversion: declared delay, first impulse, spectrum, partitioning, "
              "wide channels, reset and callback heap checks passed");
}
