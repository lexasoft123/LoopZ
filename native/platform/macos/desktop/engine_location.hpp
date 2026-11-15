#pragma once

#include <filesystem>

namespace loopz::macos {
inline constexpr char engineBundleIdentifier[] = "com.lexasoft.loopz.engine";
inline constexpr char engineJobLabel[] = "com.lexasoft.loopz.session";
inline constexpr char engineControlService[] = "com.lexasoft.loopz.editor.control";
inline constexpr char engineRequirement[] =
    "identifier \"com.lexasoft.loopz.engine\" and anchor apple generic and "
    "certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate "
    "leaf[field.1.2.840.113635.100.6.1.13] exists and certificate "
    "leaf[subject.OU] = \"USJ7H3X44X\"";

// Migration runs only in explicitly requested privileged audio maintenance.
inline const std::filesystem::path legacyEnginePath = "/Applications/LoopZ Engine.app";
inline const std::filesystem::path newEnginePath =
    "/Library/Application Support/LoopZ/LoopZ Engine.app";
inline const auto& installedEngineTarget = newEnginePath;
// Flip only after the real-Mac no-prompt system-audio consent acceptance test.
inline constexpr bool kMigrateApplicationCaptureGraphs = false;
inline bool engineMigrationPending(bool applicationCapture, bool legacy,
                                   bool enabled = kMigrateApplicationCaptureGraphs) {
    return applicationCapture && legacy && !enabled;
}
inline bool installedEnginePath(const std::filesystem::path& path) {
    return path == newEnginePath || path == legacyEnginePath;
}
// Structural admission only; callers still verify the signing identity/capabilities.
inline bool readableEngineBundle(const std::filesystem::path& bundle) {
    std::error_code error;
    for (const auto& path : {bundle, bundle / "Contents", bundle / "Contents/MacOS"}) {
        if (std::filesystem::symlink_status(path, error).type() !=
                std::filesystem::file_type::directory ||
            error)
            return false;
    }
    const auto executable =
        std::filesystem::symlink_status(bundle / "Contents/MacOS/LoopZ Engine", error);
    return !error && executable.type() == std::filesystem::file_type::regular &&
           (executable.permissions() &
            (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
             std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
}
// Include real incomplete bundles so Repair can replace them in place. Links
// and stray regular files are not an installed legacy bundle.
inline bool legacyEnginePresent(const std::filesystem::path& bundle = legacyEnginePath) {
    std::error_code error;
    return readableEngineBundle(bundle) || (std::filesystem::symlink_status(bundle, error).type() ==
                                                std::filesystem::file_type::directory &&
                                            !error);
}
inline std::filesystem::path
readEnginePath(const std::filesystem::path& preferred = newEnginePath,
               const std::filesystem::path& legacy = legacyEnginePath) {
    // Stray or incomplete new entries must not shadow a repairable legacy install.
    return readableEngineBundle(preferred) ? preferred : legacy;
}
} // namespace loopz::macos
