#include <loopz/probe_recovery.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
using namespace loopz::probe;
struct Feed {
    RecoveryCapture capture;
    uint32_t channels;
    uint64_t frame{};
    std::array<std::array<float, 64>, 64> samples{};
    std::array<const float*, 64> planes{};
    explicit Feed(uint32_t count) : capture(count, 48000), channels(count) {
        for (uint32_t channel = 0; channel < channels; ++channel)
            planes[channel] = samples[channel].data();
    }
    void block(bool silence = false, int badChannel = -1, bool invalidTime = false,
               bool nonfinite = false) {
        const auto host = 9000000000ULL + frame * 1000000000ULL / 48000;
        for (uint32_t channel = 0; channel < channels; ++channel)
            for (uint32_t offset = 0; offset < 64; ++offset) {
                const auto time = host + static_cast<uint64_t>((offset + 0.5) * 1e9 / 48000);
                samples[channel][offset] = silence ? 0 : marker(time - 4750000, channel);
            }
        if (badChannel >= 0)
            samples[badChannel][17] = nonfinite ? std::numeric_limits<float>::quiet_NaN() : 0;
        capture.append(planes.data(), 64, host, !invalidTime);
        frame += 64;
    }
    void interval(uint32_t blocks, bool silence = false) {
        for (uint32_t block = 0; block < blocks; ++block)
            this->block(silence);
    }
};

void fullRecovery(uint32_t channels) {
    Feed feed(channels);
    feed.interval(750, true); // Warmup cannot contaminate the baseline.
    feed.capture.advance(RecoveryPhase::Baseline);
    feed.interval(1500);
    feed.capture.advance(RecoveryPhase::Disconnected);
    feed.interval(75, true);
    feed.interval(75);
    assert(feed.capture.observed() == RecoveryPhase::Disconnected);
    assert(recoveredSignal(feed.capture.segment(RecoveryPhase::Baseline), channels));
    feed.capture.advance(RecoveryPhase::Recovered);
    feed.interval(2250);
    assert(recoveredSignal(feed.capture.segment(RecoveryPhase::Recovered), channels));
    const auto& outage = feed.capture.segment(RecoveryPhase::Disconnected);
    assert(outage.zeroSamples == 75 * 64 * channels && outage.longestZeroRun == 75 * 64);
    assert(!outage.timestampErrors && !outage.nonfinite && !outage.overflows);
}

void failuresCannotHideInAverages() {
    for (unsigned failure = 0; failure < 4; ++failure) {
        Feed feed(64);
        feed.capture.advance(RecoveryPhase::Baseline);
        feed.interval(750);
        feed.capture.advance(RecoveryPhase::Disconnected);
        feed.interval(75, true);
        feed.capture.advance(RecoveryPhase::Recovered);
        feed.interval(750);
        if (failure == 0)
            feed.block(false, 63); // One missing sample on the last channel.
        else if (failure == 1)
            feed.block(false, 62, false, true);
        else if (failure == 2)
            feed.block(false, -1, true);
        else {
            feed.frame -= 64; // A repeated timestamp after the outage is invalid.
            feed.block();
        }
        feed.interval(750);
        assert(!recoveredSignal(feed.capture.segment(RecoveryPhase::Recovered), 64));
    }
    Feed missing(2);
    missing.capture.advance(RecoveryPhase::Baseline);
    missing.interval(374);
    missing.capture.advance(RecoveryPhase::Disconnected);
    missing.block(true);
    assert(!recoveredSignal(missing.capture.segment(RecoveryPhase::Baseline), 2));
    bool rejected = false;
    try {
        missing.capture.advance(RecoveryPhase::Baseline);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void publication() {
    Feed feed(8);
    std::jthread writer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            feed.block();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    feed.capture.advance(RecoveryPhase::Baseline);
    while (feed.capture.observed() != RecoveryPhase::Baseline)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    feed.capture.advance(RecoveryPhase::Disconnected);
    while (feed.capture.observed() != RecoveryPhase::Disconnected)
        std::this_thread::yield();
    const auto& immutable = feed.capture.segment(RecoveryPhase::Baseline);
    const auto count = immutable.count;
    assert(count > 0);
    const auto last = immutable.points[count - 1].hostNs;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(immutable.count == count && immutable.points[count - 1].hostNs == last);
    writer.request_stop();
    writer.join();
}

void boundedStorage() {
    Feed feed(2);
    feed.capture.advance(RecoveryPhase::Baseline);
    feed.interval(7500);
    const auto& segment = feed.capture.segment(RecoveryPhase::Baseline);
    assert(segment.count == segment.points.size() && segment.overflows > 0);
    assert(!recoveredSignal(segment, 2));
    for (uint32_t channels : {0U, 65U}) {
        bool rejected = false;
        try {
            RecoveryCapture invalid(channels, 48000);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }
    feed.capture.advance(RecoveryPhase::Disconnected);
    feed.capture.advance(RecoveryPhase::Recovered);
    bool rejected = false;
    try {
        feed.capture.advance(static_cast<RecoveryPhase>(4));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void signalReadinessRequiresEveryChannelAndContinuousTime() {
    Feed feed(64);
    feed.capture.advance(RecoveryPhase::Baseline);
    feed.interval(750);
    assert(feed.capture.continuousSignalFrames() == 48000);
    feed.capture.advance(RecoveryPhase::Disconnected);
    feed.block();
    assert(feed.capture.continuousSignalFrames() == 64);
    feed.interval(749);
    assert(feed.capture.continuousSignalFrames() == 48000);
    feed.block(false, 63);
    assert(feed.capture.continuousSignalFrames() == 0);
    feed.interval(749);
    assert(feed.capture.continuousSignalFrames() < 48000);
    feed.block();
    assert(feed.capture.continuousSignalFrames() == 48000);
    feed.block(false, 62, false, true);
    assert(feed.capture.continuousSignalFrames() == 0);
    feed.interval(750);
    feed.block(false, -1, true);
    assert(feed.capture.continuousSignalFrames() == 0);
    feed.interval(750);
    feed.frame += 64;
    feed.block();
    assert(feed.capture.continuousSignalFrames() == 0);
    feed.interval(750);
    feed.block(true);
    assert(feed.capture.continuousSignalFrames() == 0);
}
} // namespace

int main() {
    for (uint32_t channels : {2U, 8U, 64U})
        fullRecovery(channels);
    failuresCannotHideInAverages();
    publication();
    boundedStorage();
    signalReadinessRequiresEveryChannelAndContinuousTime();
    std::puts("Continuous-client recovery segments preserve faults, strict healthy intervals and "
              "callback handoff");
}
