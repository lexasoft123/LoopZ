#pragma once
#include "broker_binary.hpp"
#include <filesystem>
#include <string_view>
#include <span>
#include <cstddef>

namespace loopz::windows {
// Installer-only. The expected digest must come from trusted package metadata,
// never an IPC request. Production uses the fixed directory; an explicit root
// is provided only for isolated administrative fixtures. Does not register,
// stop or start a service. Returns false when the exact protected binary exists.
bool stageBrokerExecutable(const std::filesystem::path& source, std::string_view expectedSha256,
                           const std::filesystem::path& root = brokerServiceDirectory());
// Same protected publication policy for build-time embedded immutable bytes.
bool stageBrokerBytes(std::span<const std::byte> bytes, std::string_view expectedSha256,
                      const std::filesystem::path& root = brokerServiceDirectory());
// Production maintenance entry: current EXE resources and fixed destination only.
bool stageEmbeddedBroker();
} // namespace loopz::windows
