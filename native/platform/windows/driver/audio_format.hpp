// LoopZ contributors. MIT license. Shared, allocation-free kernel/user contract.
#pragma once
namespace loopz_driver {
struct AudioFormat {
    unsigned sampleRate = 48000;
    unsigned channels = 2;
    static constexpr unsigned maxChannels = 64;
    static constexpr unsigned bytesPerSample = 2;
    bool valid() const noexcept {
        const bool rate = sampleRate == 44100 || sampleRate == 48000 || sampleRate == 88200 ||
                          sampleRate == 96000 || sampleRate == 176400 || sampleRate == 192000;
        return rate && channels >= 2 && channels <= maxChannels && !(channels & 1);
    }
    unsigned frameBytes() const noexcept {
        return channels * bytesPerSample;
    }
    unsigned ringFrames() const noexcept {
        return sampleRate / 5;
    } // 200 ms
    unsigned prefillFrames() const noexcept {
        return sampleRate / 50;
    } // 20 ms
    unsigned ringBytes() const noexcept {
        return ringFrames() * frameBytes();
    }
    bool acceptsAllocatedBuffer(unsigned requested, unsigned allocated,
                                unsigned sampleBytes = 2) const noexcept {
        return valid() && (sampleBytes == 2 || sampleBytes == 4) && requested &&
               requested <= ringFrames() * channels * sampleBytes && allocated >= requested;
    }
    bool acceptsNotificationBuffer(unsigned bytes, unsigned notifications,
                                   unsigned sampleBytes = 2) const noexcept {
        // Bound memory by duration, not by the old stereo/48 kHz byte count.
        // Every notification must cover whole frames, including odd-rate periods.
        return valid() && (sampleBytes == 2 || sampleBytes == 4) && bytes && notifications &&
               bytes <= ringFrames() * channels * sampleBytes && bytes % notifications == 0 &&
               (bytes / notifications) % (channels * sampleBytes) == 0;
    }
};
// One registry value atomically describes an adapter's nominal public format.
// Only a privileged setup process writes it, before binding/restarting that root.
struct AudioFormatRecord {
    unsigned version = 1;
    unsigned reserved = 0;
    AudioFormat format{};
    bool valid() const noexcept {
        return version == 1 && !reserved && format.valid();
    }
};
static_assert(sizeof(AudioFormat) == 8 && sizeof(AudioFormatRecord) == 16);
} // namespace loopz_driver
