#pragma once

#include <loopz/audio_resampler.hpp>
#include <loopz/sinc_kernel.hpp>
#include <loopz/timeline.hpp>
#include <memory>

namespace loopz {
struct CausalResampleBlock {
    bool valid{false};
    uint64_t firstFrame{0};
    uint32_t frames{0};
    // Owned by the converter; valid until its next convert/reset/destruction.
    std::span<const float> interleaved;
};

// Prepared on control, then owned by one serial render thread. Retains bounded
// input history and produces contiguous output timestamps without future input.
// A discontinuity starts a new, zero-padded stream and cannot replay old audio.
class CausalResampler {
  public:
    // Conservative bound for conversion to a higher nominal rate. The extra
    // frame makes the symmetric kernel strictly causal at block boundaries.
    static constexpr uint32_t kMaximumUpsamplingDelayFrames =
        static_cast<uint32_t>(SincKernel::kRadius / SincKernel::kCutoff) + 2;
    CausalResampler(uint32_t channels, uint32_t sourceRate, uint32_t outputRate,
                    uint32_t maximumInputFrames = kBlock);
    ~CausalResampler();
    CausalResampler(const CausalResampler&) = delete;
    CausalResampler& operator=(const CausalResampler&) = delete;
    // Delay is expressed in the source clock. Zero denotes an unsupported rate.
    static uint32_t delayFrames(uint32_t sourceRate, uint32_t outputRate) noexcept;
    uint32_t sourceRate() const noexcept;
    uint32_t delayFrames() const noexcept;
    CausalResampleBlock convert(uint64_t firstFrame, RoutingInput input, uint32_t frames) noexcept;
    void reset() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
