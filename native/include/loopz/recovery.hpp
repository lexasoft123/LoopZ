#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace loopz {

// Control-domain desired connections. A missing endpoint never removes a node.
// Dependencies are resource prerequisites, not optional audio source inputs.
struct RecoveryNode {
    std::string id;
    std::vector<std::string> dependencies;
};
struct RecoveryAttempt {
    std::string id;
    uint64_t token;
};

// Single control-thread owner; monotonic time in milliseconds, no OS APIs.
// Backends release lost resources before calling attempts(), in the returned
// reverse dependency order. Audio callbacks only see separately published state.
class GraphRecovery {
  public:
    // Replacing intent invalidates every outstanding token. The backend must
    // release its old resources first; no platform objects are owned here.
    void setDesired(std::vector<RecoveryNode> nodes, uint64_t now);
    std::vector<RecoveryAttempt> attempts(uint64_t now);
    bool complete(const RecoveryAttempt& attempt, bool success, uint64_t now);
    std::vector<std::string> lost(const std::string& id, uint64_t now);
    // Wake/inventory events accelerate waiting retries without duplicating
    // attempts already in flight or disturbing healthy resources.
    void retryNow(uint64_t now) noexcept;
    // Whole-owner transitions after its backend resources have been released.
    // Retain desired nodes and token uniqueness, with no allocation. Restart
    // clears failure escalation; disconnected preserves the retry backoff.
    void restart(uint64_t now) noexcept;
    void disconnected(uint64_t now) noexcept;
    bool ready(const std::string& id) const;
    bool restored() const noexcept;
    size_t desiredCount() const noexcept;

  private:
    enum class Phase { Missing, Connecting, Ready };
    struct Entry {
        RecoveryNode node;
        Phase phase{Phase::Missing};
        uint64_t token{0}, due{0}, deadline{0}, readySince{0};
        uint32_t failures{0};
    };
    std::vector<Entry> entries_;
    uint64_t nextToken_{0};
    Entry& find(const std::string& id);
    const Entry& find(const std::string& id) const;
    static void failed(Entry& entry, uint64_t now) noexcept;
};

} // namespace loopz
