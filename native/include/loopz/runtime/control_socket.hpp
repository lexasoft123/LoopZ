#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace loopz::runtime {
inline constexpr size_t kMaximumControlRequestBytes = 1024 * 1024;
inline constexpr size_t kMaximumControlResponseBytes = 16 * 1024 * 1024;
std::filesystem::path defaultSocketPath();
std::string requestSocket(const std::filesystem::path& path, std::string_view request);
int serveSocket(const std::filesystem::path& path, const std::filesystem::path& state);
} // namespace loopz::runtime
