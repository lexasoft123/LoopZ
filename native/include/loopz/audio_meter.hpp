#pragma once

#include <loopz/routing.hpp>

#include <array>
#include <atomic>

namespace loopz {
struct AudioMeterSnapshot {
    uint32_t channels{0};
    uint32_t frames{0};
    std::array<float, kMaximumRoutingChannels> peak{}, rms{};
};

// One serial producer and concurrent observers. Payloads are atomic so a
// bounded seqlock retry cannot hide a C++ data race. No render-time allocation.
class AudioMeter {
  public:
    explicit AudioMeter(uint32_t channels);
    // Invalid input clears the observation. Nonfinite samples count as silence.
    void publish(RoutingInput input, uint32_t frames) noexcept;
    // The producer has already established every sample is zero. Positive
    // frames retain a valid silent block; zero clears the observation.
    void publishSilence(uint32_t frames) noexcept;
    void clear() noexcept;
    bool read(AudioMeterSnapshot& result) const noexcept;

  private:
    const uint32_t channels_;
    std::atomic<uint64_t> version_{0};
    std::atomic<uint32_t> frames_{0};
    std::array<std::atomic<uint32_t>, kMaximumRoutingChannels> peak_{}, rms_{};
};
} // namespace loopz
