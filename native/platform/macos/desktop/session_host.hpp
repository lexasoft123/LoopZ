#pragma once

#include "session.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
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
                                    SessionCommandOutput output = SessionCommandOutput::Combined,
                                    size_t maximumOutputBytes = 65536);
struct SessionPaths {
    std::filesystem::path engine, document, directory;
    std::string label, service;
    std::filesystem::path legacyEngine{};    // Native fixtures may supply a private legacy path.
    std::filesystem::path preferredEngine{}; // The matching preferred path for inspect fixtures.
};
// Explicit paths are dependency injection for isolated fixtures. The desktop
// and login helper expose no endpoint, path or command overrides. The optional
// inspect command and legacy path are native-only seams for isolated inspect fixtures.
using SessionInspectCommand = std::function<SessionCommandResult(const std::vector<std::string>&)>;
std::unique_ptr<SessionHost> makeSessionHost(SessionPaths paths,
                                             const std::string& engineRequirement,
                                             SessionInspectCommand inspectCommand = {});
// Shared account launch factory, also used by private malformed-job fixtures.
std::unique_ptr<SessionHost> makeAccountSessionHost(SessionPaths paths,
                                                    const std::string& engineRequirement,
                                                    SessionInspectCommand inspectCommand = {});
} // namespace loopz::macos
