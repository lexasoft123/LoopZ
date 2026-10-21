#include "session.hpp"
#include <stdexcept>
#include <utility>

namespace loopz::macos {
SessionSupervisor::SessionSupervisor(std::unique_ptr<SessionHost> host) : host_(std::move(host)) {
    if (!host_)
        throw std::invalid_argument("A background session needs a host");
}
void SessionSupervisor::poll(uint64_t now, bool allowLaunch) {
    if (now < nextAttempt_)
        return;
    nextAttempt_ = now + 5000;
    try {
        const auto job = host_->inspect();
        if (job == SessionJob::Foreign)
            throw std::runtime_error("Another engine job occupies LoopZ’s session. Quit that "
                                     "session before starting this one.");
        if (job == SessionJob::Missing) {
            if (!allowLaunch) {
                status_ = {false, "Routing is provided by a separate Engine session."};
                return;
            }
            host_->validateEngine();
            host_->launch();
            // A successful launch request is not proof that launchd accepted
            // the expected job. Reinspect before advertising supervision.
            if (host_->inspect() != SessionJob::Managed)
                throw std::runtime_error("The background engine is still starting.");
        }
        status_ = {true, "The audio engine restarts automatically after an unexpected exit."};
    } catch (const std::exception& error) {
        status_ = {false, error.what()};
    }
}
const SessionStatus& SessionSupervisor::status() const noexcept {
    return status_;
}
void SessionSupervisor::retry() noexcept {
    nextAttempt_ = 0;
}
} // namespace loopz::macos
