#pragma once
#include <cstddef>
#include <span>
#include <string>
namespace loopz::windows {
struct EmbeddedBrokerPayload {
    // Resource memory stays valid for the lifetime of the current executable.
    std::span<const std::byte> bytes;
    std::string sha256;
};
// Reads only this executable's build-time resources. No external metadata or
// module/path override. Release trust comes from the maintenance EXE signature.
EmbeddedBrokerPayload embeddedBrokerPayload();
} // namespace loopz::windows
