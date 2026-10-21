#include <loopz/output_clock.hpp>
#include <loopz/timeline.hpp>

#include <cassert>
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace loopz;
void driftingClock(double nominal, double ppm, uint32_t frames, uint64_t originFrame,
                   uint64_t originNs) {
    OutputClock clock;
    const auto actual = nominal * (1 + ppm / 1e6);
    double maximumPpm = 0;
    for (uint64_t index = 0; index < 2000; ++index) {
        const auto frame = index * frames;
        const auto host = originNs + frameNs(frame, actual);
        const auto rate = clock.observe(originFrame + frame, host, frames, nominal);
        assert(std::isfinite(rate) && rate >= nominal * 0.995 && rate <= nominal * 1.005);
        if (frameNs(frame, actual) > 100000000)
            maximumPpm = std::max(maximumPpm, std::abs(rate / actual - 1) * 1e6);
    }
    std::printf("clock %.0f Hz, %.0f ppm, %u frames: maximum settled error %.6f ppm\n", nominal,
                ppm, frames, maximumPpm);
    assert(maximumPpm < 0.05);
}
void restartAndMalformedAnchors() {
    OutputClock clock;
    uint64_t frame = 0, host = 1000000000;
    for (; frame < 48000; frame += 64)
        clock.observe(frame, host + frameNs(frame, 48000 * 1.0002), 64, 48000);
    host += frameNs(frame, 48000 * 1.0002);
    assert(clock.observe(frame, host, 64, 48000) > 48009);
    // A reused timestamp/position, a gap, explicit stream discontinuity and
    // format replacement must never retain the old endpoint's measured rate.
    assert(clock.observe(0, 1000000000, 64, 48000) == 48000);
    assert(clock.observe(64, 2000000000, 64, 48000) == 48000);
    assert(clock.observe(128, 2001333333, 64, 48000, true) == 48000);
    assert(clock.observe(192, 2002666666, 64, 44100) == 44100);
    assert(clock.observe(UINT64_MAX - 10, 3000000000, 64, 48000) == 0);
    assert(clock.observe(0, 0, 64, 48000) == 0);
    assert(clock.observe(0, 1, 0, 48000) == 0);
    assert(clock.observe(0, 1, 64, std::numeric_limits<double>::quiet_NaN()) == 0);
    assert(clock.observe(0, 1, 64, 7999) == 0);
    assert(clock.observe(0, 1, 64, 192001) == 0);
    assert(clock.observe(0, 1, 64, 8000) == 8000);
    clock.reset();
    assert(clock.observe(0, 1, 64, 192000) == 192000);
}
void changingRateAndQuantization() {
    OutputClock clock;
    constexpr double nominal = 48000;
    long double exactNs = 1000000000;
    double worstSettled = 0;
    for (uint64_t index = 0; index < 1600; ++index) {
        const auto actual = nominal * (index < 800 ? 1.0002 : 0.9998);
        // A 24 MHz hardware counter has a 41.67 ns quantum. Estimate from
        // timestamp progression, never from callback scheduling/arrival time.
        const auto host = static_cast<uint64_t>(std::round(exactNs * 24 / 1000) * 1000 / 24);
        const auto rate = clock.observe(index * 64, host, 64, nominal);
        if ((index > 80 && index < 800) || index > 880)
            worstSettled = std::max(worstSettled, std::abs(rate / actual - 1) * 1e6);
        if (index > 840)
            assert(rate < nominal);
        exactNs += 64 * 1e9L / actual;
    }
    std::printf("42 ns timestamp quantization and +200/-200 ppm reversal: %.6f ppm\n",
                worstSettled);
    assert(worstSettled < 2);
}
} // namespace
int main() {
    for (const auto rate : {8000., 48000., 192000.})
        for (const auto ppm : {-200., 200.}) {
            driftingClock(rate, ppm, 64, 0, 1000000000);
            driftingClock(rate, ppm, 257, (uint64_t{1} << 55) + 1234, 4000000000000000000ULL);
        }
    restartAndMalformedAnchors();
    changingRateAndQuantization();
    std::puts("Output clock drift, reset, precision and quantization tests passed");
}
