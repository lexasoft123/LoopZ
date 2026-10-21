#include <loopz/audio_resampler.hpp>
#include <loopz/minimum_phase_kernel.hpp>
#include <loopz/audio_history.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>
#include <numbers>
#include <stdexcept>
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
std::complex<double> response(double sourceRate, double outputRate, double frequency,
                              double phase) {
    const auto& kernel = MinimumPhaseKernel::prepared();
    const auto cutoff = SincKernel::kCutoff * std::min(1.0, outputRate / sourceRate);
    const auto taps = static_cast<uint32_t>(std::ceil(MinimumPhaseKernel::kPastSupport / cutoff));
    std::complex<double> sum{};
    double normalization = 0;
    for (uint32_t tap = 0; tap <= taps; ++tap) {
        const auto distance = phase + tap;
        const auto weight = kernel.coefficient(distance, cutoff);
        sum += weight *
               std::polar(1.0, -2 * std::numbers::pi_v<double> * frequency / sourceRate * distance);
        normalization += weight;
    }
    assert(std::isfinite(normalization) && normalization > .5);
    return sum / normalization;
}
void spectralQualityAndFractionalContinuity() {
    struct Band {
        int source, output;
        double passEnd;
    };
    for (const auto [source, output, passEnd] : {Band{48000, 48000, 20000},
                                                 {48000, 44100, 20000},
                                                 {44100, 48000, 19000},
                                                 {96000, 48000, 20000},
                                                 {192000, 8000, 3200},
                                                 {8000, 192000, 3200}}) {
        double worstPass = 0, worstVariation = 0, worstStop = -300;
        for (unsigned frequency = 0; frequency <= 32; ++frequency) {
            const auto hz = passEnd * frequency / 32;
            const auto reference = response(source, output, hz, 0);
            for (unsigned phase = 0; phase <= 16; ++phase) {
                const auto actual = response(source, output, hz, phase / 16.0);
                worstPass = std::max(worstPass, std::abs(20 * std::log10(std::abs(actual))));
                worstVariation = std::max(worstVariation, std::abs(actual - reference));
            }
        }
        if (source > output)
            for (unsigned frequency = 0; frequency <= 32; ++frequency) {
                const auto begin = output / 2.0 + 50;
                const auto hz = begin + (source / 2.0 - 50 - begin) * frequency / 32;
                for (unsigned phase = 0; phase <= 16; ++phase)
                    worstStop = std::max(
                        worstStop,
                        20 * std::log10(std::max(
                                 std::abs(response(source, output, hz, phase / 16.0)), 1e-30)));
            }
        std::printf(
            "minimum-phase %d -> %d: pass ripple %.6f dB, phase-bank error %.2f dB, stop %.2f dB\n",
            source, output, worstPass, 20 * std::log10(std::max(worstVariation, 1e-30)), worstStop);
        assert(worstPass < .001 && worstVariation < .000031622777);
        assert(worstStop < -90);
    }
}
struct Provider {
    uint64_t availableThrough{1000000}, missing{UINT64_MAX};
    mutable uint64_t latestRead{0};
    uint32_t channels{2};
    double sourceRate{48000};
    double frequency{1000};
    static ResampleFrame read(const void* context, uint64_t frame,
                              std::span<float> output) noexcept {
        auto& self = *static_cast<const Provider*>(context);
        self.latestRead = std::max(self.latestRead, frame);
        if (frame > self.availableThrough || frame == self.missing)
            return {false, 0};
        for (size_t channel = 0; channel < output.size(); ++channel)
            output[channel] = static_cast<float>(
                .1 * std::sin(2 * std::numbers::pi_v<double> * (self.frequency + channel * 37) *
                              frame / self.sourceRate));
        return {true, self.channels};
    }
};
void changingClockKeepsFilterPhase() {
    AudioResampler causal(ResamplingFilter::MinimumPhase);
    for (const auto frequency : {997.0, 10000.0, 19000.0}) {
        Provider provider;
        provider.channels = 1;
        provider.frequency = frequency;
        std::array<float, 32> storage{};
        const std::array<float*, 1> planes{storage.data()};
        const auto transfer = response(48000, 48000, frequency, 0);
        long double point = 10000.27L;
        double maximumError = 0, maximumBoundaryError = 0, previousError = 0;
        bool previous = false;
        for (const auto step : {1.0, 1.0002, .9998, 32.0 / 31, 32.0 / 33, 1.0}) {
            const auto anchor = static_cast<uint64_t>(std::floor(point));
            const ResamplePosition position{.anchor = anchor,
                                            .offset = point - anchor,
                                            .sourceRate = 48000 * step,
                                            .outputRate = 48000,
                                            .localClockCorrection = std::abs(step - 1) > .005,
                                            .nominalSourceRate = 48000};
            provider.availableThrough = static_cast<uint64_t>(std::floor(point + 31 * step));
            provider.latestRead = 0;
            heapEvents = 0;
            trackHeap = true;
            const auto missing = causal.read(position, {planes, 32}, 32, Provider::read, &provider);
            trackHeap = false;
            assert(!missing && !heapEvents && provider.latestRead <= provider.availableThrough);
            for (unsigned frame = 0; frame < 32; ++frame) {
                const auto coordinate = point + frame * step;
                const auto expected =
                    .1 * std::abs(transfer) *
                    std::sin(2 * std::numbers::pi_v<long double> * frequency * coordinate / 48000 +
                             std::arg(transfer));
                const auto error = static_cast<double>(storage[frame] - expected);
                maximumError = std::max(maximumError, std::abs(error));
                if (previous)
                    maximumBoundaryError =
                        std::max(maximumBoundaryError, std::abs(error - previousError));
                previousError = error;
                previous = true;
            }
            point += 32 * step;
        }
        std::printf("minimum-phase clock %.0f Hz: error %.8f, boundary %.8f\n", frequency,
                    maximumError, maximumBoundaryError);
        assert(maximumError < .00001 && maximumBoundaryError < .00001);
    }
}
void fixedFilterDriftRejection() {
    for (const auto [source, output] :
         {std::pair{8000, 8000}, {48000, 48000}, {48000, 44100}, {96000, 48000}, {192000, 8000}}) {
        double maximumGain = -300;
        for (const auto ratio : {.995, 1.005}) {
            const auto actualRate = source * ratio;
            const auto begin = output / 2.0 + 1;
            const auto end = actualRate / 2 - 1;
            if (end < begin)
                continue;
            for (unsigned frequency = 0; frequency <= 32; ++frequency) {
                const auto physicalHz = begin + (end - begin) * frequency / 32;
                const auto nominalHz = physicalHz * source / actualRate;
                for (unsigned phase = 0; phase <= 16; ++phase)
                    maximumGain = std::max(
                        maximumGain,
                        20 * std::log10(std::max(
                                 std::abs(response(source, output, nominalHz, phase / 16.0)),
                                 1e-30)));
            }
        }
        std::printf("minimum-phase fixed band %d -> %d, +/-5000 ppm: stop %.2f dB\n", source,
                    output, maximumGain);
        assert(maximumGain < -90);
    }
}
void historyClockCorrections() {
    for (const auto [source, output] :
         {std::pair{8000U, 8000U}, {48000U, 48000U}, {192000U, 48000U}}) {
        AudioHistory history(2, 8192, ResamplingFilter::MinimumPhase);
        std::array<float, 32> left{}, right{};
        const std::array<const float*, 2> input{left.data(), right.data()};
        std::vector<uint64_t> anchors;
        anchors.reserve(128);
        constexpr uint64_t origin = 9000000000ULL;
        const auto hz = std::min(20000.0, source * .4);
        for (unsigned block = 0; block < 128; ++block) {
            const auto first = block * 32;
            const auto offset = (block / 16) % 2 ? frameNs(1, source) : 0;
            const auto ns = origin + frameNs(first, source) + offset;
            anchors.push_back(ns);
            for (unsigned frame = 0; frame < 32; ++frame) {
                left[frame] = static_cast<float>(
                    .1 * std::sin(2 * std::numbers::pi_v<double> * hz * (first + frame) / source));
                right[frame] =
                    static_cast<float>(.1 * std::sin(2 * std::numbers::pi_v<double> * (hz + 17) *
                                                     (first + frame) / source));
            }
            heapEvents = 0;
            trackHeap = true;
            const auto appended = history.append({input, 32}, 32, ns, source, first, true);
            trackHeap = false;
            assert(appended && !heapEvents);
        }
        std::array<float, 64> a{}, b{};
        const std::array<float*, 2> planes{a.data(), b.data()};
        AudioReadCursor cursor;
        double maximumError = 0;
        for (unsigned block = 40; block < 88; ++block) {
            const auto first = anchors[block] + frameNs(1, source) / 3;
            heapEvents = 0;
            trackHeap = true;
            const auto result = history.read(first, output, {planes, 64}, 64, cursor);
            trackHeap = false;
            assert(result.online && !result.missingFrames && !heapEvents);
            for (unsigned channel = 0; channel < 2; ++channel) {
                const auto frequency = hz + channel * 17;
                const auto transfer = response(source, output, frequency, 0);
                for (unsigned frame = 0; frame < 64; ++frame) {
                    const auto ns = first + static_cast<long double>(frame) * 1e9L / output;
                    const auto next = std::upper_bound(anchors.begin(), anchors.end(), ns);
                    assert(next != anchors.begin() && next != anchors.end());
                    const auto index = static_cast<size_t>(next - anchors.begin() - 1);
                    const auto coordinate = index * 32 + (ns - anchors[index]) * 32 /
                                                             (anchors[index + 1] - anchors[index]);
                    const auto expected = .1 * std::abs(transfer) *
                                          std::sin(2 * std::numbers::pi_v<long double> * frequency *
                                                       coordinate / source +
                                                   std::arg(transfer));
                    maximumError =
                        std::max(maximumError,
                                 std::abs(static_cast<double>(planes[channel][frame] - expected)));
                }
            }
        }
        std::printf("minimum-phase history %u -> %u: correction error %.8f\n", source, output,
                    maximumError);
        // The clock contract admits two ns of nominal-interval rounding.
        const auto tolerance = 2e-9 * 2 * std::numbers::pi_v<double> * (hz + 17) * .1 + .000005;
        assert(maximumError < tolerance && history.status().discontinuities == 0);
    }
}
void independentInputAndOutputClocks() {
    constexpr std::array rates{8000U, 44100U, 48000U, 96000U, 192000U};
    constexpr uint32_t period = 32, callbacks = 512;
    constexpr uint64_t origin = 9000000000ULL;
    unsigned reads = 0;
    double worstRelativeError = 0;
    for (const auto source : rates)
        for (const auto output : rates)
            for (const auto sourceDrift : {.995, 1.0, 1.005}) {
                AudioHistory history(2, period * callbacks, ResamplingFilter::MinimumPhase, true);
                const std::array frequencies{.031 * std::min(source, output),
                                             .38 * std::min(source, output)};
                std::array<float, period> left{}, right{};
                const std::array<const float*, 2> input{left.data(), right.data()};
                std::array<uint64_t, callbacks> anchors{};
                for (uint32_t block = 0; block < callbacks; ++block) {
                    const auto first = block * period;
                    // A separate source oscillator plus one-sample host-time
                    // corrections. Sample counters stay authoritative and contiguous.
                    const auto correction = (block / 16) % 2 ? frameNs(1, source) : 0;
                    anchors[block] =
                        origin +
                        static_cast<uint64_t>(std::llround(first * 1e9L / (source * sourceDrift))) +
                        correction;
                    for (uint32_t frame = 0; frame < period; ++frame) {
                        left[frame] = static_cast<float>(
                            .1 * std::sin(2 * std::numbers::pi_v<double> * frequencies[0] *
                                          (first + frame) / source));
                        right[frame] = static_cast<float>(
                            .1 * std::sin(2 * std::numbers::pi_v<double> * frequencies[1] *
                                          (first + frame) / source));
                    }
                    heapEvents = 0;
                    trackHeap = true;
                    const auto appended = history.append({input, period}, period, anchors[block],
                                                         source, first, true);
                    trackHeap = false;
                    assert(appended && !heapEvents);
                }
                AudioReadCursor cursor;
                for (const auto outputDrift : {.995, 1.0, 1.005}) {
                    const auto outputClock = output * outputDrift;
                    const std::array transfer{response(source, outputClock, frequencies[0], 0),
                                              response(source, outputClock, frequencies[1], 0)};
                    for (const auto block : {271U, 287U})
                        for (const auto phase : {0.0L, .37L, .99L})
                            for (const auto frames : {32U, 64U}) {
                                // Start just before both signs of a correction;
                                // high conversion ratios cross several clock intervals.
                                const auto first =
                                    anchors[block] +
                                    static_cast<uint64_t>((period - 1 + phase) *
                                                          (anchors[block + 1] - anchors[block]) /
                                                          period);
                                std::array<float, 66> a{}, b{};
                                a.fill(9);
                                b.fill(9);
                                const std::array<float*, 2> planes{a.data() + 1, b.data() + 1};
                                heapEvents = 0;
                                trackHeap = true;
                                const auto result = history.read(first, outputClock,
                                                                 {planes, frames}, frames, cursor);
                                trackHeap = false;
                                assert(result.online && !result.missingFrames && !heapEvents);
                                assert(a.front() == 9 && a[frames + 1] == 9 && b.front() == 9 &&
                                       b[frames + 1] == 9);
                                for (unsigned channel = 0; channel < 2; ++channel)
                                    for (unsigned frame = 0; frame < frames; ++frame) {
                                        // Independent scalar timestamp interpolation and
                                        // the frequency-domain response, rather than a
                                        // second call through the production resampler.
                                        const auto ns = first + frame * 1e9L / outputClock;
                                        const auto next =
                                            std::upper_bound(anchors.begin(), anchors.end(), ns);
                                        assert(next != anchors.begin() && next != anchors.end());
                                        const auto index =
                                            static_cast<size_t>(next - anchors.begin() - 1);
                                        const auto coordinate =
                                            index * period +
                                            (ns - anchors[index]) * period /
                                                (anchors[index + 1] - anchors[index]);
                                        const auto expected =
                                            .1 * std::abs(transfer[channel]) *
                                            std::sin(2 * std::numbers::pi_v<long double> *
                                                         frequencies[channel] * coordinate /
                                                         source +
                                                     std::arg(transfer[channel]));
                                        const auto error = std::abs(
                                            static_cast<double>(planes[channel][frame] - expected));
                                        // The clock contract may snap an interval within
                                        // two ns to nominal. Bound that error explicitly.
                                        const auto tolerance =
                                            .00001 + 2e-9 * 2 * std::numbers::pi_v<double> *
                                                         frequencies[channel] * .1;
                                        worstRelativeError =
                                            std::max(worstRelativeError, error / tolerance);
                                        assert(error < tolerance);
                                    }
                                ++reads;
                            }
                }
                const auto diagnostic = history.readDiagnostics();
                assert(diagnostic.enabled && diagnostic.reads == 36 &&
                       diagnostic.requestedFrames == 1728);
                assert(std::all_of(diagnostic.missingFrames.begin(), diagnostic.missingFrames.end(),
                                   [](auto value) {
                                       return value == 0;
                                   }));
                assert(!diagnostic.unpublishedSampleReads && !diagnostic.expiredSampleReads &&
                       !diagnostic.racingSampleReads && !diagnostic.absentSampleReads);
                assert(!history.status().discontinuities);
            }
    std::printf("minimum-phase independent clocks: %u reads, 25 rate pairs, "
                "maximum error/tolerance %.6f\n",
                reads, worstRelativeError);
}
void independentClockStopband() {
    constexpr std::array rates{8000U, 44100U, 48000U, 96000U, 192000U};
    double worstStop = -300;
    unsigned cases = 0, responses = 0;
    for (const auto source : rates)
        for (const auto output : rates)
            for (const auto sourceDrift : {.995, 1.0, 1.005})
                for (const auto outputDrift : {.995, 1.0, 1.005}) {
                    const auto sourceClock = source * sourceDrift;
                    const auto outputClock = output * outputDrift;
                    const auto begin = outputClock / 2 + 50;
                    const auto end = sourceClock / 2 - 50;
                    if (end < begin)
                        continue;
                    ++cases;
                    // The filter is pinned to the source format, but samples
                    // arrive on its measured clock and leave on a separate
                    // measured clock. Sample physical frequencies above the
                    // destination Nyquist frequency, including its near edge.
                    for (unsigned frequency = 0; frequency <= 16; ++frequency) {
                        const auto physicalHz = begin + (end - begin) * frequency / 16;
                        const auto sourceHz = physicalHz / sourceDrift;
                        for (unsigned phase = 0; phase <= 8; ++phase) {
                            const auto gain =
                                20 * std::log10(std::max(std::abs(response(source, outputClock,
                                                                           sourceHz, phase / 8.0)),
                                                         1e-30));
                            worstStop = std::max(worstStop, gain);
                            assert(gain < -90);
                            ++responses;
                        }
                    }
                }
    std::printf("minimum-phase independent-clock stopband: %u cases, %u responses, "
                "maximum gain %.6f dB\n",
                cases, responses, worstStop);
}
void invalidNominalClockIsSilent() {
    AudioResampler causal(ResamplingFilter::MinimumPhase), linear;
    Provider provider;
    std::array<float, 32> storage{};
    const std::array<float*, 1> planes{storage.data()};
    std::array<uint8_t, 32> coverage{};
    for (const auto nominal :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -48000.0, 7999.0, 192001.0, 44100.0, 96000.0}) {
        storage.fill(1);
        coverage.fill(1);
        provider.latestRead = 0;
        const ResamplePosition position{.anchor = 10000,
                                        .offset = 0,
                                        .sourceRate = 48000,
                                        .outputRate = 48000,
                                        .nominalSourceRate = nominal};
        assert(causal.read(position, {planes, 32}, 32, Provider::read, &provider, coverage) == 32);
        assert(!provider.latestRead);
        assert(std::all_of(storage.begin(), storage.end(), [](auto value) {
            return value == 0;
        }));
        assert(std::all_of(coverage.begin(), coverage.end(), [](auto value) {
            return value == 0;
        }));
        // The production reader ignores this optional experimental hint.
        assert(!linear.read(position, {planes, 32}, 32, Provider::read, &provider));
    }
    for (const auto sourceRate : {38399.0, 60001.0}) {
        provider.latestRead = 0;
        assert(causal.read({.anchor = 10000,
                            .offset = 0,
                            .sourceRate = sourceRate,
                            .outputRate = 48000,
                            .localClockCorrection = true,
                            .nominalSourceRate = 48000},
                           {planes, 32}, 32, Provider::read, &provider) == 32);
        assert(!provider.latestRead);
    }
}
void rateConversionAndStridedBounds() {
    AudioResampler causal(ResamplingFilter::MinimumPhase);
    for (const auto [source, output] : {std::pair{48000, 44100},
                                        {44100, 48000},
                                        {96000, 48000},
                                        {192000, 8000},
                                        {8000, 192000}}) {
        Provider provider;
        provider.sourceRate = source;
        const auto step = double(source) / output;
        provider.availableThrough = static_cast<uint64_t>(std::floor(10000 + .27 + 31 * step));
        std::array<float, 66> storage;
        storage.fill(9);
        const std::array<float*, 2> planes{storage.data() + 1, storage.data() + 2};
        std::array<uint8_t, 32> coverage{};
        heapEvents = 0;
        trackHeap = true;
        const auto missing = causal.read({10000, .27L, double(source), double(output)},
                                         {planes, 32, 2}, 32, Provider::read, &provider, coverage);
        trackHeap = false;
        assert(!missing && !heapEvents && provider.latestRead <= provider.availableThrough);
        assert(storage.front() == 9 && storage.back() == 9);
        assert(std::all_of(coverage.begin(), coverage.end(), [](auto value) {
            return value == 1;
        }));
        for (unsigned channel = 0; channel < 2; ++channel) {
            const auto hz = 1000 + channel * 37;
            const auto transfer = response(source, output, hz, 0);
            for (unsigned frame = 0; frame < 32; ++frame) {
                const auto point = 10000 + .27 + frame * step;
                const auto expected =
                    .1 * std::abs(transfer) *
                    std::sin(2 * std::numbers::pi_v<double> * hz * point / source +
                             std::arg(transfer));
                assert(std::abs(planes[channel][frame * 2] - expected) < .00001);
            }
        }
    }
}
void causalReadsAndWideOutput() {
    AudioResampler causal(ResamplingFilter::MinimumPhase), linear;
    for (const uint32_t channels : {1U, 2U, 8U, 64U}) {
        std::array<std::array<float, 64>, 64> storage{};
        std::array<float*, 64> planes{};
        for (uint32_t channel = 0; channel < channels; ++channel)
            planes[channel] = storage[channel].data();
        const ResampleOutput output{{planes.data(), channels}, 64};
        // The corrected rate crosses an integer phase inside this block. The
        // output must keep filtering rather than jumping to the direct path.
        const ResamplePosition position{10000, .9975L, 48000.0 * 1.0001, 48000};
        Provider provider;
        provider.channels = channels;
        provider.availableThrough = 10064;
        std::array<uint8_t, 64> coverage{};
        heapEvents = 0;
        trackHeap = true;
        const auto missing = causal.read(position, output, 64, Provider::read, &provider, coverage);
        trackHeap = false;
        assert(!missing && !heapEvents && provider.latestRead <= provider.availableThrough);
        assert(std::all_of(coverage.begin(), coverage.end(), [](auto value) {
            return value == 1;
        }));
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const auto hz = 1000 + channel * 37;
            // Provider phase uses a nominal 48k frame index; the filter's
            // cutoff still follows the corrected source clock.
            const auto actualTransfer = response(position.sourceRate, position.outputRate,
                                                 hz * position.sourceRate / 48000, 0);
            for (uint32_t frame = 0; frame < 64; ++frame) {
                const auto point = 10000 + .9975 + frame * 1.0001;
                const auto expected = .1 * std::abs(actualTransfer) *
                                      std::sin(2 * std::numbers::pi_v<double> * hz * point / 48000 +
                                               std::arg(actualTransfer));
                assert(std::abs(storage[channel][frame] - expected) < .00001);
            }
        }
        assert(linear.read(position, output, 64, Provider::read, &provider) > 0);
        provider.latestRead = 0;
        provider.availableThrough = 10063;
        assert(!causal.read({10000, 0, 48000, 48000}, output, 64, Provider::read, &provider));
        assert(provider.latestRead == 10063);
        const auto raw = .1 * std::sin(2 * std::numbers::pi_v<double> * 1000 * 10000 / 48000);
        assert(std::abs(storage[0][0] - raw) > .001); // Integer reads keep the same filter phase.
        provider.availableThrough = 10064;
        provider.missing = 9996;
        assert(causal.read(position, output, 64, Provider::read, &provider) > 0);
        provider.latestRead = 0;
        assert(causal.read({0, 0, 48000, 48000}, output, 64, Provider::read, &provider) == 64);
        assert(!provider.latestRead);
        for (uint32_t channel = 0; channel < channels; ++channel)
            assert(std::all_of(storage[channel].begin(), storage[channel].end(), [](auto value) {
                return value == 0;
            }));
    }
    const auto& kernel = MinimumPhaseKernel::prepared();
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto distance : {-1.0, infinity, nan, 1000000.0})
        assert(kernel.coefficient(distance, .94) == 0);
    for (const auto cutoff : {0.0, -1.0, 2.0, infinity, nan})
        assert(kernel.coefficient(1, cutoff) == 0);
    bool rejected = false;
    try {
        AudioResampler invalid(static_cast<ResamplingFilter>(99));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace
int main() {
    spectralQualityAndFractionalContinuity();
    changingClockKeepsFilterPhase();
    fixedFilterDriftRejection();
    historyClockCorrections();
    independentInputAndOutputClocks();
    independentClockStopband();
    invalidNominalClockIsSilent();
    rateConversionAndStridedBounds();
    causalReadsAndWideOutput();
}
