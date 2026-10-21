#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace loopz::macos {
struct AudioMappedImage {
    uint64_t device{}, inode{};
    bool operator==(const AudioMappedImage&) const = default;
};
// Parse a complete NUL-delimited lsof inventory. No process may be signalled
// unless its actual mapped image belongs exclusively to LoopZ's HAL slot.
std::optional<AudioMappedImage> loopZMappedImage(std::string_view inventory, int pid);
} // namespace loopz::macos
