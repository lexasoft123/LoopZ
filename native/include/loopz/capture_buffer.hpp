#pragma once
#include <loopz/timeline.hpp>
#include <atomic>

namespace loopz {
// Source-local sample clock, with a copied host-time anchor. A render reader
// interpolates directly against host time; it never consumes another reader's
// samples or blindly equates two nominal device rates. Linear interpolation is
// a feasibility converter, not the release-quality bandlimited ASRC.
class CaptureBuffer {
  public:
    void append(const float* left, const float* right, uint32_t frames, uint64_t firstHostNs,
                double nominalRate, double sourceFrame = NAN) noexcept;
    // Each render reader owns its cache; capture publication never waits for it.
    struct ReaderState {
        uint64_t frame{0}, ns{0}, generation{0};
        double rate{0};
        uint32_t frames{0};
    };
    uint32_t sample(uint64_t firstHostNs, double outputRate, float* left, float* right,
                    uint32_t frames, ReaderState* reader = nullptr) const noexcept;
    struct ClockSnapshot {
        uint64_t anchorNs;
        double measuredRate;
    };
    ClockSnapshot clock() const noexcept {
        const auto before = version_.load();
        const auto ns = anchorNs_.load();
        const auto rate = rateBits_.load();
        return before && !(before & 1) && before == version_.load()
                   ? ClockSnapshot{ns, std::bit_cast<double>(rate)}
                   : ClockSnapshot{};
    }
    std::atomic<uint64_t> callbacks{0};
    std::atomic<uint64_t> discontinuities{0};

  private:
    Timeline timeline_;
    uint64_t cursor_{0}, originHost_{0}, originFrame_{0}, previousHost_{0};
    uint32_t previousFrames_{0};
    double previousRate_{0};
    double previousSourceFrame_{NAN};
    std::atomic<uint32_t> anchorFrames_{0};
    std::atomic<uint64_t> version_{0}, anchorFrame_{0}, anchorNs_{0}, rateBits_{0}, generation_{0};
};
} // namespace loopz
