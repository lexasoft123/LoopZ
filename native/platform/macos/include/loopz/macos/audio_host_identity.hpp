#pragma once
#include <string>
#include <string_view>

namespace loopz::macos {
// Intel macOS 15 and newer Apple silicon systems use distinct remote hosts.
// Keep the driver and broker allowlists identical; neither accepts other apps.
inline constexpr char kAudioHostRequirement[] =
    "anchor apple and (identifier \"com.apple.audio.coreaudiod\" or "
    "identifier \"com.apple.audio.Core-Audio-Driver-Service\" or "
    "identifier \"com.apple.audio.Core-Audio-Driver-Service.helper\")";

inline bool isolatedAudioHostPath(std::string_view path) {
    constexpr std::string_view base =
        "/System/Library/Frameworks/CoreAudio.framework/Versions/A/XPCServices/";
    return path == std::string(base) +
                       "com.apple.audio.Core-Audio-Driver-Service.xpc/Contents/MacOS/"
                       "com.apple.audio.Core-Audio-Driver-Service" ||
           path == std::string(base) +
                       "com.apple.audio.Core-Audio-Driver-Service.helper.xpc/Contents/MacOS/"
                       "com.apple.audio.Core-Audio-Driver-Service.helper";
}
} // namespace loopz::macos
