#pragma once
#include "driver/audio_format.hpp"
#include <string>
namespace loopz::windows {
// Privileged setup only. Refresh the two endpoints proven by typed ancestry to
// belong to this mix, and only if their public format differs from the request.
// Tries documented reset, then a known, guarded policy setter if Windows retains
// an unsupported factory selection. Never writes MMDevice properties directly or
// changes system defaults, endpoint volume, or other drivers.
// Render-only sink: fixed 48 kHz stereo, with no identified capture endpoint
// across active, disabled, unplugged and not-present endpoint states.
void ensureAppRoutingSinkEndpointFormat(const std::string& sinkKey);
void ensureMixEndpointFormat(const std::string& mixKey, loopz_driver::AudioFormat format);
} // namespace loopz::windows
