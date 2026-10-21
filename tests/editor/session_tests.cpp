#include "../../native/platform/macos/desktop/session.hpp"
#include "../../native/platform/macos/desktop/session_host.hpp"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

using namespace loopz::macos;
class Host final : public SessionHost {
  public:
    SessionJob job{SessionJob::Missing};
    unsigned validations{0}, launches{0};
    bool rejected{false}, accepted{true};
    SessionJob inspect() override {
        return job;
    }
    void validateEngine() override {
        ++validations;
        if (rejected)
            throw std::runtime_error("Update required");
    }
    void launch() override {
        ++launches;
        if (accepted)
            job = SessionJob::Managed;
    }
};
int main() {
    auto host = std::make_unique<Host>();
    auto* raw = host.get();
    SessionSupervisor supervisor(std::move(host));
    supervisor.poll(0, false);
    assert(!supervisor.status().managed && !raw->launches);
    supervisor.retry();
    raw->rejected = true;
    supervisor.poll(1);
    assert(!supervisor.status().managed && !raw->launches && raw->validations == 1);
    raw->rejected = false;
    supervisor.poll(100);
    assert(!raw->launches);
    supervisor.poll(5001);
    assert(supervisor.status().managed && raw->launches == 1);
    supervisor.poll(10001);
    assert(raw->launches == 1);
    raw->job = SessionJob::Foreign;
    supervisor.poll(15001);
    assert(!supervisor.status().managed && raw->launches == 1);
    raw->job = SessionJob::Missing;
    raw->accepted = false;
    supervisor.retry();
    supervisor.poll(15002);
    assert(!supervisor.status().managed && raw->launches == 2);
    raw->accepted = true;
    supervisor.poll(20002);
    assert(supervisor.status().managed && raw->launches == 3);
    const auto output = sessionCommand({"/usr/bin/printf", "session-command"});
    assert(output.exitCode == 0 && output.output == "session-command");
    assert(sessionCommand({"/usr/bin/false"}).exitCode == 1);
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        close(0);
        close(1);
        close(2);
        const auto quiet = sessionCommand({"/usr/bin/printf", "closed-stdio"});
        _exit(quiet.exitCode == 0 && quiet.output == "closed-stdio" ? 0 : 1);
    }
    int childStatus = 0;
    assert(waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus) &&
           WEXITSTATUS(childStatus) == 0);
    bool bounded = false;
    try {
        sessionCommand({"/usr/bin/yes"});
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    assert(bounded);
    bounded = false;
    try {
        sessionCommand({"/bin/sleep", "6"});
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    assert(bounded);
    std::puts("Session policy preserves foreign jobs, validates before launch, retries failures "
              "and bounds child processes");
}
