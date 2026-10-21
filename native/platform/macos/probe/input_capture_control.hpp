#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace loopz::macos::diagnostic {
inline void inputCaptureEvent(const char* event) {
    if (std::printf("{\"event\":\"%s\",\"protocol\":\"stdio-v1\",\"inputOnly\":true}\n", event) <
            0 ||
        std::fflush(stdout))
        throw std::runtime_error("Input capture controller disconnected");
}
inline void awaitInputCapture(const char* expected, const volatile std::sig_atomic_t& stopping) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::string command;
    while (!stopping && std::chrono::steady_clock::now() < deadline) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&input, 1, 20);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Input capture controller polling failed");
        }
        if (!ready)
            continue;
        char byte;
        if (::read(STDIN_FILENO, &byte, 1) != 1)
            throw std::runtime_error("Input capture controller closed before measurement");
        if (byte == '\n') {
            if (command == expected)
                return;
            throw std::invalid_argument("Unexpected input capture command");
        }
        if (command.size() >= 32)
            throw std::invalid_argument("Input capture command is too long");
        command += byte;
    }
    throw std::runtime_error("Input capture preparation cancelled or expired");
}
} // namespace loopz::macos::diagnostic
