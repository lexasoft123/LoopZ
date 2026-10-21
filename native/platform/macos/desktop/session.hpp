#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace loopz::macos {
enum class SessionJob { Missing, Managed, Foreign };
struct SessionStatus {
    bool managed{false};
    std::string detail;
};
// Serial control-domain boundary. Neither this policy nor its host edits the
// routing document, replaces an existing job, or terminates an Engine.
class SessionHost {
  public:
    virtual ~SessionHost() = default;
    virtual SessionJob inspect() = 0;
    virtual void validateEngine() = 0;
    virtual void launch() = 0;
};
class SessionSupervisor {
  public:
    explicit SessionSupervisor(std::unique_ptr<SessionHost> host);
    void poll(uint64_t nowMilliseconds, bool allowLaunch = true);
    const SessionStatus& status() const noexcept;
    void retry() noexcept;

  private:
    std::unique_ptr<SessionHost> host_;
    SessionStatus status_;
    uint64_t nextAttempt_{0};
};
std::unique_ptr<SessionHost> productionSessionHost();
} // namespace loopz::macos
