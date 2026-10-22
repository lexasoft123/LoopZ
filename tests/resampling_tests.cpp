#include <loopz/audio_history.hpp>
#include <loopz/audio_resampler.hpp>
#include <loopz/timeline.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>
#include <numbers>
#include <string_view>
#include <thread>
#include <vector>

namespace {
thread_local bool trackHeap = false;
thread_local uint32_t heapEvents = 0;
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
void operator delete(void* memory) noexcept {
    if (trackHeap && memory)
        ++heapEvents;
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}
void operator delete(void* memory, size_t) noexcept {
    ::operator delete(memory);
}
void operator delete[](void* memory, size_t) noexcept {
    ::operator delete(memory);
}

namespace {
using namespace loopz;
struct Planes {
    std::vector<float> values;
    std::array<float*, 64> write{};
    std::array<const float*, 64> read{};
    uint32_t channels, frames;
    Planes(uint32_t count, uint32_t capacity)
        : values(static_cast<size_t>(count) * capacity), channels(count), frames(capacity) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            write[channel] = values.data() + channel * frames;
            read[channel] = write[channel];
        }
    }
    RoutingInput input() const {
        return {{read.data(), channels}, frames};
    }
    RoutingOutput output() {
        return {{write.data(), channels}, frames};
    }
};
template <typename Operation> auto realtime(Operation operation) {
    trackHeap = true;
    heapEvents = 0;
    auto result = operation();
    trackHeap = false;
    assert(heapEvents == 0);
    return result;
}
double tone(double frame, double rate, double frequency) {
    return 0.25 * std::sin(2 * std::numbers::pi_v<double> * frequency * frame / rate);
}
struct Fixture {
    AudioHistory history;
    Planes input, output;
    const uint32_t channels, sourceRate;
    const uint64_t origin;
    Fixture(uint32_t count, uint32_t rate, double frequency, uint64_t start = 9000000000ULL)
        : history(count), input(count, kCapacity), output(count, kBlock), channels(count),
          sourceRate(rate), origin(start) {
        for (uint32_t channel = 0; channel < channels; ++channel)
            for (uint32_t frame = 0; frame < kCapacity; ++frame)
                input.write[channel][frame] =
                    static_cast<float>(tone(frame, rate, frequency + channel * 37));
        assert(realtime([&] {
            return history.append(input.input(), kCapacity, origin, sourceRate, 0, true);
        }));
    }
    uint64_t start(bool fractional = true) const {
        return origin + static_cast<uint64_t>((4096 + (fractional ? 0.37 : 0)) * 1e9L / sourceRate);
    }
};
struct Quality {
    double gainDb, snrDb, maximumGainDb;
};
Quality quality(uint32_t sourceRate, uint32_t outputRate, double frequency, uint32_t channels = 2,
                uint64_t origin = 9000000000ULL) {
    Fixture fixture(channels, sourceRate, frequency, origin);
    AudioReadCursor cursor;
    std::array<double, 64> error{}, energy{}, reference{};
    for (uint32_t offset = 0; offset < 768; offset += kBlock) {
        const auto first = fixture.start() + frameNs(offset, outputRate);
        const auto result = realtime([&] {
            return fixture.history.read(first, outputRate, fixture.output.output(), kBlock, cursor);
        });
        assert(result.online && result.missingFrames == 0);
        for (uint32_t channel = 0; channel < channels; ++channel)
            for (uint32_t frame = 0; frame < kBlock; ++frame) {
                const auto expected = tone((first - origin) * (sourceRate / 1e9) +
                                               static_cast<double>(frame) * sourceRate / outputRate,
                                           sourceRate, frequency + channel * 37);
                const auto actual = fixture.output.write[channel][frame];
                assert(std::isfinite(actual));
                error[channel] += (actual - expected) * (actual - expected);
                energy[channel] += actual * actual;
                reference[channel] += expected * expected;
            }
    }
    Quality result{0, 300, -300};
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const auto gain = 10 * std::log10(std::max(energy[channel], 1e-30) / reference[channel]);
        const auto snr = 10 * std::log10(reference[channel] / std::max(error[channel], 1e-30));
        if (std::abs(gain) > std::abs(result.gainDb))
            result.gainDb = gain;
        result.snrDb = std::min(result.snrDb, snr);
        result.maximumGainDb = std::max(result.maximumGainDb, gain);
    }
    return result;
}
void spectralQuality() {
    struct Case {
        uint32_t source, output;
        double frequency;
    };
    for (const auto value :
         {Case{48000, 44100, 1000}, Case{48000, 44100, 20000}, Case{44100, 48000, 19000},
          Case{96000, 48000, 20000}, Case{192000, 8000, 3200}, Case{8000, 192000, 3200}}) {
        const auto result = quality(value.source, value.output, value.frequency);
        std::printf("passband %u -> %u, %.0f Hz: gain %.5f dB, SNR %.2f dB\n", value.source,
                    value.output, value.frequency, result.gainDb, result.snrDb);
        assert(std::abs(result.gainDb) < 0.002 && result.snrDb > 90);
    }
    for (const auto value :
         {Case{48000, 44100, 22100}, Case{48000, 44100, 23000}, Case{96000, 48000, 24100},
          Case{96000, 48000, 26400}, Case{192000, 8000, 4050}, Case{192000, 8000, 4400}}) {
        const auto result = quality(value.source, value.output, value.frequency);
        std::printf("stopband %u -> %u, %.0f Hz: %.2f dB\n", value.source, value.output,
                    value.frequency, result.maximumGainDb);
        assert(result.maximumGainDb < -90);
    }
    for (uint32_t channels : {2U, 8U, 64U}) {
        const auto result = quality(48000, 44100, 1000, channels);
        assert(std::abs(result.gainDb) < 0.002 && result.snrDb > 90);
    }
    const auto longUptime = quality(48000, 44100, 1000, 8, 4000000000000000000ULL);
    assert(longUptime.snrDb > 90);
}
void sparseChannelsMatchFullHistory() {
    for (auto filter : {ResamplingFilter::LinearPhase, ResamplingFilter::MinimumPhase}) {
        AudioHistory history(8, AudioHistory::kDefaultCapacityFrames, filter);
        Planes input(8, 8192), full(8, kBlock), sparse(2, kBlock);
        for (uint32_t channel = 0; channel < 8; ++channel)
            for (uint32_t frame = 0; frame < input.frames; ++frame)
                input.write[channel][frame] =
                    static_cast<float>(tone(frame, 48000, 300 + channel * 700));
        constexpr uint64_t origin = 1000000000;
        assert(history.append(input.input(), input.frames, origin, 48000, 0, true));
        for (double rate : {44100.0, 48000.0, 96000.0}) {
            for (const auto channels :
                 {std::array<uint32_t, 2>{2, 3}, std::array<uint32_t, 2>{1, 7}}) {
                AudioReadCursor fullCursor, selectedCursor;
                const auto when = origin + frameNs(2048, 48000) + 7000;
                const auto baseline = realtime([&] {
                    return history.readWithFilter(when, rate, full.output(), 64, fullCursor,
                                                  filter);
                });
                const auto result = realtime([&] {
                    return history.readChannels(when, rate, sparse.output(), 64, selectedCursor,
                                                channels, filter);
                });
                assert(result && result->online && result->missingFrames == baseline.missingFrames);
                assert(selectedCursor.hostNs == fullCursor.hostNs &&
                       selectedCursor.rate == fullCursor.rate);
                for (uint32_t channel = 0; channel < 2; ++channel)
                    for (uint32_t frame = 0; frame < 64; ++frame)
                        assert(sparse.write[channel][frame] ==
                               full.write[channels[channel]][frame]);
            }
        }
        // Input channels may disappear without changing the logical layout.
        // An unwired missing tail must not make a valid selected lane missing.
        history.stop();
        const RoutingInput narrow{{input.read.data(), 4}, input.frames};
        assert(history.append(narrow, input.frames, origin + 1000000000, 48000, 0, true));
        AudioReadCursor cursor;
        const std::array<uint32_t, 2> partial{2, 7}, available{1, 3};
        auto result = history.readChannels(origin + 1000000000 + frameNs(2048, 48000), 48000,
                                           sparse.output(), 64, cursor, partial, filter);
        assert(result && result->online && result->missingFrames == 64);
        assert(std::all_of(sparse.write[1], sparse.write[1] + 64, [](auto value) {
            return value == 0;
        }));
        result = history.readChannels(origin + 1000000000 + frameNs(2048, 48000), 48000,
                                      sparse.output(), 64, cursor, available, filter);
        assert(result && result->online && !result->missingFrames);
        for (const auto invalid : {std::array<uint32_t, 2>{3, 2}, std::array<uint32_t, 2>{2, 2},
                                   std::array<uint32_t, 2>{2, 8}}) {
            result = history.readChannels(origin + 1000000000 + frameNs(2048, 48000), 48000,
                                          sparse.output(), 64, cursor, invalid, filter);
            assert(result && !result->online && result->missingFrames == 64);
            for (uint32_t channel = 0; channel < 2; ++channel)
                assert(
                    std::all_of(sparse.write[channel], sparse.write[channel] + 64, [](auto value) {
                        return value == 0;
                    }));
        }
        history.stop();
        result = history.readChannels(origin + 1000000000 + frameNs(2048, 48000), 48000,
                                      sparse.output(), 64, cursor, available, filter);
        assert(result && !result->online && result->missingFrames == 64);
    }
}
void boundariesAndPartitioning() {
    Fixture fixture(8, 48000, 1000);
    AudioReadCursor cursor;
    assert(!fixture.history.read(fixture.start(false), 48000, fixture.output.output(), 256, cursor)
                .missingFrames);
    for (uint32_t channel = 0; channel < 8; ++channel)
        for (uint32_t frame = 0; frame < 256; ++frame)
            assert(fixture.output.write[channel][frame] ==
                   fixture.input.write[channel][4096 + frame]);
    std::array<std::array<float, 256>, 8> reference{};
    assert(!fixture.history.read(fixture.start(), 44100, fixture.output.output(), 256, cursor)
                .missingFrames);
    for (uint32_t channel = 0; channel < 8; ++channel)
        std::copy_n(fixture.output.write[channel], 256, reference[channel].begin());
    for (uint32_t offset = 0; offset < 256; offset += 64) {
        assert(!realtime([&] {
                    return fixture.history.read(fixture.start() + frameNs(offset, 44100), 44100,
                                                fixture.output.output(), 64, cursor);
                }).missingFrames);
        for (uint32_t channel = 0; channel < 8; ++channel)
            for (uint32_t frame = 0; frame < 64; ++frame)
                assert(std::abs(fixture.output.write[channel][frame] -
                                reference[channel][offset + frame]) < 0.00002);
    }
    // Missing filter support must be reported; never reuse the old generation
    // or substitute a lower-quality interpolator at a recovery boundary.
    assert(fixture.history.read(fixture.origin, 44100, fixture.output.output(), 64, cursor)
               .missingFrames == 64);
    for (uint32_t channel = 0; channel < 8; ++channel)
        for (uint32_t frame = 0; frame < 64; ++frame)
            assert(fixture.output.write[channel][frame] == 0);
    fixture.history.stop();
    assert(
        !fixture.history.read(fixture.start(), 44100, fixture.output.output(), 64, cursor).online);
    assert(fixture.history.append(fixture.input.input(), 256, fixture.origin + 1000000000, 48000, 0,
                                  true));
    assert(fixture.history
               .read(fixture.origin + 1000000000, 44100, fixture.output.output(), 64, cursor)
               .missingFrames == 64);
}
void contiguousHardwareTimestampCorrections() {
    constexpr uint64_t origin = 2000000000;
    for (double rate : {8000.0, 48000.0, 192000.0}) {
        for (uint32_t period : {32U, 64U, 128U}) {
            for (int sign : {-1, 1}) {
                for (bool verified : {false, true}) {
                    AudioHistory history(1);
                    std::array<float, 128> input{};
                    const float* planes[]{input.data()};
                    const auto shift = static_cast<int64_t>(2.21e9 / rate) * sign;
                    for (uint64_t block = 0; block < 256; ++block) {
                        for (uint32_t frame = 0; frame < period; ++frame)
                            input[frame] =
                                0.1f + static_cast<float>(block * period + frame) * 1e-5f;
                        const auto ns = static_cast<uint64_t>(
                            origin + frameNs(block * period, rate) + (block >= 128 ? shift : 0));
                        assert(realtime([&] {
                            return history.append({planes, period}, period, ns, rate,
                                                  static_cast<double>(block * period), verified);
                        }));
                    }
                    std::array<float, 256> output{};
                    float* destination[]{output.data()};
                    AudioReadCursor cursor;
                    const auto read = realtime([&] {
                        return history.read(origin + frameNs(127 * period + 8, rate), rate,
                                            {destination, 256}, 256, cursor);
                    });
                    assert(read.online);
                    assert(verified ? !read.missingFrames : read.missingFrames > 0);
                    if (verified) {
                        const auto first = origin + frameNs(127 * period + 8, rate);
                        for (uint32_t frame = 0; frame < output.size(); ++frame) {
                            const long double ns =
                                first + static_cast<long double>(frame) * 1e9L / rate;
                            uint64_t block = 126;
                            const auto anchor = [&](uint64_t index) {
                                return static_cast<uint64_t>(origin +
                                                             frameNs(index * period, rate) +
                                                             (index >= 128 ? shift : 0));
                            };
                            while (ns >= anchor(block + 1))
                                ++block;
                            const auto position =
                                block * period +
                                (ns - anchor(block)) * period / (anchor(block + 1) - anchor(block));
                            const auto expected = 0.1L + position * 1e-5L;
                            assert(std::abs(output[frame] - expected) < 0.00001L);
                        }
                    }
                    // A real sample-counter gap invalidates the old generation.
                    const auto before = history.status().discontinuities;
                    assert(history.append({planes, period}, period,
                                          origin + frameNs(257 * period, rate) + shift, rate,
                                          257 * period, true));
                    if (verified) {
                        assert(history.status().discontinuities == before + 1);
                        assert(history
                                   .read(origin + frameNs(127 * period, rate), rate,
                                         {destination, 256}, 256, cursor)
                                   .missingFrames == 256);
                    }
                }
            }
        }
    }
}
void historyReadFailureDiagnostics() {
    constexpr uint64_t origin = 4000000000;
    constexpr double rate = 48000;
    AudioHistory history(1, 512, ResamplingFilter::LinearPhase, true);
    Planes input(1, 256), output(1, 64);
    std::fill(input.values.begin(), input.values.end(), .25f);
    AudioReadCursor cursor;
    const auto read = [&](uint64_t time, double outputRate = 48000.0) {
        return realtime([&] {
            return history.read(time, outputRate, output.output(), 64, cursor);
        });
    };
    const auto reason = [](const auto& snapshot, AudioHistoryMissingReason category) {
        return snapshot.missingFrames[static_cast<size_t>(category)];
    };
    assert(read(origin).missingFrames == 64);
    assert(reason(history.readDiagnostics(), AudioHistoryMissingReason::Offline) == 64);
    assert(realtime([&] {
        return history.append(input.input(), 256, origin, rate, 0, true);
    }));
    assert(read(origin + frameNs(64, rate)).missingFrames == 0);
    assert(read(origin + frameNs(320, rate)).missingFrames == 64);
    auto observed = history.readDiagnostics();
    assert(reason(observed, AudioHistoryMissingReason::ResamplingSupport) == 64);
    assert(observed.unpublishedSampleReads == 64 && !observed.expiredSampleReads);
    for (uint64_t block = 1; block < 3; ++block)
        assert(history.append(input.input(), 256, origin + frameNs(block * 256, rate), rate,
                              static_cast<double>(block * 256), true));
    assert(read(origin + frameNs(64, rate)).missingFrames == 64);
    observed = history.readDiagnostics();
    assert(observed.expiredSampleReads == 64 && !observed.absentSampleReads);
    assert(reason(observed, AudioHistoryMissingReason::ResamplingSupport) == 128);
    assert(read(origin, NAN).missingFrames == 64);
    assert(reason(history.readDiagnostics(), AudioHistoryMissingReason::InvalidRequest) == 64);
    // Stop must preserve lifetime evidence; valid silence must not be counted
    // as missing audio. Independent readers update the optional counters safely.
    std::fill(input.values.begin(), input.values.end(), 0.0f);
    history.stop();
    assert(history.append(input.input(), 256, origin + 1000000000, rate, 0, true));
    const auto before = history.readDiagnostics();
    const auto independentReader = [&] {
        Planes destination(1, 64);
        AudioReadCursor independent;
        for (unsigned index = 0; index < 1000; ++index) {
            assert(!realtime([&] {
                        return history.read(origin + 1000000000 + frameNs(64, rate), rate,
                                            destination.output(), 64, independent);
                    }).missingFrames);
            assert(
                std::all_of(destination.values.begin(), destination.values.end(), [](float value) {
                    return value == 0;
                }));
        }
    };
    std::thread first(independentReader), second(independentReader);
    first.join();
    second.join();
    observed = history.readDiagnostics();
    assert(observed.enabled && observed.reads - before.reads == 2000);
    assert(observed.requestedFrames - before.requestedFrames == 128000);
    assert(observed.missingFrames == before.missingFrames);
    // Continuous sample counters do not justify an unbounded host-clock jump.
    // This still fails as a clock mapping, without blaming late sample delivery.
    AudioHistory clock(1, 1024, ResamplingFilter::LinearPhase, true);
    assert(clock.append(input.input(), 256, origin, rate, 0, true));
    assert(
        clock.append(input.input(), 256, origin + frameNs(256, rate) + 1000000, rate, 256, true));
    assert(realtime([&] {
               return clock.read(origin + frameNs(64, rate), rate, output.output(), 64, cursor);
           }).missingFrames == 64);
    observed = clock.readDiagnostics();
    assert(reason(observed, AudioHistoryMissingReason::ClockMapping) == 64);
    assert(!reason(observed, AudioHistoryMissingReason::ResamplingSupport));
    assert(!observed.unpublishedSampleReads && !observed.expiredSampleReads);
    AudioHistory disabled(1);
    assert(disabled.read(origin, rate, output.output(), 64, cursor).missingFrames == 64);
    assert(!disabled.readDiagnostics().enabled && !disabled.readDiagnostics().reads);
}
void drift() {
    for (double actualRate : {47995.2, 48004.8}) {
        AudioHistory history(2);
        Planes input(2, 256), output(2, 256);
        constexpr uint64_t origin = 9000000000ULL;
        uint64_t latest = 0;
        for (uint32_t block = 0; block < 600; ++block) {
            for (uint32_t channel = 0; channel < 2; ++channel)
                for (uint32_t frame = 0; frame < 256; ++frame)
                    input.write[channel][frame] = static_cast<float>(
                        tone(block * 256 + frame, actualRate, 1000 + channel * 37));
            latest = origin + static_cast<uint64_t>(block * 256 * 1e9L / actualRate);
            assert(realtime([&] {
                return history.append(input.input(), 256, latest, 48000, block * 256, true);
            }));
        }
        AudioReadCursor cursor;
        const auto first = latest - frameNs(1024, 48000);
        assert(!realtime([&] {
                    return history.read(first, 48000, output.output(), 256, cursor);
                }).missingFrames);
        double error = 0, energy = 0;
        for (uint32_t frame = 0; frame < 256; ++frame) {
            const auto expected = tone(
                (first - origin) * actualRate / 1e9 + frame * actualRate / 48000, actualRate, 1000);
            error += std::pow(output.write[0][frame] - expected, 2);
            energy += expected * expected;
        }
        assert(10 * std::log10(energy / error) > 90);
    }
}
void finiteContainment() {
    Fixture fixture(2, 48000, 1000);
    for (uint32_t channel = 0; channel < 2; ++channel)
        for (uint32_t frame = 0; frame < kCapacity; ++frame)
            fixture.input.write[channel][frame] =
                frame < 4096 ? 0 : (channel ? -1 : 1) * std::numeric_limits<float>::max();
    fixture.history.stop();
    assert(
        fixture.history.append(fixture.input.input(), kCapacity, fixture.origin, 48000, 0, true));
    AudioReadCursor cursor;
    assert(!realtime([&] {
                return fixture.history.read(fixture.start(), 44100, fixture.output.output(), 256,
                                            cursor);
            }).missingFrames);
    for (uint32_t channel = 0; channel < 2; ++channel)
        for (uint32_t frame = 0; frame < 256; ++frame)
            assert(std::isfinite(fixture.output.write[channel][frame]));
}
ResampleFrame rawFrame(const void* context, uint64_t position, std::span<float> values) noexcept {
    const auto& input = *static_cast<const Planes*>(context);
    if (position >= input.frames || values.size() > input.channels)
        return {false, 0};
    for (size_t channel = 0; channel < values.size(); ++channel)
        values[channel] = input.read[channel][position];
    return {true, input.channels};
}
ResampleFrame partialRawFrame(const void* context, uint64_t position,
                              std::span<float> values) noexcept {
    auto result = rawFrame(context, position, values);
    result.availableChannels = 2;
    return result;
}
void stridedOutputAndInvalidRequests() {
    Fixture fixture(8, 48000, 1000);
    AudioResampler reader;
    constexpr uint32_t frames = 128;
    std::array<float, frames * 8 + 2> interleaved{};
    std::array<float*, 8> planes{};
    for (uint32_t channel = 0; channel < planes.size(); ++channel)
        planes[channel] = interleaved.data() + 1 + channel;
    const ResampleOutput output{planes, frames, 8};
    const ResamplePosition position{0, 4096.37L, 48000, 44100};
    interleaved.fill(99);
    assert(realtime([&] {
               return reader.read(position, output, frames, rawFrame, &fixture.input);
           }) == 0);
    assert(interleaved.front() == 99 && interleaved.back() == 99);
    for (uint32_t frame = 0; frame < frames; ++frame)
        for (uint32_t channel = 0; channel < planes.size(); ++channel) {
            const auto expected =
                tone(4096.37 + frame * 48000.0 / 44100, 48000, 1000 + channel * 37);
            assert(std::abs(interleaved[1 + frame * 8 + channel] - expected) < 0.00001);
        }
    auto invalid = position;
    for (const auto unsupported : {7959.0, 192961.0}) {
        invalid.sourceRate = unsupported;
        assert(reader.read(invalid, output, frames, rawFrame, &fixture.input) == frames);
    }
    invalid = position;
    invalid.offset = std::numeric_limits<long double>::quiet_NaN();
    assert(reader.read(invalid, output, frames, rawFrame, &fixture.input) == frames);
    assert(interleaved.front() == 99 && interleaved.back() == 99);
    assert(std::all_of(interleaved.begin() + 1, interleaved.end() - 1, [](float value) {
        return value == 0;
    }));
    assert(reader.read(position, output, AudioResampler::kMaximumFrames + 1, rawFrame,
                       &fixture.input) == AudioResampler::kMaximumFrames + 1);
    auto badStride = output;
    badStride.stride = 0;
    interleaved.fill(99);
    assert(reader.read(position, badStride, frames, rawFrame, &fixture.input) == frames);
    assert(std::all_of(interleaved.begin(), interleaved.end(), [](float value) {
        return value == 99;
    }));
    fixture.input.write[0][4096] = NAN;
    assert(reader.read({0, 4096, 48000, 48000}, output, frames, rawFrame, &fixture.input) == 0);
    assert(interleaved[1] == 0 && interleaved.front() == 99 && interleaved.back() == 99);
    // The provider deliberately leaves nonzero data in unavailable tail planes.
    // The shared reader must preserve present channels and silence that tail.
    assert(reader.read(position, output, frames, partialRawFrame, &fixture.input) == frames);
    assert(interleaved[2] != 0 && interleaved.front() == 99 && interleaved.back() == 99);
    for (uint32_t frame = 0; frame < frames; ++frame)
        for (uint32_t channel = 2; channel < 8; ++channel)
            assert(interleaved[1 + frame * 8 + channel] == 0);
}
void benchmark(bool callbackAnchors = false, double driftPpm = 0) {
    for (uint32_t channels : {2U, 8U, 64U})
        for (const auto& rates : {std::pair{48000U, 48000U}, std::pair{48000U, 44100U},
                                  std::pair{96000U, 48000U}, std::pair{192000U, 8000U}}) {
            Fixture fixture(channels, rates.first, 1000);
            if (callbackAnchors) {
                fixture.history.stop();
                std::array<const float*, 64> inputs{};
                for (uint32_t offset = 0; offset < kCapacity; offset += 64) {
                    for (uint32_t channel = 0; channel < channels; ++channel)
                        inputs[channel] = fixture.input.read[channel] + offset;
                    assert(fixture.history.append(
                        {{inputs.data(), channels}, 64}, 64,
                        fixture.origin + frameNs(offset, rates.first * (1 + driftPpm * 1e-6)),
                        rates.first, offset, true));
                }
            }
            AudioReadCursor cursor;
            std::array<double, 20> timings{};
            for (uint32_t block = 0; block < 24; ++block) {
                const auto start = std::chrono::steady_clock::now();
                const auto result = fixture.history.read(fixture.start(false), rates.second,
                                                         fixture.output.output(), 64, cursor);
                assert(!result.missingFrames);
                if (block >= 4)
                    timings[block - 4] = std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now() - start)
                                             .count();
            }
            std::sort(timings.begin(), timings.end());
            std::printf("{\"channels\":%u,\"sourceRate\":%u,\"outputRate\":%u,\"frames\":64,"
                        "\"medianMicroseconds\":%.3f,\"maximumMicroseconds\":%.3f,"
                        "\"periodMicroseconds\":%.3f}\n",
                        channels, rates.first, rates.second, timings[10], timings.back(),
                        64e6 / rates.second);
        }
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        benchmark();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark-callback-clock") {
        benchmark(true);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark-drifting-clock") {
        benchmark(true, 200);
        return 0;
    }
    assert(argc == 1);
    spectralQuality();
    sparseChannelsMatchFullHistory();
    boundariesAndPartitioning();
    contiguousHardwareTimestampCorrections();
    historyReadFailureDiagnostics();
    drift();
    finiteContainment();
    stridedOutputAndInvalidRequests();
    std::puts("Bandlimited history conversion: spectrum, wide channels, partitioning, drift, "
              "failure silence and no callback heap activity passed");
}
