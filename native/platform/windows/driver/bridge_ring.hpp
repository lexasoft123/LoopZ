// LoopZ contributors. MIT license. Caller serializes all operations.
#pragma once
#include "audio_format.hpp"
#include "byte_copy.hpp"

namespace loopz_driver {
// Configured interleaved frames; the sample width is fixed by the type.
// Samples are opaque bytes here. The adapter owns optional external
// nonpaged storage. The default storage preserves the legacy stereo contract.
// No allocation, waiting, or user pointers.
template <unsigned SampleBytes> class SampleBridgeRing {
    static_assert(SampleBytes == 2 || SampleBytes == 4);

  public:
    SampleBridgeRing() = default;
    SampleBridgeRing(const SampleBridgeRing&) = delete;
    SampleBridgeRing& operator=(const SampleBridgeRing&) = delete;
    static constexpr unsigned capacity = 9600;
    static constexpr unsigned prefill = 960;
    bool configure(AudioFormat format, unsigned char* storage, unsigned bytes) noexcept {
        if (!format.valid() || !storage ||
            bytes < format.ringFrames() * format.channels * SampleBytes)
            return false;
        data_ = storage;
        capacity_ = format.ringFrames();
        prefill_ = format.prefillFrames();
        frameBytes_ = format.channels * SampleBytes;
        capturing_ = false;
        reset(false);
        return true;
    }
    void reset(bool running) noexcept {
        read_ = count_ = 0;
        running_ = running;
        primed_ = false;
    }
    void capture(bool active) noexcept {
        read_ = count_ = 0;
        primed_ = false;
        capturing_ = active;
    }
    void write(const unsigned char* bytes, unsigned frames) noexcept {
        if (!running_ || !capturing_)
            return;
        if (frames > capacity_) {
            bytes += (frames - capacity_) * frameBytes_;
            frames = capacity_;
        }
        if (count_ + frames > capacity_) {
            const unsigned drop = count_ + frames - capacity_;
            read_ = (read_ + drop) % capacity_;
            count_ -= drop;
            dropped_ += drop;
        }
        const auto slot = (read_ + count_) % capacity_;
        const auto first = frames < capacity_ - slot ? frames : capacity_ - slot;
        if (first)
            copyBytes(data_ + slot * frameBytes_, bytes, first * frameBytes_);
        if (frames > first)
            copyBytes(data_, bytes + first * frameBytes_, (frames - first) * frameBytes_);
        count_ += frames;
    }
    void read(unsigned char* bytes, unsigned frames) noexcept {
        if (!primed_ && running_ && count_ >= prefill_)
            primed_ = true;
        const unsigned available = primed_ && running_ ? (frames < count_ ? frames : count_) : 0;
        const auto first = available < capacity_ - read_ ? available : capacity_ - read_;
        if (first)
            copyBytes(bytes, data_ + read_ * frameBytes_, first * frameBytes_);
        if (available > first)
            copyBytes(bytes + first * frameBytes_, data_, (available - first) * frameBytes_);
        if (frames > available)
            zeroBytes(bytes + available * frameBytes_, (frames - available) * frameBytes_);
        read_ = (read_ + available) % capacity_;
        count_ -= available;
        if (available < frames) {
            if (primed_ && running_)
                ++underruns_;
            missing_ += frames - available;
            primed_ = false;
        }
    }
    unsigned queued() const noexcept {
        return count_;
    }
    unsigned long long missing() const noexcept {
        return missing_;
    }
    unsigned long long dropped() const noexcept {
        return dropped_;
    }
    unsigned long long underruns() const noexcept {
        return underruns_;
    }

  private:
    unsigned char fallback_[capacity * 2 * SampleBytes]{};
    unsigned char* data_ = fallback_;
    unsigned capacity_ = capacity, prefill_ = prefill, frameBytes_ = 2 * SampleBytes;
    unsigned read_ = 0, count_ = 0;
    bool running_ = false, primed_ = false, capturing_ = false;
    unsigned long long missing_ = 0, dropped_ = 0, underruns_ = 0;
};
using BridgeRing = SampleBridgeRing<2>;
using FloatBridgeRing = SampleBridgeRing<4>;
} // namespace loopz_driver
