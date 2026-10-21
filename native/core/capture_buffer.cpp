#include <loopz/capture_buffer.hpp>
#include <algorithm>
#include <array>

namespace loopz {
bool CaptureBuffer::append(const float* left, const float* right, uint32_t frames,
                           uint64_t firstHostNs, double nominalRate, double sourceFrame) noexcept {
    if (!left || !right || !frames || frames > kBlock * 16)
        return false;
    const std::array<const float*, 2> planes{left, right};
    if (!history_.append({planes, frames}, frames, firstHostNs, nominalRate, sourceFrame,
                         std::isfinite(sourceFrame)))
        return false;
    const auto status = history_.status();
    version_.fetch_add(1);
    anchorNs_.store(status.lastHostNs);
    rateBits_.store(std::bit_cast<uint64_t>(status.measuredRate));
    version_.fetch_add(1);
    callbacks.store(status.callbacks);
    discontinuities.store(status.discontinuities);
    return true;
}
void CaptureBuffer::stop() noexcept {
    history_.stop();
    version_.fetch_add(1);
    anchorNs_.store(0);
    rateBits_.store(0);
    version_.fetch_add(1);
}
CaptureBuffer::ClockSnapshot CaptureBuffer::clock() const noexcept {
    const auto before = version_.load();
    const auto ns = anchorNs_.load();
    const auto rate = rateBits_.load();
    return before && !(before & 1) && before == version_.load()
               ? ClockSnapshot{ns, std::bit_cast<double>(rate)}
               : ClockSnapshot{};
}
uint32_t CaptureBuffer::sample(uint64_t firstHostNs, double outputRate, float* left, float* right,
                               uint32_t frames, ReaderState* reader,
                               std::span<uint8_t> availableFrames) const noexcept {
    std::fill_n(availableFrames.begin(), std::min<size_t>(frames, availableFrames.size()),
                uint8_t{0});
    if (!left || !right || !frames || frames > kBlock) {
        // This API accepts at most one DSP block; never clear an unchecked size.
        if (left)
            std::fill_n(left, std::min(frames, kBlock), 0.0f);
        if (right)
            std::fill_n(right, std::min(frames, kBlock), 0.0f);
        return frames;
    }
    AudioReadCursor cursor;
    if (reader)
        cursor = {reader->frame, reader->ns, reader->generation, reader->rate, reader->frames};
    const std::array<float*, 2> planes{left, right};
    const auto result = history_.readWithCoverage(firstHostNs, outputRate, {planes, frames}, frames,
                                                  cursor, availableFrames);
    if (reader)
        *reader = {cursor.frame, cursor.hostNs, cursor.generation, cursor.rate, cursor.frames};
    return result.missingFrames;
}
} // namespace loopz
