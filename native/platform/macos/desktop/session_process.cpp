#include "session_host.hpp"

#include <array>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace loopz::macos {
SessionCommandResult sessionCommand(const std::vector<std::string>& arguments,
                                    std::chrono::milliseconds timeout,
                                    const std::atomic<bool>* running,
                                    SessionCommandOutput outputMode, size_t maximumOutputBytes) {
    if (arguments.empty() || arguments[0].empty())
        throw std::invalid_argument("Missing session command");
    if (!maximumOutputBytes || maximumOutputBytes > 32 * 1024 * 1024)
        throw std::invalid_argument("Invalid session response limit.");
    if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(70))
        throw std::invalid_argument("The command timeout must be positive and at most 70 seconds.");
    if (running && !running->load())
        throw std::runtime_error("The request was canceled.");
    int pipes[2];
    if (pipe(pipes))
        throw std::runtime_error("The engine launcher could not create a response pipe.");
    // A background process may have closed standard descriptors. Keep both
    // pipe ends above stderr before installing child dup/close actions.
    const int input = fcntl(pipes[0], F_DUPFD_CLOEXEC, 3);
    const int output = fcntl(pipes[1], F_DUPFD_CLOEXEC, 3);
    close(pipes[0]);
    close(pipes[1]);
    if (input < 0 || output < 0 || fcntl(input, F_SETFL, O_NONBLOCK)) {
        if (input >= 0)
            close(input);
        if (output >= 0)
            close(output);
        throw std::runtime_error("The engine launcher could not configure its response pipe.");
    }
    pipes[0] = input;
    pipes[1] = output;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        close(pipes[0]);
        close(pipes[1]);
        throw std::runtime_error("The engine launcher could not prepare its child process.");
    }
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) ||
        posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO) ||
        (outputMode == SessionCommandOutput::Combined
             ? posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO)
             : posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY,
                                                0)) ||
        posix_spawn_file_actions_addclose(&actions, pipes[0]) ||
        posix_spawn_file_actions_addclose(&actions, pipes[1])) {
        posix_spawn_file_actions_destroy(&actions);
        close(pipes[0]);
        close(pipes[1]);
        throw std::runtime_error("The engine launcher could not prepare its child descriptors.");
    }
    std::vector<char*> argv;
    for (const auto& item : arguments)
        argv.push_back(const_cast<char*>(item.c_str()));
    argv.push_back(nullptr);
    pid_t child = 0;
    const auto launched =
        posix_spawn(&child, arguments[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (launched) {
        close(pipes[0]);
        throw std::runtime_error("The engine launcher could not start its command.");
    }
    SessionCommandResult result{-1, {}};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    bool finished = false, invalid = false, lostChild = false;
    std::array<char, 4096> buffer{};
    while (!finished && !invalid && (!running || running->load()) &&
           std::chrono::steady_clock::now() < deadline) {
        while (true) {
            const auto count = read(pipes[0], buffer.data(), buffer.size());
            if (count <= 0)
                break;
            if (result.output.size() + static_cast<size_t>(count) > maximumOutputBytes) {
                invalid = true;
                break;
            }
            result.output.append(buffer.data(), static_cast<size_t>(count));
        }
        const auto waited = waitpid(child, &status, WNOHANG);
        finished = waited == child;
        if (waited < 0 && errno != EINTR) {
            invalid = true;
            lostChild = true;
        }
        if (!finished && !invalid) {
            pollfd descriptor{pipes[0], POLLIN, 0};
            poll(&descriptor, 1, 10);
        }
    }
    if (!finished && !lostChild) {
        // This unreaped child belongs to this invocation; no PID lookup or
        // process-name signal is used. Keep the bound even for a hung tool.
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    } else {
        while (true) {
            const auto count = read(pipes[0], buffer.data(), buffer.size());
            if (count <= 0)
                break;
            if (result.output.size() + static_cast<size_t>(count) > maximumOutputBytes) {
                invalid = true;
                break;
            }
            result.output.append(buffer.data(), static_cast<size_t>(count));
        }
    }
    close(pipes[0]);
    if (running && !running->load())
        throw std::runtime_error("The request was canceled.");
    if (!finished || invalid)
        throw std::runtime_error("The engine launcher response timed out or exceeded its limit.");
    if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    return result;
}
} // namespace loopz::macos
