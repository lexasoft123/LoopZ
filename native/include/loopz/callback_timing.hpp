#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace loopz {
struct CallbackTimingValues {
    uint32_t session, maximumWriteFrames, maximumNominalFrames;
};

// One serial callback writer; control assigns a distinct nonzero session on
// reset. Both maxima share one word, preventing partial or mixed-session reads.
// This is diagnostic metadata, never a buffer-size or timing-policy decision.
class CallbackTimingObservation {
  public:
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static constexpr uint32_t maximumFrames = UINT16_MAX;

    static constexpr CallbackTimingValues decode(uint64_t word) noexcept {
        return {static_cast<uint32_t>(word >> 32), static_cast<uint32_t>((word >> 16) & 0xffff),
                static_cast<uint32_t>(word & 0xffff)};
    }
    uint64_t snapshot() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }
    void reset(uint32_t session) noexcept {
        value_.store(uint64_t(session) << 32, std::memory_order_relaxed);
    }
    void observe(uint64_t atEntry, uint32_t actual, uint32_t nominal) noexcept {
        const auto before = decode(atEntry);
        if (!before.session || !actual || actual > maximumFrames || nominal > maximumFrames)
            return;
        const auto next = (atEntry & 0xffffffff00000000ULL) |
                          (uint64_t(std::max(actual, before.maximumWriteFrames)) << 16) |
                          std::max(nominal, before.maximumNominalFrames);
        // A reset makes this callback stale. Never spin or carry its old
        // session's observations into a newly started stream.
        if (next != atEntry)
            value_.compare_exchange_strong(atEntry, next, std::memory_order_relaxed);
    }

  private:
    std::atomic<uint64_t> value_{0};
};
} // namespace loopz
