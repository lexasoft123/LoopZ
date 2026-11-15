#pragma once

#include "audio_maintenance.hpp"
#include "session_host.hpp"
#include <functional>

namespace loopz::macos {
// Native-only adapter injection. No bridge/XPC request can supply these hooks.
// Tests call the production transaction with real private filesystem state;
// only OS processes, signing and hardware discovery are replaced.
struct AudioMaintenanceInterruption : std::exception {};
struct AudioMaintenanceFixture {
    std::filesystem::path root;
    bool migrateApplicationCapture{false};
    std::function<SessionCommandResult(const std::vector<std::string>&, bool)> command;
    std::function<void(const std::filesystem::path&, NSString*)> verify;
    std::function<NSDictionary*()> catalog;
    std::function<void()> validateGraph;
    std::function<void()> activate;
    std::function<void(NSDictionary*)> restoreDefaults;
    std::function<void(const char*)> checkpoint;
};
NSDictionary* maintainAudioComponents(const std::filesystem::path& app, bool allowAudioActivation,
                                      uid_t expectedUser, AudioMaintenanceFixture& fixture);
} // namespace loopz::macos
