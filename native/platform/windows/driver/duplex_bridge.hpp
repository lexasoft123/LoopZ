// LoopZ contributors. MIT license.
#pragma once
#include "bridge_ring.hpp"
#include "frame_clock.hpp"

namespace loopz_driver {
struct BridgeDiagnostics {
    unsigned queuedFrames = 0, ingressPackets = 0;
    unsigned long long missingFrames = 0, droppedFrames = 0, underruns = 0, lastUnderrunTime = 0;
    unsigned lastUnderrunQueued = 0, lastUnderrunRequested = 0;
    unsigned long long recordingFrames = 0, leaseSilentFrames = 0;
    unsigned long long sampleTime = 0; // Filled by the property handler, in 100 ns.
};

// Private engine transport, separate from the public recording stream. Callers
// serialize access, validate kernel property buffers, and authorize the owner.
// All clocks use monotonic 100 ns units. No allocation, waiting or user pointers.
template <unsigned SampleBytes> class SampleDuplexBridge {
    static_assert(SampleBytes == 2 || SampleBytes == 4);

  public:
    SampleDuplexBridge() = default;
    SampleDuplexBridge(const SampleDuplexBridge&) = delete;
    SampleDuplexBridge& operator=(const SampleDuplexBridge&) = delete;
    static constexpr unsigned sampleBytes = SampleBytes;
    // Keep the payload byte limit and total ingress frame budget unchanged.
    static constexpr unsigned packetFrames = 960 / SampleBytes;
    static constexpr unsigned packetCapacity = 9600 / packetFrames;
    static constexpr unsigned long long leaseDuration = 1000000; // 100 ms
    using Diagnostics = BridgeDiagnostics;
    Diagnostics diagnostics() const noexcept {
        return {processed_.queued(),    count_,
                processed_.missing(),   processed_.dropped(),
                processed_.underruns(), lastUnderrunTime_,
                lastUnderrunQueued_,    lastUnderrunRequested_,
                recordingFrames_,       leaseSilentFrames_};
    }
    struct Packet {
        unsigned long long time = 0;
        unsigned long long sequence = 0;
        unsigned frames = 0;
        unsigned discontinuity = 0;
        unsigned char samples[packetFrames * AudioFormat::maxChannels * SampleBytes]{};
    };

    static unsigned ingressBytes(AudioFormat format) noexcept {
        return packetCapacity * packetFrames * format.channels * SampleBytes;
    }
    bool configure(AudioFormat format, unsigned char* ingress, unsigned ingressSize,
                   unsigned char* recording, unsigned recordingSize) noexcept {
        if (engine_ || playing_ || capturing_ || !format.valid() || !ingress ||
            ingressSize < ingressBytes(format) ||
            !processed_.configure(format, recording, recordingSize))
            return false;
        format_ = format;
        ingress_ = ingress;
        clear();
        return true;
    }
    static unsigned recordingBytes(AudioFormat format) noexcept {
        return format.ringFrames() * format.channels * SampleBytes;
    }
    unsigned frameBytes() const noexcept {
        return format_.channels * SampleBytes;
    }
    AudioFormat format() const noexcept {
        return format_;
    }
    unsigned long long connect(unsigned long long now) noexcept {
        if (alive(now))
            return 0; // another live engine owns this adapter
        clear();
        if (++generation_ == 0)
            ++generation_;
        engine_ = true;
        lastContact_ = now;
        processed_.reset(true);
        processed_.capture(capturing_);
        return generation_;
    }
    bool disconnect(unsigned long long generation) noexcept {
        if (!engine_ || generation != generation_)
            return false;
        engine_ = false;
        clear();
        return true;
    }
    bool heartbeat(unsigned long long generation, unsigned long long now) noexcept {
        return contact(generation, now);
    }
    void playback(bool active) noexcept {
        playing_ = active;
        clearIngress();
    }
    void capture(bool active) noexcept {
        capturing_ = active;
        processed_.capture(active);
    }
    // Called by the render DMA path, with the first frame's presentation time.
    bool pushPlayback(const unsigned char* samples, unsigned frames, unsigned long long time,
                      unsigned long long now) noexcept {
        if (!samples || !frames || frames > packetFrames || !playing_ || !alive(now))
            return false;
        if constexpr (SampleBytes == 4) {
            const auto expectedTime =
                ingressOriginTime_ + FrameClock::timeForFrames(ingressFrame_, format_.sampleRate);
            const auto clockDifference =
                expectedTime > time ? expectedTime - time : time - expectedTime;
            if (!ingressClockValid_ || expectedTime < ingressOriginTime_ ||
                ingressFrame_ > ~0ULL - frames || clockDifference > 1) {
                ingressOriginTime_ = time;
                ingressFrame_ = 0;
                ingressClockValid_ = true;
            }
            const auto firstFrame = ingressFrame_;
            time = ingressOriginTime_ + FrameClock::timeForFrames(firstFrame, format_.sampleRate);
            ingressFrame_ += frames;
            // Position updates can contain very short fragments. Pack adjacent
            // audio into the existing tail slot so capacity measures audio,
            // rather than the frequency of those position queries. A partial
            // packet remains immediately readable: no extra buffering delay.
            if (count_) {
                const unsigned tail = (read_ + count_ - 1) % packetCapacity;
                auto& previous = packets_[tail];
                const auto expected =
                    previous.time + FrameClock::timeForFrames(previous.frames, format_.sampleRate);
                const auto difference = expected > time ? expected - time : time - expected;
                // Keep the original rational frame phase across packet splits.
                // Only the driver's existing one-tick quantization is removed;
                // a larger timestamp gap starts a new origin above.
                if (previous.frames < packetFrames && expected >= previous.time &&
                    difference <= 1) {
                    const unsigned append = frames < packetFrames - previous.frames
                                                ? frames
                                                : packetFrames - previous.frames;
                    copyBytes(ingress_ + (tail * packetFrames + previous.frames) * frameBytes(),
                              samples, append * frameBytes());
                    previous.frames += append;
                    if (append == frames)
                        return true;
                    samples += append * frameBytes();
                    frames -= append;
                    time = ingressOriginTime_ +
                           FrameClock::timeForFrames(firstFrame + append, format_.sampleRate);
                }
            }
        }
        if (count_ == packetCapacity) {
            read_ = (read_ + 1) % packetCapacity;
            --count_;
            discontinuity_ = true;
        }
        const unsigned slot = (read_ + count_) % packetCapacity;
        auto& packet = packets_[slot];
        packet = {};
        packet.time = time;
        packet.sequence = sequence_++;
        packet.frames = frames;
        copyBytes(ingress_ + slot * packetFrames * frameBytes(), samples, frames * frameBytes());
        ++count_;
        return true;
    }
    bool takePlayback(unsigned long long generation, unsigned long long now,
                      Packet& packet) noexcept {
        // Do not create a maximum-sized aggregate temporary on a kernel stack.
        packet.time = packet.sequence = 0;
        packet.frames = packet.discontinuity = 0;
        zeroBytes(packet.samples, static_cast<unsigned>(sizeof(packet.samples)));
        if (!contact(generation, now) || !count_)
            return false;
        packet.time = packets_[read_].time;
        packet.sequence = packets_[read_].sequence;
        packet.frames = packets_[read_].frames;
        copyBytes(packet.samples, ingress_ + read_ * packetFrames * frameBytes(),
                  packet.frames * frameBytes());
        packet.discontinuity = discontinuity_ ? 1 : 0;
        discontinuity_ = false;
        read_ = (read_ + 1) % packetCapacity;
        --count_;
        return true;
    }
    // Only processed engine audio may reach recording; ingress never does.
    bool pushProcessed(unsigned long long generation, unsigned long long now,
                       const unsigned char* samples, unsigned frames) noexcept {
        if (!samples || !frames || frames > packetFrames || !contact(generation, now))
            return false;
        processed_.write(samples, frames);
        return true;
    }
    void readRecording(unsigned char* samples, unsigned frames, unsigned long long now) noexcept {
        recordingFrames_ += frames;
        if (alive(now)) {
            const auto underruns = processed_.underruns();
            const auto queued = processed_.queued();
            processed_.read(samples, frames);
            if (processed_.underruns() != underruns) {
                lastUnderrunTime_ = now;
                lastUnderrunQueued_ = queued;
                lastUnderrunRequested_ = frames;
            }
        } else {
            leaseSilentFrames_ += frames;
            zeroBytes(samples, frames * frameBytes());
        }
    }

  private:
    bool alive(unsigned long long now) noexcept {
        if (engine_ && (now < lastContact_ || now - lastContact_ > leaseDuration)) {
            engine_ = false;
            clear();
        }
        return engine_;
    }
    bool contact(unsigned long long generation, unsigned long long now) noexcept {
        if (!alive(now) || generation != generation_)
            return false;
        lastContact_ = now;
        return true;
    }
    void clearIngress() noexcept {
        read_ = count_ = 0;
        discontinuity_ = true;
        ingressClockValid_ = false;
        ingressOriginTime_ = ingressFrame_ = 0;
    }
    void clear() noexcept {
        clearIngress();
        processed_.reset(false);
    }
    struct Slot {
        unsigned long long time = 0, sequence = 0;
        unsigned frames = 0;
    };
    Slot packets_[packetCapacity]{};
    unsigned char fallbackIngress_[packetCapacity * packetFrames * 2 * SampleBytes]{};
    unsigned char* ingress_ = fallbackIngress_;
    AudioFormat format_{};
    SampleBridgeRing<SampleBytes> processed_;
    unsigned read_ = 0, count_ = 0;
    unsigned long long generation_ = 0, sequence_ = 0, lastContact_ = 0;
    unsigned long long ingressOriginTime_ = 0, ingressFrame_ = 0;
    unsigned long long lastUnderrunTime_ = 0, recordingFrames_ = 0, leaseSilentFrames_ = 0;
    unsigned lastUnderrunQueued_ = 0, lastUnderrunRequested_ = 0;
    bool engine_ = false, playing_ = false, capturing_ = false, discontinuity_ = true;
    bool ingressClockValid_ = false;
};
using DuplexBridge = SampleDuplexBridge<2>;
using FloatDuplexBridge = SampleDuplexBridge<4>;
} // namespace loopz_driver
