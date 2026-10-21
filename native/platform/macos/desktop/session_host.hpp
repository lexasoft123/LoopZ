#pragma once

#include "session.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>

namespace loopz::macos {
struct SessionCommandResult {
    int exitCode;
    std::string output;
};
enum class SessionCommandOutput { Combined, StandardOutput };
// Cancellation terminates only this invocation's unreaped child. Normal
// session commands retain their five-second limit; explicit consent may wait.
SessionCommandResult sessionCommand(const std::vector<std::string>& arguments,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(5),
                                    const std::atomic<bool>* running = nullptr,
                                    SessionCommandOutput output = SessionCommandOutput::Combined);
struct SessionPaths {
    std::filesystem::path engine, document, directory;
    std::string label, service;
};
// Explicit paths are dependency injection for isolated fixtures. The desktop
// and login helper expose no endpoint, path or command overrides.
std::unique_ptr<SessionHost> makeSessionHost(SessionPaths paths,
                                             const std::string& engineRequirement);
} // namespace loopz::macos
