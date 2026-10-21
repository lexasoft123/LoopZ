#include <loopz/callback_timing.hpp>

#include <atomic>
#include <cassert>
#include <latch>
#include <thread>

namespace {
using loopz::CallbackTimingObservation;
void independentSizesAndBounds() {
    CallbackTimingObservation observation;
    observation.observe(0, 64, 128);
    assert(observation.snapshot() == 0);
    observation.reset(1);
    observation.observe(observation.snapshot(), 64, 128);
    auto value = observation.decode(observation.snapshot());
    assert(value.session == 1 && value.maximumWriteFrames == 64 &&
           value.maximumNominalFrames == 128);
    observation.observe(observation.snapshot(), 256, 32);
    value = observation.decode(observation.snapshot());
    assert(value.maximumWriteFrames == 256 && value.maximumNominalFrames == 128);
    const auto before = observation.snapshot();
    observation.observe(before, 0, 128);
    observation.observe(before, UINT16_MAX + 1U, 128);
    observation.observe(before, 64, UINT16_MAX + 1U);
    assert(observation.snapshot() == before);
    observation.reset(UINT32_MAX);
    observation.observe(observation.snapshot(), UINT16_MAX, UINT16_MAX);
    value = observation.decode(observation.snapshot());
    assert(value.session == UINT32_MAX && value.maximumWriteFrames == UINT16_MAX &&
           value.maximumNominalFrames == UINT16_MAX);
}
void staleCallbackCannotCrossSessionReset() {
    CallbackTimingObservation observation;
    observation.reset(1);
    std::latch entered{1}, release{1};
    std::jthread callback([&] {
        const auto entry = observation.snapshot();
        entered.count_down();
        release.wait();
        observation.observe(entry, 4096, 8192);
    });
    entered.wait();
    observation.reset(2);
    release.count_down();
    callback.join();
    auto value = observation.decode(observation.snapshot());
    assert(value.session == 2 && value.maximumWriteFrames == 0 && value.maximumNominalFrames == 0);
    observation.observe(observation.snapshot(), 32, 0);
    value = observation.decode(observation.snapshot());
    assert(value.session == 2 && value.maximumWriteFrames == 32 && value.maximumNominalFrames == 0);
}
void concurrentSnapshotsKeepThePairCoherent() {
    CallbackTimingObservation observation;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> reads{0};
    std::latch started{1};
    std::jthread reader([&] {
        started.count_down();
        while (!stop.load()) {
            const auto value = observation.decode(observation.snapshot());
            if (value.maximumWriteFrames) {
                const auto expected = value.session % 1024 + 1;
                assert(value.maximumWriteFrames == expected &&
                       value.maximumNominalFrames == 2 * expected);
                reads.fetch_add(1);
            } else
                assert(value.maximumNominalFrames == 0);
        }
    });
    started.wait();
    for (uint32_t session = 1; session <= 20000; ++session) {
        observation.reset(session);
        const auto count = session % 1024 + 1;
        observation.observe(observation.snapshot(), count, 2 * count);
        if (session == 1)
            while (!reads.load())
                std::this_thread::yield();
    }
    stop.store(true);
    reader.join();
    assert(reads.load());
}
} // namespace
int main() {
    independentSizesAndBounds();
    staleCallbackCannotCrossSessionReset();
    concurrentSnapshotsKeepThePairCoherent();
}
