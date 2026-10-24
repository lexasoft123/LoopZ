#include "catalog_channel.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace loopz::macos {
namespace {
using Clock = std::chrono::steady_clock;
struct Pipe {
    std::array<int, 2> ends{-1, -1};
    Pipe() {
        int raw[2];
        if (pipe(raw))
            throw std::runtime_error("The catalog channel could not create its pipes.");
        for (size_t i = 0; i < ends.size(); ++i)
            ends[i] = fcntl(raw[i], F_DUPFD_CLOEXEC, 3);
        close(raw[0]);
        close(raw[1]);
        if (ends[0] < 0 || ends[1] < 0) {
            for (auto fd : ends)
                if (fd >= 0)
                    close(fd);
            throw std::runtime_error("The catalog channel could not protect its descriptors.");
        }
    }
    ~Pipe() {
        for (auto fd : ends)
            if (fd >= 0)
                close(fd);
    }
};
void ready(int fd, short events, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        pollfd item{fd, events, 0};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const auto result = poll(&item, 1, static_cast<int>(std::max<int64_t>(1, remaining)));
        if (result > 0 && (item.revents & (events | POLLHUP)))
            return;
        if (result < 0 && errno == EINTR)
            continue;
        if (result != 0)
            throw std::runtime_error("The catalog channel closed unexpectedly.");
    }
    throw std::runtime_error("The catalog checker response timed out.");
}
} // namespace
CatalogChannel::CatalogChannel(const std::vector<std::string>& arguments,
                               std::chrono::milliseconds timeout)
    : timeout_(timeout) {
    if (arguments.empty() || arguments.front().empty() ||
        timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(30))
        throw std::invalid_argument("Invalid catalog channel launch.");
    Pipe request, response;
    if (fcntl(request.ends[1], F_SETFL, O_NONBLOCK) ||
        fcntl(response.ends[0], F_SETFL, O_NONBLOCK) || fcntl(request.ends[1], F_SETNOSIGPIPE, 1))
        throw std::runtime_error("The catalog channel could not configure its pipes.");
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions))
        throw std::runtime_error("The catalog checker could not prepare its launch.");
    struct Actions {
        posix_spawn_file_actions_t* value;
        ~Actions() {
            posix_spawn_file_actions_destroy(value);
        }
    } cleanup{&actions};
    bool invalid =
        posix_spawn_file_actions_adddup2(&actions, request.ends[0], STDIN_FILENO) ||
        posix_spawn_file_actions_adddup2(&actions, response.ends[1], STDOUT_FILENO) ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    for (auto fd : {request.ends[0], request.ends[1], response.ends[0], response.ends[1]})
        invalid = posix_spawn_file_actions_addclose(&actions, fd) || invalid;
    if (invalid)
        throw std::runtime_error("The catalog checker descriptors could not be prepared.");
    std::vector<char*> argv;
    for (const auto& value : arguments)
        argv.push_back(const_cast<char*>(value.c_str()));
    argv.push_back(nullptr);
    if (posix_spawn(&child_, arguments.front().c_str(), &actions, nullptr, argv.data(), environ)) {
        child_ = -1;
        throw std::runtime_error("The catalog checker could not be started.");
    }
    input_ = request.ends[1];
    request.ends[1] = -1;
    output_ = response.ends[0];
    response.ends[0] = -1;
}
CatalogChannel::~CatalogChannel() {
    stop();
}
void CatalogChannel::stop() noexcept {
    if (input_ >= 0)
        close(input_);
    if (output_ >= 0)
        close(output_);
    input_ = output_ = -1;
    if (child_ < 0)
        return;
    int status = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(failed_ ? 0 : 2);
    do {
        const auto result = waitpid(child_, &status, WNOHANG);
        if (result == child_ || (result < 0 && errno == ECHILD)) {
            child_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (Clock::now() < deadline);
    // This unreaped child is still owned by this channel, so its PID cannot
    // have been recycled. Never discover or signal another process by name.
    kill(child_, SIGKILL);
    const auto killedDeadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < killedDeadline) {
        const auto result = waitpid(child_, &status, WNOHANG);
        if (result == child_ || (result < 0 && errno == ECHILD)) {
            child_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // A kernel-blocked child must not stall transaction recovery. Its private
    // pipes are closed and SIGKILL is pending. Reap only this unreaped PID.
    const auto pending = child_;
    child_ = -1;
    try {
        std::thread([pending] {
            int result;
            while (waitpid(pending, &result, 0) < 0 && errno == EINTR) {
            }
        }).detach();
    } catch (...) {
        // The short-lived maintenance service will reparent it on exit.
    }
}
std::string CatalogChannel::exchange(std::string_view request) {
    if (failed_ || input_ < 0 || request.empty() || request.size() > 64 || request.back() != '\n' ||
        request.find('\n') != request.size() - 1)
        throw std::invalid_argument("Invalid catalog request.");
    const auto deadline = Clock::now() + timeout_;
    try {
        size_t sent = 0;
        while (sent < request.size()) {
            ready(input_, POLLOUT, deadline);
            const auto count = write(input_, request.data() + sent, request.size() - sent);
            if (count > 0)
                sent += static_cast<size_t>(count);
            else if (count < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            else
                throw std::runtime_error("The catalog checker stopped accepting requests.");
        }
        std::string response;
        std::array<char, 4096> bytes{};
        while (true) {
            ready(output_, POLLIN, deadline);
            const auto count = read(output_, bytes.data(), bytes.size());
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            if (count <= 0)
                throw std::runtime_error("The catalog checker closed without a response.");
            if (response.size() + static_cast<size_t>(count) > 32 * 1024 * 1024)
                throw std::runtime_error("The catalog response exceeded its limit.");
            response.append(bytes.data(), static_cast<size_t>(count));
            const auto end = response.find('\n');
            if (end == std::string::npos)
                continue;
            if (end != response.size() - 1)
                throw std::runtime_error("The catalog checker sent an unexpected extra response.");
            response.pop_back();
            return response;
        }
    } catch (...) {
        failed_ = true;
        stop();
        throw;
    }
}
} // namespace loopz::macos
