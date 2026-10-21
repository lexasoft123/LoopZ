#include <loopz/audio_history.hpp>
#include <loopz/capture_buffer.hpp>
#include <loopz/timeline.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {
using namespace loopz;
double tone(long double seconds, uint32_t channel) {
    return static_cast<double>(0.25L * std::sin(2 * std::numbers::pi_v<long double> *
                                              (997 + channel * 74) * seconds));
}
void changingClock(uint32_t nominal, bool stereoAdapter) {
    AudioHistory history(2);
    CaptureBuffer stereo;
    const auto frames = nominal / 100;
    std::vector<float> left(frames), right(frames);
    const std::array<const float*, 2> input{left.data(), right.data()};
    std::array<float, 64> a{}, b{};
    const std::array<float*, 2> output{a.data(), b.data()};
    constexpr uint64_t origin = 9000000000ULL;
    uint64_t firstFrame = 0;
    long double elapsed = 0;
    AudioReadCursor reader;
    CaptureBuffer::ReaderState stereoReader;
    double minimumSnr = 300, maximumRateErrorPpm = 0, maximumTransitionError = 0;
    uint32_t checked = 0, streamed = 0;
    for (double ppm : {200.0, -200.0, 100.0}) {
        const auto actual = nominal * (1 + ppm * 1e-6);
        const auto phaseStart = elapsed;
        for (uint32_t block = 0; block < 250; ++block) {
            for (uint32_t frame = 0; frame < frames; ++frame) {
                const auto time = elapsed + static_cast<long double>(frame) / actual;
                left[frame] = static_cast<float>(tone(time, 0));
                right[frame] = static_cast<float>(tone(time, 1));
            }
            const auto timestamp = origin + static_cast<uint64_t>(elapsed * 1e9L);
            if (stereoAdapter)
                stereo.append(left.data(), right.data(), frames, timestamp, nominal, static_cast<double>(firstFrame));
            else
                assert(
                    history.append({input, frames}, frames, timestamp, nominal, static_cast<double>(firstFrame), true));
            if (elapsed > 0.25L && block % 10 == 0) {
                const bool settled = elapsed - phaseStart > 1.1L;
                const auto measured =
                    stereoAdapter ? stereo.clock().measuredRate : history.status().measuredRate;
                const auto rateError = std::abs(measured / actual - 1) * 1e6;
                if (settled)
                    maximumRateErrorPpm = std::max(maximumRateErrorPpm, rateError);
                if (settled && rateError > 0.02)
                    std::fprintf(stderr,
                                 "Clock failed to settle: nominal=%u stereo=%d ppm=%.0f "
                                 "phaseSeconds=%.3Lf errorPpm=%.3f\n",
                                 nominal, stereoAdapter, ppm, elapsed - phaseStart, rateError);
                assert(!settled || rateError < 0.02);
                const auto first = timestamp - frameNs(1024, nominal);
                if (stereoAdapter)
                    assert(stereo.sample(first, nominal, a.data(), b.data(), 64, &stereoReader) ==
                           0);
                else {
                    const auto result = history.read(first, nominal, {output, 64}, 64, reader);
                    assert(result.online && result.missingFrames == 0);
                }
                for (uint32_t channel = 0; channel < 2; ++channel) {
                    double error = 0, reference = 0;
                    for (uint32_t frame = 0; frame < 64; ++frame) {
                        const auto expected = tone(static_cast<long double>(first - origin) / 1e9L +
                                                       static_cast<long double>(frame) / nominal,
                                                   channel);
                        const auto value = channel ? b[frame] : a[frame];
                        assert(std::isfinite(value) && std::abs(value) < 0.251);
                        maximumTransitionError =
                            std::max(maximumTransitionError, std::abs(value - expected));
                        error += (value - expected) * (value - expected);
                        reference += expected * expected;
                    }
                    if (settled)
                        minimumSnr = std::min(minimumSnr,
                                              10 * std::log10(reference / std::max(error, 1e-30)));
                }
                checked += settled;
                ++streamed;
            }
            firstFrame += frames;
            elapsed += static_cast<long double>(frames) / actual;
        }
    }
    assert(checked >= 39 && streamed >= 70 && minimumSnr > 90 && maximumTransitionError < 0.00001);
    assert((stereoAdapter ? stereo.discontinuities.load() : history.status().discontinuities) == 0);
    std::printf("changing clock %u Hz, %s: error %.6f ppm, settled SNR %.2f dB, "
                "transition error %.6f, %u settled/%u streamed blocks\n",
                nominal, stereoAdapter ? "stereo" : "history", maximumRateErrorPpm, minimumSnr,
                maximumTransitionError, checked, streamed);
}
void continuousTransitionCoverage() {
    uint32_t caseIndex = 0;
    for (const auto& rates :
         {std::pair{8000U, 8000U}, std::pair{48000U, 48000U}, std::pair{48000U, 44100U},
          std::pair{44100U, 48000U}, std::pair{192000U, 192000U}, std::pair{192000U, 48000U},
          std::pair{48000U, 44100U}}) {
        const bool varying = caseIndex++ == 6;
        AudioHistory history(2);
        std::array<float, 64> a{}, b{}, left{}, right{};
        const std::array<const float*, 2> input{a.data(), b.data()};
        const std::array<float*, 2> output{left.data(), right.data()};
        constexpr uint64_t origin = 9000000000ULL;
        const auto frequency = std::min(20000.0, std::min(rates.first, rates.second) * 0.42);
        const auto wave = [&](long double seconds, uint32_t channel) {
            return 0.25 * std::sin(2 * std::numbers::pi_v<long double> *
                                   (frequency + channel * 17) * seconds);
        };
        const auto radius =
            std::ceil(96 / (0.94 * std::min(1.0, double(rates.second) / rates.first)));
        const auto queue = static_cast<uint32_t>(std::max(256.0, radius + 128));
        uint64_t nextOutput = (512ULL * rates.second + rates.first - 1) / rates.first;
        long double elapsed = 0;
        AudioReadCursor cursor;
        double maximumError = 0, energy = 0, error = 0, maximumBoundaryError = 0;
        std::array<double, 2> previous{}, previousExpected{};
        uint64_t measured = 0;
        for (uint32_t block = 0; block < (varying ? 1024U : 128U); ++block) {
            const auto ppm =
                varying ? 200 * std::sin(2 * std::numbers::pi_v<long double> * elapsed / 0.75L)
                        : (block < 64 ? 200 : -200);
            const auto actual = rates.first * (1 + ppm * 1e-6L);
            for (uint32_t frame = 0; frame < 64; ++frame) {
                const auto time = elapsed + static_cast<long double>(frame) / actual;
                a[frame] = static_cast<float>(wave(time, 0));
                b[frame] = static_cast<float>(wave(time, 1));
            }
            const auto timestamp = origin + static_cast<uint64_t>(elapsed * 1e9L);
            assert(history.append({input, 64}, 64, timestamp, rates.first, block * 64, true));
            const auto delay = frameNs(queue, rates.first);
            while (timestamp > origin + delay &&
                   frameNs(nextOutput + 64, rates.second) <= timestamp - origin - delay) {
                const auto first = origin + frameNs(nextOutput, rates.second);
                const auto result = history.read(first, rates.second, {output, 64}, 64, cursor);
                assert(result.online && !result.missingFrames);
                for (uint32_t frame = 0; frame < 64; ++frame)
                    for (uint32_t channel = 0; channel < 2; ++channel) {
                        const auto expected = static_cast<double>(
                            wave(static_cast<long double>(first - origin) / 1e9L +
                                     static_cast<long double>(frame) / rates.second,
                                 channel));
                        const auto value = channel ? right[frame] : left[frame];
                        assert(std::isfinite(value));
                        maximumError = std::max(maximumError, std::abs(value - expected));
                        if (measured || frame)
                            maximumBoundaryError =
                                std::max(maximumBoundaryError,
                                         std::abs((value - previous[channel]) -
                                                  (expected - previousExpected[channel])));
                        previous[channel] = value;
                        previousExpected[channel] = expected;
                        energy += expected * expected;
                        error += (value - expected) * (value - expected);
                    }
                measured += 64;
                nextOutput += 64;
            }
            elapsed += 64.0L / actual;
        }
        const auto snr = 10 * std::log10(energy / std::max(error, 1e-30));
        std::printf("continuous phase %u -> %u (%s), %.0f Hz: SNR %.2f dB, max error %.8f, "
                    "delta error %.8f, %llu frames\n",
                    rates.first, rates.second, varying ? "varying" : "step", frequency, snr,
                    maximumError, maximumBoundaryError, static_cast<unsigned long long>(measured));
        assert(measured > 1000 && maximumError < 0.0002 && maximumBoundaryError < 0.0003 &&
               snr > 80);
    }
}
void nonmonotonicTimestampAndRestart() {
    AudioHistory history(2);
    std::array<float, 64> a{}, b{};
    a.fill(0.5f);
    b.fill(-0.5f);
    const std::array<const float*, 2> input{a.data(), b.data()};
    constexpr uint64_t origin = 1000000000ULL;
    assert(history.append({input, 64}, 64, origin, 48000, 0, true));
    std::array<float, 64> earlyLeft{}, earlyRight{};
    const std::array<float*, 2> earlyOutput{earlyLeft.data(), earlyRight.data()};
    AudioReadCursor earlyCursor;
    const auto partial =
        history.read(origin - frameNs(16, 48000), 48000, {earlyOutput, 64}, 64, earlyCursor);
    assert(partial.online && partial.missingFrames == 16);
    for (uint32_t frame = 0; frame < 64; ++frame) {
        assert(earlyLeft[frame] == (frame < 16 ? 0 : 0.5f));
        assert(earlyRight[frame] == (frame < 16 ? 0 : -0.5f));
    }
    assert(history.append({input, 64}, 64, origin - 1, 48000, 64, true));
    assert(history.status().discontinuities == 1);
    AudioReadCursor cursor;
    const std::array<float*, 2> output{a.data(), b.data()};
    assert(history.read(origin - 1, 48000, {output, 64}, 64, cursor).missingFrames == 0);
    const auto generation = cursor.generation;
    history.stop();
    assert(history.append({input, 64}, 64, origin, 48000, 0, true));
    assert(history.read(origin, 48000, {output, 64}, 64, cursor).missingFrames == 0);
    assert(cursor.generation != generation);
    assert(history.status().measuredRate == 48000);
}
} // namespace
int main() {
    for (uint32_t nominal : {8000U, 48000U, 192000U}) {
        changingClock(nominal, false);
        changingClock(nominal, true);
    }
    continuousTransitionCoverage();
    nonmonotonicTimestampAndRestart();
    std::puts("Changing capture clocks, endpoint-rate drift bounds and backward timestamp recovery "
              "passed");
}
