#pragma once

#include <loopz/routing.hpp>
#include <zcore/routing/audio_resampler.h>

namespace loopz {
enum class ResamplingFilter { LinearPhase, MinimumPhase };

struct ResamplePosition {
    uint64_t anchor;
    long double offset;
    // Nominal formats are 8–192 kHz. Either clock estimate may differ from
    // those endpoint limits by at most 0.5% to account for physical drift.
    double sourceRate, outputRate;
    // Only a bounded, sample-counter-verified closed timestamp interval may
    // use a locally corrected slope. Endpoint/output rate limits do not change.
    bool localClockCorrection{false};
    // Optional fixed source format for minimum-phase filter shape. Sample
    // positions still follow sourceRate; the linear-phase reader ignores it.
    double nominalSourceRate{0};
};
// Alias the shared provider result so callbacks cross no per-frame trampoline.
using ResampleFrame = zcore::routing::ResampleFrame;
struct ResampleOutput {
    std::span<float* const> channels;
    // Capacity is measured in logical frames. Each plane must span
    // (capacityFrames - 1) * stride + 1 floats; stride supports interleaving.
    uint32_t capacityFrames;
    uint32_t stride{1};
};

// Construct on control to prepare the immutable sinc table. read() owns only
// bounded stack state; concurrent callers need no shared cursor or workspace.
// The provider supplies timestamp-addressed frames, including all filter wings.
// Missing support is silent and counted, without a linear fallback or time shift.
// The default keeps the existing symmetric kernel and aligned direct reads.
// Cached FIR/convolution fast paths come from the pinned zCore implementation.
// Experimental minimum-phase mode uses past support only and always filters,
// including integer-aligned targets, to keep its nonlinear phase continuous.
class AudioResampler {
  public:
    using ReadFrame = ResampleFrame (*)(const void*, uint64_t, std::span<float>) noexcept;
    static constexpr uint32_t kMaximumFrames = 2048;
    AudioResampler();
    explicit AudioResampler(ResamplingFilter filter);
    uint32_t read(ResamplePosition position, ResampleOutput output, uint32_t frames,
                  ReadFrame provider, const void* context,
                  std::span<uint8_t> availableFrames = {}) const noexcept;

  private:
    zcore::routing::AudioResampler reader_;
};
} // namespace loopz
