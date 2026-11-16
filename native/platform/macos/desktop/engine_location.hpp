#pragma once

#include <filesystem>

namespace loopz::macos {
// Slice 1 keeps writes at the legacy target. Migration is an explicit future transaction.
inline const std::filesystem::path legacyEnginePath = "/Applications/LoopZ Engine.app";
inline const std::filesystem::path newEnginePath =
    "/Library/Application Support/LoopZ/LoopZ Engine.app";
inline const auto& installedEngineTarget = legacyEnginePath;
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
inline std::filesystem::path
readEnginePath(const std::filesystem::path& preferred = newEnginePath,
               const std::filesystem::path& legacy = legacyEnginePath) {
    // Stray or incomplete new entries must not shadow a repairable legacy install.
    return readableEngineBundle(preferred) ? preferred : legacy;
}
} // namespace loopz::macos
