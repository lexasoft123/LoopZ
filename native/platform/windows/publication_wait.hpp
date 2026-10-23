#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace loopz::windows {
// The paired policy is a transport-v3 float publication contract. Legacy
// transports and sources without publication history retain buffered delivery.
// A selected paired connection must not silently lose its queue-budget proof.
struct PublicationFlowPolicy {
    bool paired{};
    constexpr unsigned version() const noexcept {
        return paired ? 1U : 0U;
    }
    constexpr bool acceptsQueueStatus(bool available) const noexcept {
        return !paired || available;
    }
};
inline constexpr PublicationFlowPolicy publicationFlowPolicy(unsigned transportVersion,
                                                             unsigned sampleBytes,
                                                             bool publishedHistory,
                                                             bool smallBatches) noexcept {
    return {transportVersion == 3 && sampleBytes == 4 && publishedHistory && !smallBatches};
}
// Private COM worker policy, not an audio callback. queryStarted precedes the
// driver's queue snapshot so property-call time conservatively spends budget.
// A late chunk may still wait while the recording FIFO covers rendering and
// delivery. Never exceed the existing worker lateness limit or spend that reserve.
inline constexpr uint64_t privateWorkerLatenessLimitNs = 30000000;
inline uint64_t publicationWaitDeadline(uint64_t queryStarted, uint64_t due, uint32_t queuedFrames,
                                        uint32_t rate, uint64_t renderReserveNs) noexcept {
    constexpr auto limitNs = privateWorkerLatenessLimitNs;
    if (!rate || rate > 192000 || queryStarted < due ||
        due > std::numeric_limits<uint64_t>::max() - limitNs || queryStarted >= due + limitNs)
        return queryStarted;
    const auto queuedNs = uint64_t(queuedFrames) * 1000000000 / rate;
    if (queuedNs <= renderReserveNs)
        return queryStarted;
    return queryStarted + std::min(queuedNs - renderReserveNs, due + limitNs - queryStarted);
}
} // namespace loopz::windows
