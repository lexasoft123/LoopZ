#pragma once

#include <cstdint>
#include <string>

namespace loopz::macos {
// Invoked only after routing-io-check validates both virtual identities and
// existing microphone authorization. Uses one open/start per client throughout.
int routingRecoveryCheck(const std::string& writeUID, const std::string& readUID, uint32_t channels,
                         uint32_t bufferFrames, bool coordinatedBaseline = false);
} // namespace loopz::macos
