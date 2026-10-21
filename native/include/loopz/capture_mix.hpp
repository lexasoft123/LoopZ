#pragma once

#include <loopz/capture_buffer.hpp>
#include <array>

namespace loopz {
struct CaptureMixReader {
    std::array<CaptureBuffer::ReaderState, kMaximumRoutingSources> streams{};
};
// Timestamp-align independent stereo histories and sum at unity. Histories and
// their ordered membership must remain alive/unchanged for this call. One serial
// render owner per reader; no allocation, OS query, limiter or level normalization.
// Missing frames count the union of incomplete streams, while valid contributions
// continue. A valid all-zero stream is not missing audio.
uint32_t sampleCaptureMix(std::span<const CaptureBuffer* const> captures, uint64_t firstHostNs,
                          double rate, float* left, float* right, uint32_t frames,
                          CaptureMixReader& reader) noexcept;
} // namespace loopz
