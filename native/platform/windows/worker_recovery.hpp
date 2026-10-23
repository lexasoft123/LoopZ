#pragma once
#include <cstdint>
#include <limits>

namespace loopz::windows {
// Control-thread only. Try a newly observed fault immediately; keep persistent
// faults from becoming a busy reconnect loop. A healthy observation re-arms it.
class WorkerRecovery final {
  public:
    bool attempt(uint64_t nowNs, bool healthy) noexcept {
        if (healthy) {
            waiting_ = false;
            return false;
        }
        if (waiting_ && nowNs < nextAttemptNs_)
            return false;
        waiting_ = true;
        constexpr uint64_t backoff = 3000000000;
        constexpr auto limit = std::numeric_limits<uint64_t>::max();
        nextAttemptNs_ = nowNs > limit - backoff ? limit : nowNs + backoff;
        return true;
    }

  private:
    bool waiting_{};
    uint64_t nextAttemptNs_{};
};
} // namespace loopz::windows
