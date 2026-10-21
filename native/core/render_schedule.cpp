#include <loopz/render_schedule.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <stdexcept>

namespace loopz {
RenderSchedule::RenderSchedule(RenderScheduleTiming timing) : timing_(timing) {
    if (timing.rate < 8000 || timing.rate > 192000 ||
        (timing.blockFrames != 32 && timing.blockFrames != 64 && timing.blockFrames != 128 &&
         timing.blockFrames != kBlock) ||
        timing.leadFrames > kCapacity / 4 || timing.maximumCatchupFrames < timing.blockFrames ||
        timing.maximumCatchupFrames > kCapacity / 2)
        throw std::invalid_argument("Invalid render schedule timing");
}
void RenderSchedule::reset() noexcept {
    next_ = previousNow_ = 0;
    initialized_ = false;
}
RenderScheduleTiming RenderSchedule::timing() const noexcept {
    return timing_;
}
ScheduledRender RenderSchedule::step(uint64_t nowNs) noexcept {
    if (!nowNs || nowNs > UINT64_MAX - 10000000000ULL || (initialized_ && nowNs < previousNow_))
        return {};
    previousNow_ = nowNs;
    const auto target = hostFrame(nowNs, timing_.rate) + timing_.leadFrames;
    ScheduledRender result;
    result.valid = true;
    if (!initialized_) {
        next_ = target / timing_.blockFrames * timing_.blockFrames;
        initialized_ = true;
    } else if (next_ + timing_.maximumCatchupFrames < target) {
        const auto anchor = target / timing_.blockFrames * timing_.blockFrames;
        result.skippedFrames = anchor - next_;
        next_ = anchor;
        result.reanchored = true;
    }
    result.firstFrame = next_;
    if (next_ <= target) {
        result.blocks = static_cast<uint32_t>(
            std::min<uint64_t>(4, (target - next_) / timing_.blockFrames + 1));
        const auto late = (target - next_) / timing_.blockFrames;
        result.lateFrames = std::min<uint64_t>(result.blocks, late) * timing_.blockFrames;
        next_ += static_cast<uint64_t>(result.blocks) * timing_.blockFrames;
    }
    // Ceiling the boundary prevents a rounded-down host time from consuming a
    // future block early. Its integer frame is exact at the configured rate.
    const auto wakeFrame = next_ > timing_.leadFrames ? next_ - timing_.leadFrames : 0;
    result.nextWakeNs = frameNs(wakeFrame, timing_.rate);
    if (hostFrame(result.nextWakeNs, timing_.rate) < wakeFrame)
        ++result.nextWakeNs;
    return result;
}
} // namespace loopz
