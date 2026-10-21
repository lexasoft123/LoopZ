#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace loopz::macos {
// Bounded regular-file reads reject leaf symlinks. Replacement writes use a
// private sibling, flush it, and atomically publish mode 0600 on the same volume.
std::string readLocalFile(const std::filesystem::path& path, size_t maximumBytes);
void writeLocalFileAtomic(const std::filesystem::path& path, std::string_view bytes);
} // namespace loopz::macos
