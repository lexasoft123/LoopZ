#include <loopz/audio_delivery_timing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace loopz {
namespace {
bool age(uint64_t now, uint64_t timestamp, int64_t& result) noexcept {
    const auto distance = now >= timestamp ? now - timestamp : timestamp - now;
    if (distance > static_cast<uint64_t>(INT64_MAX))
        return false;
    result = now >= timestamp ? static_cast<int64_t>(distance) : -static_cast<int64_t>(distance);
    return true;
}
} // namespace
void AudioDeliveryTiming::observe(uint64_t firstHostNs, uint32_t frames, double rate,
                                  uint64_t enteredNs, uint64_t readyNs, double sourceFrame,
                                  bool sourceFrameValid) noexcept {
    if (!firstHostNs || !enteredNs || readyNs < enteredNs || !frames || frames > 4096 ||
        !std::isfinite(rate) || rate < 8000 || rate > 192000 ||
        (previousEntryNs_ && enteredNs <= previousEntryNs_)) {
        ++value_.rejected;
        return;
    }
    const auto duration = static_cast<uint64_t>(static_cast<long double>(frames) * 1e9L / rate);
    int64_t entryAge{}, readyAge{};
    if (firstHostNs > UINT64_MAX - duration || !age(enteredNs, firstHostNs, entryAge) ||
        !age(readyNs, firstHostNs + duration, readyAge)) {
        ++value_.rejected;
        return;
    }
    if (!firstEntryNs_)
        firstEntryNs_ = enteredNs;
    const auto gap = previousEntryNs_ ? enteredNs - previousEntryNs_ : 0;
    previousEntryNs_ = enteredNs;
    uint64_t anchorError = 0;
    bool unsupported = false;
    if (previousSampleNs_ && firstHostNs > previousSampleNs_) {
        const auto elapsed = firstHostNs - previousSampleNs_;
        const auto expected =
            static_cast<uint64_t>(static_cast<long double>(previousFrames_) * 1e9L / previousRate_);
        anchorError = elapsed > expected ? elapsed - expected : expected - elapsed;
        const auto intervalRate = static_cast<double>(previousFrames_) * 1e9 / elapsed;
        unsupported = intervalRate < previousRate_ * 0.995 || intervalRate > previousRate_ * 1.005;
    }
    const bool sourceStep = sourceFrameValid && previousSourceValid_ &&
                            std::fabs(sourceFrame - previousSourceFrame_ - previousFrames_) > 0.5;
    // Bounded callback-owned evidence. Never allocate or write a log on the
    // audio thread. Preserve signed frame deltas to distinguish lost cycles
    // from repeated/backward timestamps rather than calling both a dropout.
    if (enteredNs - firstEntryNs_ >= warmupNs_ && (sourceStep || unsupported) &&
        value_.eventCount < value_.events.size()) {
        value_.events[value_.eventCount++] = {
            enteredNs - firstEntryNs_,
            firstHostNs > previousSampleNs_ ? firstHostNs - previousSampleNs_ : 0,
            gap,
            readyNs - enteredNs,
            previousFrames_,
            frames,
            sourceFrame - previousSourceFrame_,
            static_cast<double>(previousFrames_),
            sourceFrameValid && previousSourceValid_};
    }
    previousSampleNs_ = firstHostNs;
    previousSourceFrame_ = sourceFrame;
    previousSourceValid_ = sourceFrameValid;
    previousFrames_ = frames;
    previousRate_ = rate;
    if (enteredNs - firstEntryNs_ < warmupNs_) {
        ++value_.warmupCallbacks;
        return;
    }
    if (!value_.callbacks) {
        value_.minimumFrames = value_.maximumFrames = frames;
        value_.minimumEntryAgeNs = value_.maximumEntryAgeNs = entryAge;
        value_.minimumReadyEndAgeNs = value_.maximumReadyEndAgeNs = readyAge;
        value_.minimumGapNs = gap;
    }
    ++value_.callbacks;
    value_.maximumAnchorErrorNs = std::max(value_.maximumAnchorErrorNs, anchorError);
    value_.unsupportedAnchorIntervals += unsupported;
    value_.sourceFrameDiscontinuities += sourceStep;
    value_.frames += frames;
    value_.minimumFrames = std::min(value_.minimumFrames, frames);
    value_.maximumFrames = std::max(value_.maximumFrames, frames);
    value_.minimumEntryAgeNs = std::min(value_.minimumEntryAgeNs, entryAge);
    value_.maximumEntryAgeNs = std::max(value_.maximumEntryAgeNs, entryAge);
    value_.minimumReadyEndAgeNs = std::min(value_.minimumReadyEndAgeNs, readyAge);
    value_.maximumReadyEndAgeNs = std::max(value_.maximumReadyEndAgeNs, readyAge);
    if (gap) {
        if (!value_.minimumGapNs)
            value_.minimumGapNs = gap;
        value_.minimumGapNs = std::min(value_.minimumGapNs, gap);
        value_.maximumGapNs = std::max(value_.maximumGapNs, gap);
    }
    value_.maximumProcessingNs = std::max(value_.maximumProcessingNs, readyNs - enteredNs);
}
AudioDeliverySnapshot AudioDeliveryTiming::snapshot() const noexcept {
    return value_;
}
void AudioDeliveryTiming::reset() noexcept {
    firstEntryNs_ = previousEntryNs_ = 0;
    previousSampleNs_ = 0;
    previousFrames_ = 0;
    previousRate_ = 0;
    previousSourceFrame_ = 0;
    previousSourceValid_ = false;
    value_ = {};
}
} // namespace loopz
