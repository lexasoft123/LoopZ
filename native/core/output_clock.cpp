#include <loopz/output_clock.hpp>

#include <algorithm>
#include <cmath>

namespace loopz {
void OutputClock::reset() noexcept {
    *this = {};
}
double OutputClock::observe(uint64_t frame, uint64_t hostNs, uint32_t frames, double nominalRate,
                            bool discontinuity) noexcept {
    if (!frames || frame > UINT64_MAX - frames || !hostNs || !std::isfinite(nominalRate) ||
        nominalRate < 8000 || nominalRate > 192000) {
        reset();
        return 0;
    }
    if (previousNs_) {
        const auto expectedNs = static_cast<long double>(previousFrames_) * 1e9L / nominalRate;
        const auto elapsedNs = hostNs >= previousNs_
                                   ? static_cast<long double>(hostNs - previousNs_)
                                   : -static_cast<long double>(previousNs_ - hostNs);
        discontinuity =
            discontinuity || nominalRate != nominalRate_ ||
            frame != previousFrame_ + previousFrames_ || hostNs <= previousNs_ ||
            std::abs(elapsedNs - expectedNs) > std::max(2000000.0L, expectedNs * 0.005L + 1000);
    }
    if (discontinuity)
        reset();
    if (!originNs_) {
        originNs_ = recentNs_ = hostNs;
        originFrame_ = recentFrame_ = frame;
    }
    constexpr uint64_t windowNs = 25000000;
    if (hostNs - recentNs_ >= windowNs) {
        if (recentNs_ != originNs_) {
            originNs_ = recentNs_;
            originFrame_ = recentFrame_;
        }
        recentNs_ = hostNs;
        recentFrame_ = frame;
    }
    double rate = nominalRate;
    if (hostNs - originNs_ >= windowNs) {
        rate = static_cast<double>(static_cast<long double>(frame - originFrame_) * 1e9L /
                                   (hostNs - originNs_));
        if (rate < nominalRate * 0.995 || rate > nominalRate * 1.005) {
            // A clock step is not an extreme sample-rate conversion. Start
            // a fresh estimate without carrying a prior stream's rate.
            originNs_ = recentNs_ = hostNs;
            originFrame_ = recentFrame_ = frame;
            rate = nominalRate;
        }
    }
    previousNs_ = hostNs;
    previousFrame_ = frame;
    previousFrames_ = frames;
    nominalRate_ = nominalRate;
    return rate;
}
} // namespace loopz
