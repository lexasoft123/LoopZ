#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace loopz {
struct HistoryReadSnapshot {
    bool valid{false}, outsideBand{false};
    uint64_t observations{0}, bandExits{0}, reanchors{0}, generationChanges{0};
    uint64_t windowObservations{0};
    uint32_t appendFrames{0};
    double headroomFrames{0}, minimumFrames{0}, maximumFrames{0}, meanFrames{0};
    double acquisitionFrames{0}, lowerBoundFrames{0}, upperBoundFrames{0}, ratio{1};
};

// One serial reader writes; control reads a bounded atomic snapshot. This is
// observation ONLY: it never changes a timestamp, phase, ratio or latency.
// Windows contain about one output second. Band exits count in->out edges;
// acquisitions include the first valid read, generation and request boundaries.
class HistoryReadObservation {
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

  public:
    void bounds(double minimum, double maximum, uint32_t includedAppendFrames = 0,
                double referenceRate = 0) noexcept {
        if (std::isfinite(minimum) && std::isfinite(maximum) && minimum >= 0 &&
            maximum >= minimum && std::isfinite(referenceRate) && referenceRate >= 0) {
            boundsChanged_ |= !configured_ || lower_ != minimum || upper_ != maximum ||
                              coveredAppend_ != includedAppendFrames ||
                              referenceRate_ != referenceRate;
            coveredAppend_ = includedAppendFrames;
            referenceRate_ = referenceRate;
            configured_ = true;
            lower_ = minimum;
            upper_ = maximum;
        }
    }
    void unavailable() noexcept {
        needAcquire_ = true;
        begin();
        valid_.store(false);
        end();
    }
    void observe(uint64_t generation, uint64_t hostNs, uint64_t publishedEnd, uint64_t readFrame,
                 long double fraction, uint32_t frames, double sourceRate, double outputRate,
                 double capacityFrames, uint32_t appendFrames = 0) noexcept {
        if (!generation || !hostNs || !frames || !std::isfinite(fraction) ||
            !std::isfinite(sourceRate) || !std::isfinite(outputRate) || sourceRate <= 0 ||
            outputRate <= 0) {
            unavailable();
            return;
        }
        const double headroom = static_cast<double>(static_cast<long double>(publishedEnd) -
                                                    static_cast<long double>(readFrame) - fraction);
        // Scene/monitor budgets are in their render clock. Convert once to
        // the SOURCE-frame units used by published-end and append sizes.
        const auto scale = referenceRate_ > 0 ? sourceRate / referenceRate_ : 1;
        const auto lower = lower_ * scale;
        const auto coveredAppend = coveredAppend_ * scale;
        // One producer append changes published-end in a single step. Include
        // its observed size beyond the scheduling allowance already in the band.
        const auto extra = appendFrames > coveredAppend ? appendFrames - coveredAppend : 0;
        const auto upper =
            configured_ ? std::min(upper_ * scale + extra, capacityFrames) : capacityFrames;
        const bool generationChanged = generation_ && generation_ != generation;
        const bool boundary =
            !generation_ || generationChanged || boundsChanged_ || needAcquire_ ||
            (previousEndNs_ && (hostNs <= previousNs_ || std::abs(static_cast<long double>(hostNs) -
                                                                  previousEndNs_) > 2000000));
        if (boundary) {
            acquisition_ = headroom;
            ++reanchors_;
            generationChanges_ += generationChanged;
            // A new epoch starts a new observational window, not a new read phase.
            windowFrames_ = windowCount_ = 0;
            wasOutside_ = false;
        }
        boundsChanged_ = needAcquire_ = false;
        generation_ = generation;
        previousNs_ = hostNs;
        previousEndNs_ = static_cast<long double>(hostNs) + frames * 1e9L / outputRate;
        if (!windowCount_ || windowFrames_ >= outputRate) {
            windowFrames_ = windowCount_ = 0;
            minimum_ = maximum_ = headroom;
            sum_ = 0;
        }
        minimum_ = std::min(minimum_, headroom);
        maximum_ = std::max(maximum_, headroom);
        sum_ += headroom;
        ++windowCount_;
        windowFrames_ += frames;
        const bool outside = headroom < lower || headroom > upper;
        bandExits_ += outside && !wasOutside_;
        wasOutside_ = outside;
        ++observations_;
        begin();
        valid_.store(true);
        outside_.store(outside);
        observationsAtomic_.store(observations_);
        exitsAtomic_.store(bandExits_);
        reanchorsAtomic_.store(reanchors_);
        generationsAtomic_.store(generationChanges_);
        windowAtomic_.store(windowCount_);
        appendAtomic_.store(appendFrames);
        const double values[]{headroom,     minimum_, maximum_, sum_ / windowCount_,
                              acquisition_, lower,    upper,    sourceRate / outputRate};
        for (std::size_t i = 0; i < 8; ++i)
            values_[i].store(std::bit_cast<uint64_t>(values[i]));
        end();
    }
    HistoryReadSnapshot snapshot() const noexcept {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto version = version_.load();
            if (version & 1)
                continue;
            HistoryReadSnapshot result;
            result.valid = valid_.load();
            result.outsideBand = outside_.load();
            result.observations = observationsAtomic_.load();
            result.bandExits = exitsAtomic_.load();
            result.reanchors = reanchorsAtomic_.load();
            result.generationChanges = generationsAtomic_.load();
            result.windowObservations = windowAtomic_.load();
            result.appendFrames = appendAtomic_.load();
            double* values[]{&result.headroomFrames,    &result.minimumFrames,
                             &result.maximumFrames,     &result.meanFrames,
                             &result.acquisitionFrames, &result.lowerBoundFrames,
                             &result.upperBoundFrames,  &result.ratio};
            for (std::size_t i = 0; i < 8; ++i)
                *values[i] = std::bit_cast<double>(values_[i].load());
            if (version_.load() == version)
                return result;
        }
        return {};
    }

  private:
    void begin() noexcept {
        version_.fetch_add(1);
    }
    void end() noexcept {
        version_.fetch_add(1);
    }
    uint64_t generation_{0}, previousNs_{0}, windowFrames_{0}, windowCount_{0};
    uint64_t observations_{0}, reanchors_{0}, generationChanges_{0}, bandExits_{0};
    long double previousEndNs_{0};
    double referenceRate_{0}, lower_{0}, upper_{0}, minimum_{0}, maximum_{0}, sum_{0},
        acquisition_{0};
    uint32_t coveredAppend_{0};
    std::atomic<uint32_t> appendAtomic_{0};
    bool wasOutside_{false}, boundsChanged_{false}, needAcquire_{false}, configured_{false};
    std::atomic<uint64_t> version_{0}, observationsAtomic_{0}, exitsAtomic_{0}, reanchorsAtomic_{0},
        generationsAtomic_{0}, windowAtomic_{0}, values_[8]{};
    std::atomic<bool> valid_{false}, outside_{false};
};
} // namespace loopz
