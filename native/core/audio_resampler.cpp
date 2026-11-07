#include <loopz/audio_resampler.hpp>
#include <stdexcept>

namespace loopz {
static_assert(kMaximumRoutingChannels == zcore::routing::kMaximumRoutingChannels);
static_assert(AudioResampler::kMaximumFrames == zcore::routing::AudioResampler::kMaximumFrames);
namespace {
zcore::routing::ResamplingFilter filter(ResamplingFilter value) {
    switch (value) {
    case ResamplingFilter::LinearPhase:
        return zcore::routing::ResamplingFilter::LinearPhase;
    case ResamplingFilter::MinimumPhase:
        return zcore::routing::ResamplingFilter::MinimumPhase;
    }
    throw std::invalid_argument("Unsupported resampling filter");
}
} // namespace
AudioResampler::AudioResampler() : AudioResampler(ResamplingFilter::LinearPhase) {}
AudioResampler::AudioResampler(ResamplingFilter selected) : reader_(filter(selected)) {}
uint32_t AudioResampler::read(ResamplePosition position, ResampleOutput output, uint32_t frames,
                              ReadFrame provider, const void* context,
                              std::span<uint8_t> availableFrames) const noexcept {
    // The pinned provider owns cached FIR/convolution and ISA-safe kernels.
    // Position/output fields retain the source interface; the shared result
    // alias forwards the provider directly, without copies, allocation or an
    // extra indirect call for each sample frame.
    return reader_.read({position.anchor, position.offset, position.sourceRate, position.outputRate,
                         position.localClockCorrection, position.nominalSourceRate},
                        {output.channels, output.capacityFrames, output.stride}, frames, provider,
                        context, availableFrames);
}
} // namespace loopz
