#pragma once
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace loopz {
inline constexpr uint32_t kChannels = 2;
inline constexpr uint32_t kCapacity = 32768;
inline constexpr uint32_t kBlock = 256;
inline constexpr uint32_t kLatency = 4096;
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
inline uint64_t hostFrame(uint64_t ns, double rate) noexcept {
    // Split to avoid overflowing a uint64 nanosecond*rate product.
    return (ns / 1000000000) * static_cast<uint64_t>(rate) +
           static_cast<uint64_t>(static_cast<double>(ns % 1000000000) * rate / 1e9);
}
inline uint64_t frameNs(uint64_t frame, double rate) noexcept {
    return static_cast<uint64_t>(static_cast<long double>(frame) * 1e9L / rate);
}
} // namespace loopz
