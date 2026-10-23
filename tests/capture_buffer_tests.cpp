#include <loopz/capture_buffer.hpp>
#include <loopz/audio_delivery_timing.hpp>
#include <loopz/capture_mix.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
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
template <typename Operation> auto realtime(Operation operation) {
    trackHeap = true;
    heapEvents = 0;
    auto result = operation();
    trackHeap = false;
    assert(heapEvents == 0);
    return result;
}
double tone(double position, double rate, double frequency) {
    return 0.25 * std::sin(2 * std::numbers::pi_v<double> * frequency * position / rate);
}
void deliveryTimingObservations() {
    AudioDeliveryTiming timing(1000);
    realtime([&] {
        timing.observe(100000, 64, 64000, 1100000, 1100200);
        timing.observe(101000, 128, 64000, 1101000, 1101400);
        return 0;
    });
    auto value = timing.snapshot();
    assert(value.warmupCallbacks == 1 && value.callbacks == 1 && value.frames == 128);
    assert(value.minimumFrames == 128 && value.maximumFrames == 128);
    assert(value.minimumEntryAgeNs == 1000000 && value.maximumEntryAgeNs == 1000000);
    assert(value.minimumReadyEndAgeNs == -999600 && value.maximumProcessingNs == 400);
    assert(value.minimumGapNs == 1000 && value.maximumGapNs == 1000);
    // Callback size and publication age vary independently of a requested
    // period. Future-dated samples remain negative, rather than wrapping.
    timing.observe(3000000, 32, 64000, 1103000, 1103500);
    value = timing.snapshot();
    assert(value.callbacks == 2 && value.frames == 160);
    assert(value.minimumFrames == 32 && value.maximumFrames == 128);
    assert(value.minimumEntryAgeNs == -1897000 && value.maximumEntryAgeNs == 1000000);
    assert(value.minimumReadyEndAgeNs == -2396500 && value.maximumReadyEndAgeNs == -999600);
    assert(value.minimumGapNs == 1000 && value.maximumGapNs == 2000);
    assert(value.maximumProcessingNs == 500);
    timing.observe(3000000, 32, 64000, 1103000, 1103500); // Repeated receipt clock.
    timing.observe(1, 0, 48000, 1104000, 1104001);
    timing.observe(1, 64, NAN, 1104000, 1104001);
    timing.observe(1, 64, 48000, 1104000, 1103999);
    timing.observe(UINT64_MAX, 64, 48000, 1104000, 1104001);
    timing.observe(1, 64, 48000, UINT64_MAX - 1, UINT64_MAX);
    assert(timing.snapshot().rejected == 6 && timing.snapshot().callbacks == 2);
    timing.reset();
    value = timing.snapshot();
    assert(!value.callbacks && !value.frames && !value.rejected && !value.warmupCallbacks);
    assert(!value.minimumReadyEndAgeNs && !value.maximumReadyEndAgeNs);
    AudioDeliveryTiming immediate(0);
    immediate.observe(100000, 64, 64000, 1200000, 1200500);
    value = immediate.snapshot();
    assert(value.callbacks == 1 && value.minimumReadyEndAgeNs == 100500);
    assert(!value.minimumGapNs && !value.maximumGapNs);
    immediate.observe(200000, 4097, 48000, 2200000, 2200500);
    assert(immediate.snapshot().callbacks == 1 && immediate.snapshot().rejected == 1);

    AudioDeliveryTiming anchors(0);
    constexpr uint64_t origin = 1000000000;
    realtime([&] {
        anchors.observe(origin, 32, 48000, origin + 2200000, origin + 2201000, 0, true);
        anchors.observe(origin + 2000000, 32, 48000, origin + 4200000, origin + 4201000, 32, true);
        anchors.observe(origin + 2666667, 32, 48000, origin + 4866667, origin + 4867667, 96, true);
        return 0;
    });
    const auto observed = anchors.snapshot();
    assert(observed.unsupportedAnchorIntervals == 1);
    assert(observed.maximumAnchorErrorNs == 1333334);
    assert(observed.sourceFrameDiscontinuities == 1);
    assert(observed.eventCount == 2);
    assert(observed.events[0].hostIntervalNs == 2000000);
    assert(observed.events[0].sourceFrameDelta == 32);
    assert(observed.events[1].sourceFrameDelta == 64);
    assert(observed.events[1].expectedSourceFrameDelta == 32);
    assert(observed.events[1].processingNs == 1000);
    // Repeated/backward sample times have a different signed signature.
    anchors.observe(origin + 3333334, 32, 48000, origin + 5533334, origin + 5534334, 64, true);
    assert(anchors.snapshot().events[2].sourceFrameDelta == -32);
    for (uint32_t i = 0; i < 32; ++i)
        realtime([&] {
            anchors.observe(origin + 4000000 + i * 1000000, 32, 48000,
                            origin + 6200000 + i * 1000000, origin + 6201000 + i * 1000000,
                            1000 + i * 64, true);
            return 0;
        });
    assert(anchors.snapshot().eventCount == observed.events.size());
    anchors.reset();
    assert(!anchors.snapshot().maximumAnchorErrorNs &&
           !anchors.snapshot().unsupportedAnchorIntervals &&
           !anchors.snapshot().sourceFrameDiscontinuities && !anchors.snapshot().eventCount);
}
void audioHistoryCoverageAndMix() {
    constexpr uint64_t origin = 7000000000ULL;
    constexpr double rate = 48000;
    AudioHistory a(2), b(2), silent(2);
    std::array<float, kBlock> inputL{}, inputR{}, left{}, right{};
    const std::array<const float*, 2> input{inputL.data(), inputR.data()};
    inputL.fill(0.25f);
    inputR.fill(-0.25f);
    assert(a.append({input, kBlock}, kBlock, origin, rate, 0, true));
    inputL.fill(0.5f);
    inputR.fill(-0.5f);
    assert(b.append({input, kBlock}, kBlock, origin + frameNs(128, rate), rate, 128, true));
    const AudioHistory* histories[]{&a, &b};
    AudioHistoryMixReader first, independent;
    const auto time = origin + frameNs(64, rate);
    const auto result = realtime([&] {
        return sampleAudioHistoryMix(histories, time, rate, left.data(), right.data(), kBlock,
                                     first);
    });
    assert(result.online && result.missingFrames == 128);
    for (uint32_t frame = 0; frame < kBlock; ++frame) {
        const auto expected = frame < 64 ? 0.25f : frame < 192 ? 0.75f : 0.5f;
        assert(std::abs(left[frame] - expected) < 0.00001f &&
               std::abs(right[frame] + expected) < 0.00001f);
    }
    assert(
        sampleAudioHistoryMix(histories, time, rate, left.data(), right.data(), kBlock, independent)
            .missingFrames == 128);
    inputL.fill(0);
    inputR.fill(0);
    assert(silent.append({input, kBlock}, kBlock, origin, rate, 0, true));
    const AudioHistory* withSilence[]{&a, &silent};
    AudioHistoryMixReader zeroReader;
    assert(sampleAudioHistoryMix(withSilence, origin, rate, left.data(), right.data(), kBlock,
                                 zeroReader)
               .missingFrames == 0);
    for (auto value : left)
        assert(value == 0.25f);
    b.stop();
    assert(sampleAudioHistoryMix(histories, time, rate, left.data(), right.data(), kBlock, first)
               .missingFrames == kBlock);
    assert(
        sampleAudioHistoryMix({}, time, rate, left.data(), right.data(), 64, first).missingFrames ==
        64);
    for (unsigned frame = 0; frame < 64; ++frame)
        assert(left[frame] == 0 && right[frame] == 0);
}
void captureCoverageAndMix() {
    constexpr uint64_t origin = 7000000000ULL;
    constexpr double rate = 48000;
    CaptureBuffer a, b, silent;
    std::array<float, kBlock> inputL{}, inputR{}, left{}, right{};
    inputL.fill(0.25f);
    inputR.fill(-0.25f);
    assert(a.append(inputL.data(), inputR.data(), kBlock, origin, rate, 0));
    inputL.fill(0.5f);
    inputR.fill(-0.5f);
    b.append(inputL.data(), inputR.data(), kBlock, origin + frameNs(128, rate), rate, 128);
    const CaptureBuffer* captures[]{&a, &b};
    CaptureMixReader first, independent;
    // The requested window has 64 frames missing from each stream at opposite
    // ends: the union is 128, not max(64,64) or a repeated zero-value heuristic.
    const auto time = origin + frameNs(64, rate);
    assert(realtime([&] {
               return sampleCaptureMix(captures, time, rate, left.data(), right.data(), kBlock,
                                       first);
           }) == 128);
    for (uint32_t frame = 0; frame < kBlock; ++frame) {
        const auto expected = frame < 64 ? 0.25f : frame < 192 ? 0.75f : 0.5f;
        assert(std::abs(left[frame] - expected) < 0.00001f);
        assert(std::abs(right[frame] + expected) < 0.00001f);
    }
    assert(sampleCaptureMix(captures, time, rate, left.data(), right.data(), kBlock, independent) ==
           128);
    std::array<uint8_t, kBlock + 2> coverage{};
    coverage.fill(7);
    CaptureBuffer::ReaderState cache;
    assert(a.sample(time, rate, left.data(), right.data(), kBlock, &cache,
                    {coverage.data() + 1, kBlock}) == 64);
    assert(coverage.front() == 7 && coverage.back() == 7);
    for (uint32_t frame = 0; frame < kBlock; ++frame)
        assert(coverage[frame + 1] == (frame < 192 ? 1 : 0));
    inputL.fill(0);
    inputR.fill(0);
    silent.append(inputL.data(), inputR.data(), kBlock, origin, rate, 0);
    const CaptureBuffer* withSilence[]{&a, &silent};
    CaptureMixReader silenceReader;
    assert(sampleCaptureMix(withSilence, origin, rate, left.data(), right.data(), kBlock,
                            silenceReader) == 0);
    for (auto value : left)
        assert(value == 0.25f);
    b.stop();
    assert(sampleCaptureMix(captures, time, rate, left.data(), right.data(), kBlock, first) ==
           kBlock);
    for (uint32_t frame = 0; frame < kBlock; ++frame)
        assert(left[frame] == (frame < 192 ? 0.25f : 0));
    assert(a.sample(origin, rate, left.data(), right.data(), 65, nullptr,
                    {coverage.data() + 1, 64}) == 65);
    assert(coverage.front() == 7 && coverage.back() == 7);
    for (uint32_t frame = 1; frame <= 64; ++frame)
        assert(coverage[frame] == 0);
    const CaptureBuffer* invalid[]{&a, nullptr};
    assert(sampleCaptureMix(invalid, time, rate, left.data(), right.data(), 64, first) == 64);
    assert(sampleCaptureMix({}, time, rate, left.data(), right.data(), 64, first) == 64);

    CaptureBuffer slow, fast;
    std::array<float, 4096> sourceL{}, sourceR{};
    for (auto* stream : {&slow, &fast}) {
        const double sourceRate = stream == &slow ? 44100 : 48000;
        sourceL.fill(stream == &slow ? 0.25f : 0.5f);
        sourceR.fill(stream == &slow ? -0.25f : -0.5f);
        for (uint32_t offset = 0; offset < 8192; offset += 4096)
            stream->append(sourceL.data(), sourceR.data(), 4096,
                           origin + frameNs(offset, sourceRate), sourceRate, offset);
    }
    const CaptureBuffer* clocks[]{&slow, &fast};
    CaptureMixReader clocksReader;
    assert(realtime([&] {
               return sampleCaptureMix(clocks, origin + frameNs(4096, rate), rate, left.data(),
                                       right.data(), kBlock, clocksReader);
           }) == 0);
    for (uint32_t frame = 0; frame < kBlock; ++frame) {
        assert(std::abs(left[frame] - 0.75f) < 0.00001f);
        assert(std::abs(right[frame] + 0.75f) < 0.00001f);
    }
}
void concurrentCaptureMix() {
    constexpr uint64_t origin = 11000000000ULL, total = 16384;
    CaptureBuffer a, b;
    std::array<float, 4096> seedL{}, seedR{};
    for (auto* capture : {&a, &b}) {
        seedL.fill(capture == &a ? 0.2f : 0.3f);
        seedR.fill(capture == &a ? -0.2f : -0.3f);
        capture->append(seedL.data(), seedR.data(), 4096, origin, 48000, 0);
    }
    CaptureMixReader readers[2];
    const CaptureBuffer* captures[]{&a, &b};
    std::array<float, kBlock> seedOutputL{}, seedOutputR{};
    for (auto& reader : readers)
        assert(sampleCaptureMix(captures, origin + frameNs(2048, 48000), 48000, seedOutputL.data(),
                                seedOutputR.data(), kBlock, reader) == 0);
    // Prime a valid clock snapshot before deliberately running producers with
    // no real-time callback idle interval. This tests retained-anchor fallback
    // under sustained publication contention, separately from startup warmup.
    std::atomic<uint64_t> written[2]{{4096}, {4096}}, consumed[2]{{2048}, {2048}};
    std::atomic<uint64_t> verified[2]{};
    const auto producer = [&](unsigned index, CaptureBuffer& capture) {
        std::array<float, 64> left{}, right{};
        left.fill(index ? 0.3f : 0.2f);
        right.fill(index ? -0.3f : -0.2f);
        for (uint64_t frame = 4096; frame < total; frame += 64) {
            while (frame > std::min(consumed[0].load(), consumed[1].load()) + 4096)
                std::this_thread::yield();
            realtime([&] {
                capture.append(left.data(), right.data(), 64, origin + frameNs(frame, 48000), 48000,
                               static_cast<double>(frame));
                return true;
            });
            written[index].store(frame + 64);
        }
    };
    const auto consumer = [&](unsigned index) {
        auto& reader = readers[index];
        std::array<float, kBlock> left{}, right{};
        for (uint64_t frame = 2048; frame < total - 2048; frame += 64) {
            while (std::min(written[0].load(), written[1].load()) < frame + 512)
                std::this_thread::yield();
            const auto missing = realtime([&] {
                return sampleCaptureMix(captures, origin + frameNs(frame, 48000), 48000,
                                        left.data(), right.data(), kBlock, reader);
            });
            assert(missing == 0);
            ++verified[index];
            for (uint32_t sample = 0; sample < kBlock; ++sample) {
                assert(std::abs(left[sample] - 0.5f) < 0.00001f);
                assert(std::abs(right[sample] + 0.5f) < 0.00001f);
            }
            consumed[index].store(frame + 64);
        }
        consumed[index].store(total);
    };
    std::thread first(producer, 0, std::ref(a)), second(producer, 1, std::ref(b));
    std::thread readerA(consumer, 0), readerB(consumer, 1);
    first.join();
    second.join();
    readerA.join();
    readerB.join();
    assert(verified[0].load() == 192 && verified[1].load() == 192);
}
void stereoSpectrum() {
    struct Case {
        uint32_t source, output;
        double frequency;
        bool stopband;
    };
    for (const auto value : {Case{48000, 48000, 20000, false}, Case{48000, 44100, 20000, false},
                             Case{44100, 48000, 19000, false}, Case{96000, 48000, 20000, false},
                             Case{48000, 8000, 3200, false}, Case{48000, 44100, 22100, true},
                             Case{96000, 48000, 24100, true}, Case{48000, 8000, 4400, true}}) {
        CaptureBuffer capture;
        constexpr uint32_t inputFrames = 8192;
        std::vector<float> left(inputFrames), right(inputFrames);
        for (uint32_t frame = 0; frame < inputFrames; ++frame) {
            left[frame] = static_cast<float>(tone(frame, value.source, value.frequency));
            right[frame] = static_cast<float>(tone(frame, value.source, value.frequency + 17));
        }
        constexpr uint64_t origin = 9000000000ULL;
        for (uint32_t offset = 0; offset < inputFrames; offset += 4096) {
            const auto callbacks = capture.callbacks.load();
            realtime([&] {
                capture.append(left.data() + offset, right.data() + offset, 4096,
                               origin + frameNs(offset, value.source), value.source, offset);
                return true;
            });
            assert(capture.callbacks.load() == callbacks + 1);
        }
        std::array<float, kBlock + 2> a{}, b{}, againA{}, againB{};
        CaptureBuffer::ReaderState firstReader, secondReader;
        const uint64_t first = origin + static_cast<uint64_t>(4096.37L * 1e9L / value.source);
        double minimumSnr = 300, maximumGain = -300;
        a.fill(99);
        b.fill(99);
        assert(realtime([&] {
                   return capture.sample(first, value.output, a.data() + 1, b.data() + 1, kBlock,
                                         &firstReader);
               }) == 0);
        assert(a.front() == 99 && a.back() == 99 && b.front() == 99 && b.back() == 99);
        assert(capture.sample(first + frameNs(64, value.output), value.output, againA.data(),
                              againB.data(), 64, &secondReader) == 0);
        assert(capture.sample(first, value.output, againA.data(), againB.data(), kBlock,
                              &firstReader) == 0);
        for (uint32_t frame = 0; frame < kBlock; ++frame) {
            assert(a[frame + 1] == againA[frame] && b[frame + 1] == againB[frame]);
        }
        for (uint32_t channel = 0; channel < 2; ++channel) {
            double energy = 0, error = 0, reference = 0;
            for (uint32_t frame = 0; frame < kBlock; ++frame) {
                // The final callback supplies the frame-4096 host anchor. Its
                // integer-nanosecond rounding is part of the input clock.
                const auto position =
                    4096 +
                    double(first - origin - frameNs(4096, value.source)) * value.source / 1e9 +
                    double(frame) * value.source / value.output;
                const auto expected = tone(position, value.source, value.frequency + channel * 17);
                const auto actual = channel ? b[frame + 1] : a[frame + 1];
                assert(std::isfinite(actual));
                energy += actual * actual;
                reference += expected * expected;
                error += (actual - expected) * (actual - expected);
            }
            minimumSnr = std::min(minimumSnr, 10 * std::log10(reference / std::max(error, 1e-30)));
            maximumGain =
                std::max(maximumGain, 10 * std::log10(std::max(energy, 1e-30) / reference));
        }
        std::printf("stereo %u -> %u, %.0f Hz: SNR %.2f dB, gain %.5f dB\n", value.source,
                    value.output, value.frequency, minimumSnr, maximumGain);
        assert(value.stopband ? maximumGain < -90 : minimumSnr > 90);
    }
}
void stopRestartAndMissingHistory() {
    CaptureBuffer capture;
    std::array<float, 4096> inputL{}, inputR{};
    inputL.fill(0.5f);
    inputR.fill(-0.5f);
    constexpr uint64_t origin = 1000000000ULL;
    capture.append(inputL.data(), inputR.data(), 4096, origin, 48000, 0);
    CaptureBuffer::ReaderState cached, independent;
    std::array<float, kBlock + 2> left{}, right{};
    const auto first = origin + frameNs(2048, 48000);
    assert(capture.sample(first, 44100, left.data(), right.data(), kBlock, &cached) == 0);
    assert(std::abs(left[0] - 0.5f) < 1e-6 && std::abs(right[0] + 0.5f) < 1e-6);
    const auto oldGeneration = cached.generation;
    // A fractional conversion without its past filter wing must stay silent.
    assert(capture.sample(origin, 44100, left.data(), right.data(), 64, &independent) == 64);
    assert(std::all_of(left.begin(), left.begin() + 64, [](auto v) {
        return v == 0;
    }));
    realtime([&] {
        capture.stop();
        return true;
    });
    assert(capture.clock().anchorNs == 0 && capture.clock().measuredRate == 0);
    assert(capture.sample(first, 44100, left.data(), right.data(), kBlock, &cached) == kBlock);
    assert(std::all_of(left.begin(), left.begin() + kBlock, [](auto v) {
        return v == 0;
    }));
    inputL.fill(0);
    inputR.fill(0);
    // The same host/sample timestamps must not revive the prior generation.
    capture.append(inputL.data(), inputR.data(), 4096, origin, 48000, 0);
    assert(capture.sample(first, 44100, left.data(), right.data(), kBlock, &cached) == 0);
    assert(cached.generation != oldGeneration);
    assert(std::all_of(left.begin(), left.begin() + kBlock, [](auto v) {
        return v == 0;
    }));
    assert(std::all_of(right.begin(), right.begin() + kBlock, [](auto v) {
        return v == 0;
    }));
    const auto callbacks = capture.callbacks.load();
    assert(!capture.append(inputL.data(), inputR.data(), 4096, origin, NAN));
    assert(!capture.append(nullptr, inputR.data(), 4096, origin, 48000));
    assert(!capture.append(inputL.data(), inputR.data(), 4097, origin, 48000));
    assert(capture.callbacks.load() == callbacks);
    left.fill(99);
    right.fill(99);
    assert(capture.sample(first, 44100, left.data() + 1, right.data() + 1, kBlock + 1) ==
           kBlock + 1);
    assert(left.front() == 99 && left.back() == 99 && right.front() == 99 && right.back() == 99);
    assert(capture.sample(first, 44100, nullptr, right.data() + 1, 64) == 64);
    assert(right.front() == 99 && right[65] == 0);
    assert(capture.sample(first, NAN, left.data(), right.data(), 64) == 64);
    assert(std::all_of(left.begin(), left.begin() + 64, [](auto v) {
        return v == 0;
    }));
}
} // namespace
int main() {
    deliveryTimingObservations();
    captureCoverageAndMix();
    audioHistoryCoverageAndMix();
    concurrentCaptureMix();
    stereoSpectrum();
    stopRestartAndMissingHistory();
    std::puts("Stereo capture spectrum, independent readers, stop/restart, bounds and callback "
              "heap checks passed");
}
