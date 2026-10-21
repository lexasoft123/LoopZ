#include <loopz/probe_channels.hpp>
#include <cassert>
#include <cstdio>
#include <vector>
#include <memory>

void sineProfiles() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto check = std::make_unique<loopz::probe::StreamCheck>();
        check->requireMarker = scenario == 1;
        std::array<float, 64> left{}, right{};
        for (uint64_t first = 0; first < 4 * 48000; first += left.size()) {
            for (uint64_t frame = 0; frame < left.size(); ++frame) {
                left[frame] = static_cast<float>(
                    0.1 * std::sin(2 * std::numbers::pi * 1100 * (first + frame) / 48000));
                right[frame] = static_cast<float>(
                    0.1 * std::sin(2 * std::numbers::pi * 2200 * (first + frame) / 48000));
                if (scenario == 2)
                    std::swap(left[frame], right[frame]);
                if (scenario == 3 && first == 96000 && frame >= 10 && frame < 13)
                    left[frame] = 0;
            }
            check->append(left.data(), right.data(), left.size(),
                          10000000000ULL + first * 1000000000ULL / 48000,
                          static_cast<double>(first), true);
            check->consume();
        }
        assert(check->windows == 3 && !check->timestampErrors.load() &&
               !check->queueOverruns.load());
        if (scenario == 0) {
            assert(!check->badWindows && check->minimumAmplitude > 0.099 &&
                   check->maximumCross < 0.001);
            assert(check->maximumZeroRun.load() <= 1);
        } else if (scenario == 1 || scenario == 2) {
            assert(check->badWindows > 0);
        } else {
            assert(check->maximumZeroRun.load() == 3);
        }
    }
}

std::vector<loopz::probe::ChannelPoint> points(uint32_t channels, double gain = 1) {
    std::vector<loopz::probe::ChannelPoint> result(4000);
    for (size_t index = 0; index < result.size(); ++index) {
        auto& point = result[index];
        point.hostNs = 9000125000ULL + index * 1000000;
        for (uint32_t channel = 0; channel < channels; ++channel)
            point.samples[channel] =
                static_cast<float>(gain * loopz::probe::marker(point.hostNs - 4750000, channel));
    }
    return result;
}
void callbackCadence() {
    using loopz::probe::CallbackCadence;
    for (const auto rate : {44100., 48000., 192000.}) {
        CallbackCadence good;
        uint64_t frame = 0;
        for (uint64_t n = 0; n < 1000; ++n) {
            const auto frames = n % 2 ? 32U : 64U;
            const auto host = 9000000000ULL + static_cast<uint64_t>(frame * 1e9L / rate);
            assert(good.observe(host, frame, n + 1, frames, rate, true));
            frame += frames;
        }
        assert(good.callbacks == 1000 && !good.errors);
        assert(good.minimumFrames == 32 && good.maximumFrames == 64);
    }
    for (unsigned fault = 0; fault < 7; ++fault) {
        CallbackCadence check;
        assert(check.observe(9000000000ULL, 0, 1, 48, 48000, true));
        // A one-block hole, duplicate frame or callback, clock jump, invalid
        // timestamp, rate change or overflowing source interval must fail.
        assert(!check.observe(9001000000ULL + (fault == 3 ? 1000000 : 0),
                              fault == 0   ? 96
                              : fault == 1 ? 0
                              : fault == 6 ? UINT64_MAX
                                           : 48,
                              fault == 2 ? 1 : 2, 48, fault == 5 ? 44100 : 48000, fault != 4));
        assert(check.errors == 1);
    }
    CallbackCadence invalid;
    assert(
        !invalid.observe(9000000000ULL, 0, 1, 48, std::numeric_limits<double>::quiet_NaN(), true));
    assert(!invalid.observe(9000000000ULL, 0, 1, 0, 48000, true));
}
int main() {
    callbackCadence();
    sineProfiles();
    for (uint32_t channels : {2u, 8u, 64u}) {
        const auto value = loopz::probe::checkChannels(points(channels), channels);
        assert(value.finite && value.minimumCorrelation > 0.99);
        assert(value.maximumCrossCorrelation < 0.35);
        assert(value.minimumGain > 0.99 && value.maximumGain < 1.01);
        assert(std::abs(value.delayMs - 4.75) <= 0.5);
    }
    auto changed = points(64);
    for (auto& point : changed)
        std::swap(point.samples[62], point.samples[63]);
    assert(loopz::probe::checkChannels(changed, 64).minimumCorrelation < 0.35);
    changed = points(8);
    for (auto& point : changed)
        point.samples[7] += point.samples[6];
    assert(loopz::probe::checkChannels(changed, 8).maximumCrossCorrelation > 0.35);
    const auto quiet = loopz::probe::checkChannels(points(8, 0), 8);
    assert(quiet.maximumMagnitude == 0 && quiet.minimumCorrelation == 0);
    const auto reduced = loopz::probe::checkChannels(points(8, 0.25), 8);
    assert(reduced.minimumCorrelation > 0.99 && reduced.minimumGain > 0.249 &&
           reduced.maximumGain < 0.251);
    changed = points(2);
    changed[2000].samples[1] = std::numeric_limits<float>::quiet_NaN();
    assert(!loopz::probe::checkChannels(changed, 2).finite);
    bool rejected = false;
    try {
        loopz::probe::checkChannels(changed, 65);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    std::puts(
        "Wide marker checks detect crossed/missing channels, leakage, gain and nonfinite data");
}
