#pragma once
#include <loopz/timeline.hpp>

namespace loopz {
// Darwin process-shared ABI. Backends must define their own transport contract.
inline constexpr uint32_t kProtocol = 1;
inline constexpr char kService[] = "com.lexasoft.loopz.bridge";
inline constexpr char kDeviceUID[] = "com.lexasoft.loopz.audio.feasibility.v1";

struct SharedAudio {
    uint64_t magic{0x4c4f4f505a303031};
    uint32_t protocol{kProtocol};
    uint32_t bytes{sizeof(SharedAudio)};
    std::atomic<uint64_t> helperHeartbeat{0};
    std::atomic<uint64_t> driverHeartbeat{0};
    std::atomic<uint32_t> rate{48000};
    std::atomic<uint32_t> helperOnline{0};
    std::atomic<uint64_t> reads{0}, missing{0}, writes{0};
    Timeline capture;  // helper writes; driver reads by virtual sample host time
    Timeline playback; // driver writes mixed client output; helper reads
};
inline bool valid(const SharedAudio* audio) noexcept {
    return audio && audio->magic == 0x4c4f4f505a303031 && audio->protocol == kProtocol &&
           audio->bytes == sizeof(SharedAudio);
}
} // namespace loopz
