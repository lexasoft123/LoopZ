#include <loopz/probe_signal.hpp>
#include <cassert>
#include <cstdio>
#include <memory>

std::unique_ptr<loopz::probe::StreamCheck> simulate(double rate, bool swap, bool marker, bool jump,
                                                    uint64_t delayNs = 106700000ULL) {
    auto check = std::make_unique<loopz::probe::StreamCheck>();
    check->rate = rate;
    float left[256], right[256];
    for (uint64_t frame = 0; frame < rate * 5; frame += 256) {
        const auto ns = 9000000000ULL + static_cast<uint64_t>(frame * 1e9 / rate);
        for (unsigned i = 0; i < 256; ++i) {
            const auto time = ns + static_cast<uint64_t>((i + 0.5) * 1e9 / rate);
            const double tone[2]{0.1 * std::sin(2 * M_PI * 1100 * (frame + i) / rate),
                                 0.1 * std::sin(2 * M_PI * 2200 * (frame + i) / rate)};
            const auto delay = delayNs + (jump && frame > rate * 3 ? 20000000ULL : 0);
            left[i] = 0.01 * std::sin(2 * M_PI * 337 * (frame + i) / rate) + tone[swap ? 1 : 0] +
                      (marker ? loopz::probe::marker(time - delay, swap ? 1 : 0) : 0);
            right[i] = 0.01 * std::sin(2 * M_PI * 751 * (frame + i) / rate) + tone[swap ? 0 : 1] +
                       (marker ? loopz::probe::marker(time - delay, swap ? 0 : 1) : 0);
        }
        check->append(left, right, 256, ns, frame, true);
        check->consume();
    }
    return check;
}
int main() {
    for (double rate : {44100.0, 48000.0}) {
        auto valid = simulate(rate, false, true, false);
        assert(valid->windows >= 3 && !valid->badWindows && !valid->timestampErrors.load() &&
               !valid->queueOverruns.load());
        assert(valid->minimumDelayMs >= 106 && valid->maximumDelayMs <= 108);
        auto shortDelay = simulate(rate, false, true, false, 4000000);
        assert(shortDelay->windows >= 3 && !shortDelay->badWindows);
        assert(shortDelay->minimumDelayMs >= 3 && shortDelay->maximumDelayMs <= 5);
        assert(simulate(rate, true, true, false)->badWindows > 0);
        assert(simulate(rate, false, false, false)->badWindows > 0);
        auto jump = simulate(rate, false, true, true);
        assert(jump->maximumDelayMs - jump->minimumDelayMs > 10);
        float silence[256]{};
        valid->append(silence, silence, 256, 20000000000ULL, 0, true);
        assert(valid->timestampErrors.load() == 1);
    }
    std::puts("PASS: timestamped marker latency at both rates, channel swap, missing marker, delay "
              "growth and sample discontinuity detection");
}
