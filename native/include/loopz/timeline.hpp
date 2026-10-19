#pragma once
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace loopz {
inline constexpr uint32_t kProtocol = 1;
inline constexpr uint32_t kChannels = 2;
inline constexpr uint32_t kCapacity = 32768;
inline constexpr uint32_t kBlock = 256;
inline constexpr uint32_t kLatency = 4096;
inline constexpr char kService[] = "com.lexasoft.loopz.bridge";
inline constexpr char kDeviceUID[] = "com.lexasoft.loopz.audio.feasibility.v1";
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

// One serial writer, independent non-destructive readers. Atomic payloads avoid
// the C++ data race hidden inside the usual non-atomic seqlock implementation.
struct alignas(32) Sample {
    std::atomic<uint64_t> tag{UINT64_MAX};
    std::atomic<uint64_t> revision{0};
    std::atomic<uint32_t> left{0}, right{0};
};
struct Timeline {
    Sample samples[kCapacity];
    std::atomic<uint64_t> newest{0};
    void write(uint64_t frame, float left, float right) noexcept {
        auto& sample = samples[frame % kCapacity];
        sample.revision.fetch_add(1, std::memory_order_seq_cst);
        sample.tag.store(UINT64_MAX, std::memory_order_seq_cst);
        sample.left.store(std::bit_cast<uint32_t>(std::isfinite(left) ? left : 0.0f),
                          std::memory_order_seq_cst);
        sample.right.store(std::bit_cast<uint32_t>(std::isfinite(right) ? right : 0.0f),
                           std::memory_order_seq_cst);
        sample.tag.store(frame, std::memory_order_seq_cst);
        sample.revision.fetch_add(1, std::memory_order_seq_cst);
    }
    bool read(uint64_t frame, float& left, float& right) const noexcept {
        const auto& sample = samples[frame % kCapacity];
        const auto revision = sample.revision.load(std::memory_order_seq_cst);
        if (revision & 1) {
            left = right = 0;
            return false;
        }
        if (sample.tag.load(std::memory_order_seq_cst) != frame) {
            left = right = 0;
            return false;
        }
        const auto l = sample.left.load(std::memory_order_seq_cst);
        const auto r = sample.right.load(std::memory_order_seq_cst);
        if (sample.tag.load(std::memory_order_seq_cst) != frame ||
            sample.revision.load(std::memory_order_seq_cst) != revision) {
            left = right = 0;
            return false;
        }
        left = std::bit_cast<float>(l);
        right = std::bit_cast<float>(r);
        return true;
    }
};
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
inline uint64_t hostFrame(uint64_t ns, double rate) noexcept {
    // Split to avoid overflowing a uint64 nanosecond*rate product.
    return (ns / 1000000000) * static_cast<uint64_t>(rate) +
           static_cast<uint64_t>((ns % 1000000000) * rate / 1e9);
}
inline uint64_t frameNs(uint64_t frame, double rate) noexcept {
    return static_cast<uint64_t>(static_cast<long double>(frame) * 1e9L / rate);
}
} // namespace loopz
