#pragma once

#include <cstdint>
#include <memory>

namespace loopz::macos {
struct RecoveryEventBackend;
enum class RecoveryEvent : uint32_t {
    None = 0,
    Wake = 1,
    AudioInventory = 2,
    Applications = 4,
    AudioServiceReset = 8,
};
constexpr bool includes(RecoveryEvent events, RecoveryEvent wanted) noexcept {
    return (static_cast<uint32_t>(events) & static_cast<uint32_t>(wanted)) != 0;
}
struct RecoveryEventStatus {
    uint32_t audioListeners{0};
    int32_t registrationError{0};
};
// Control owner only. Native callbacks retain a small independent mailbox;
// they never touch EngineRuntime, acquire devices or rebuild a graph.
class RecoveryEvents {
  public:
    enum class Scope { WorkspaceOnly, WorkspaceAndAudio };
    explicit RecoveryEvents(Scope scope = Scope::WorkspaceAndAudio);
    RecoveryEvents(Scope scope, const RecoveryEventBackend& backend);
    ~RecoveryEvents();
    RecoveryEvents(const RecoveryEvents&) = delete;
    RecoveryEvents& operator=(const RecoveryEvents&) = delete;
    // Inventory bursts coalesce to at most four deliveries per second. Wake
    // and audio-service resets bypass that delay. Failed listener installation
    // retries; ordinary runtime polling remains the correctness fallback.
    RecoveryEvent poll(uint64_t nowMilliseconds);
    RecoveryEventStatus status() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz::macos
