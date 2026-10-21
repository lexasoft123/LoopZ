#pragma once

#import <Foundation/Foundation.h>
#include <array>
#include <filesystem>
#include <functional>
#include <string>

namespace loopz::macos {
enum class InstallationState { Unchecked, Installed, Missing, Invalid, Unknown };
struct InstallationComponent {
    InstallationState state{InstallationState::Unchecked};
    std::string detail;
};
struct InstallationSnapshot {
    // Engine, driver, protected audio service. Immutable after publication.
    std::array<InstallationComponent, 3> components;
    bool updateAvailable{false};
    bool maintenanceAvailable{false};
    bool activationRequired{false};
};
using InstallationProbe = std::function<InstallationSnapshot()>;
struct InstallationPaths {
    std::filesystem::path engine, legacyDriver, routingDriver, broker, daemon;
};
// Read-only control operations. Fixtures inject paths and verification; no GUI
// request can select a path, signing identity, command or alternate installer.
using InstallationCodeCheck =
    std::function<bool(const std::filesystem::path&, const std::string&, bool protectedFiles)>;
using InstallationDaemonCheck = std::function<bool(const std::filesystem::path&)>;
InstallationProbe makeInstallationProbe(InstallationPaths paths, InstallationCodeCheck code,
                                        InstallationDaemonCheck daemon);
InstallationProbe productionInstallationProbe();
NSDictionary* installationJSON(const InstallationSnapshot& snapshot);
} // namespace loopz::macos
