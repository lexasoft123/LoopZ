#include <loopz/capture.hpp>
#include <algorithm>

namespace loopz {
void CaptureBuffer::append(const float* left, const float* right, uint32_t frames,
                           uint64_t firstHostNs, double nominalRate, double sourceFrame) noexcept {
    if (!left || !right || !frames || frames > kBlock * 16 || !firstHostNs ||
        !std::isfinite(nominalRate) || nominalRate < 8000 || nominalRate > 192000)
        return;
    version_.fetch_add(1, std::memory_order_seq_cst);
    if (previousHost_) {
        const auto expected = previousHost_ + frameNs(previousFrames_, previousRate_);
        const auto gap = firstHostNs >= expected ? firstHostNs - expected : expected - firstHostNs;
        const bool sourceGap =
            std::isfinite(sourceFrame) && std::isfinite(previousSourceFrame_) &&
            std::fabs(sourceFrame - (previousSourceFrame_ + previousFrames_)) > 0.5;
        if (gap > 2000000 || nominalRate != previousRate_ || sourceGap) {
            generation_.fetch_add(1);
            cursor_ += kCapacity;
            originHost_ = firstHostNs;
            originFrame_ = cursor_;
            discontinuities.fetch_add(1);
        }
    }
    previousHost_ = firstHostNs;
    previousFrames_ = frames;
    previousRate_ = nominalRate;
    previousSourceFrame_ = sourceFrame;
    if (!originHost_) {
        originHost_ = firstHostNs;
        originFrame_ = cursor_;
    }
    double measuredRate = nominalRate;
    if (firstHostNs > originHost_ + 1000000000) {
        measuredRate = (cursor_ - originFrame_) * 1e9 / (firstHostNs - originHost_);
        if (measuredRate < nominalRate * 0.995 || measuredRate > nominalRate * 1.005) {
            originHost_ = firstHostNs;
            originFrame_ = cursor_;
            measuredRate = nominalRate;
        }
    }
    for (uint32_t i = 0; i < frames; ++i)
        timeline_.write(cursor_ + i, left[i], right[i]);
    anchorFrames_.store(frames);
    anchorFrame_.store(cursor_, std::memory_order_seq_cst);
    anchorNs_.store(firstHostNs, std::memory_order_seq_cst);
    rateBits_.store(std::bit_cast<uint64_t>(measuredRate), std::memory_order_seq_cst);
    version_.fetch_add(1, std::memory_order_seq_cst);
    cursor_ += frames;
    callbacks.fetch_add(1, std::memory_order_relaxed);
}
uint32_t CaptureBuffer::sample(uint64_t firstHostNs, double outputRate, float* left, float* right,
                               uint32_t frames, ReaderState* reader) const noexcept {
    std::fill_n(left, frames, 0);
    std::fill_n(right, frames, 0);
    const auto generation = generation_.load();
    ReaderState snapshot{};
    bool coherent = false;
    // A producer can be preempted by this realtime reader while publishing.
    // Two bounded attempts, then reuse this reader's last coherent clock anchor.
    // Timestamp-addressed history makes that anchor valid across normal appends.
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto v = version_.load();
        if (!v || (v & 1))
            continue;
        snapshot = {anchorFrame_.load(), anchorNs_.load(), generation,
                    std::bit_cast<double>(rateBits_.load()), anchorFrames_.load()};
        if (version_.load() == v && generation_.load() == generation) {
            coherent = true;
            break;
        }
    }
    if (!coherent) {
        if (!reader || !reader->ns || reader->generation != generation)
            return frames;
        snapshot = *reader;
    }
    if (!std::isfinite(snapshot.rate) || snapshot.rate <= 0 || !std::isfinite(outputRate) ||
        outputRate <= 0)
        return frames;
    if (reader)
        *reader = snapshot;
    const auto anchorFrame = snapshot.frame, anchorNs = snapshot.ns;
    const double rate = snapshot.rate;
    const double delta = firstHostNs >= anchorNs ? static_cast<double>(firstHostNs - anchorNs)
                                                 : -static_cast<double>(anchorNs - firstHostNs);
    const double first = static_cast<double>(anchorFrame) + delta * rate / 1e9;
    uint32_t missing = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const double position = first + i * rate / outputRate;
        if (position < 0 || position > static_cast<double>(UINT64_MAX - 1)) {
            ++missing;
            continue;
        }
        const auto frame = static_cast<uint64_t>(position);
        const float fraction = static_cast<float>(position - frame);
        float al, ar, bl, br;
        if (!timeline_.read(frame, al, ar) || !timeline_.read(frame + 1, bl, br)) {
            ++missing;
            continue;
        }
        left[i] = al + (bl - al) * fraction;
        right[i] = ar + (br - ar) * fraction;
    }
    if (generation_.load() != generation) {
        std::fill_n(left, frames, 0);
        std::fill_n(right, frames, 0);
        return frames;
    }
    return missing;
}
} // namespace loopz
