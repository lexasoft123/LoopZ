#include <loopz/render_schedule.hpp>
#include <loopz/timeline.hpp>

#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace loopz;
void cadence(uint32_t rate, uint32_t quantum, uint32_t lead) {
    RenderSchedule schedule({rate, quantum, lead, quantum * 16});
    uint64_t now = 100000000000ULL, expected = 0, blocks = 0;
    for (unsigned step = 0; step < 10000; ++step) {
        auto result = schedule.step(now);
        assert(result.valid && !result.reanchored && !result.skippedFrames && !result.lateFrames);
        if (result.blocks) {
            assert(result.blocks == 1);
            if (blocks)
                assert(result.firstFrame == expected);
            expected = result.firstFrame + quantum;
            blocks += result.blocks;
        }
        assert(result.nextWakeNs > now);
        // Early/inexact waits must not duplicate or render the next block.
        auto early = schedule.step(result.nextWakeNs - 1);
        assert(early.valid && !early.blocks && early.nextWakeNs == result.nextWakeNs);
        now = result.nextWakeNs;
    }
    assert(blocks == 10000);
}
void boundedCatchupAndWakeRecovery() {
    RenderSchedule schedule({48000, 64, 32, 1024});
    auto first = schedule.step(100000000000ULL);
    const auto expected = first.firstFrame + 64;
    auto burst = schedule.step(first.nextWakeNs + frameNs(640, 48000));
    assert(burst.valid && !burst.reanchored && burst.blocks == 4 && burst.firstFrame == expected);
    assert(burst.lateFrames == 256 && burst.nextWakeNs < first.nextWakeNs + frameNs(640, 48000));
    auto second = schedule.step(first.nextWakeNs + frameNs(640, 48000));
    assert(second.blocks == 4 && second.firstFrame == expected + 256 && !second.reanchored);
    auto third = schedule.step(first.nextWakeNs + frameNs(640, 48000));
    assert(third.blocks && third.blocks <= 4 && third.firstFrame == expected + 512);
    const auto resumedAt = first.nextWakeNs + 60000000000ULL;
    auto resumed = schedule.step(resumedAt);
    assert(resumed.valid && resumed.reanchored && resumed.skippedFrames > 48000 * 59 &&
           resumed.blocks == 1);
    assert(resumed.firstFrame % 64 == 0 && resumed.nextWakeNs > resumedAt);
    auto following = schedule.step(resumed.nextWakeNs);
    assert(following.blocks == 1 && !following.reanchored &&
           following.firstFrame == resumed.firstFrame + 64);
    // A regressed/invalid clock never rewrites already-published history.
    assert(!schedule.step(0).valid && !schedule.step(1).valid && !schedule.step(UINT64_MAX).valid);
    auto next = schedule.step(following.nextWakeNs);
    assert(next.valid && next.firstFrame == following.firstFrame + 64);
    schedule.reset();
    assert(schedule.step(1000000000).valid);
}
} // namespace
int main() {
    for (auto rate : {8000U, 44100U, 48000U, 192000U})
        for (auto quantum : {32U, 64U, 128U, 256U})
            for (auto lead : {0U, quantum / 2})
                cadence(rate, quantum, lead);
    // An 8 ms preparation lead is not necessarily block-aligned or even an
    // integral number of milliseconds in sample frames (e.g. 1411 at 176.4k).
    // Verify exact wake rounding and no duplicate blocks at all device rates.
    for (auto rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U})
        cadence(rate, 256, rate * 8 / 1000);
    boundedCatchupAndWakeRecovery();
    for (auto timing : {RenderScheduleTiming{0, 64, 32, 1024},
                        {48000, 16, 32, 1024},
                        {48000, 64, 9000, 1024},
                        {48000, 64, 32, 32}}) {
        bool rejected = false;
        try {
            RenderSchedule invalid(timing);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }
    std::puts(
        "Render schedule cadence, bounded catchup, long-stall recovery and clock rejection passed");
}
