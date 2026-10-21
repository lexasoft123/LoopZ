#pragma once

// Diagnostic-only comparison with the production AUHAL input adapter. Reads
// native Float32 device buffers without an AudioUnitRender pull. It never
// writes hardware formats, periods, output samples or public routing devices.
#include <loopz/audio_history.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/macos/capture_sources.hpp>
#include <loopz/macos/permissions.hpp>
#include <zcore/device/audio_input_callback_gate.h>
#include <CoreAudio/CoreAudio.h>
#include <array>
#include <cmath>
#include <thread>

namespace loopz::macos {
class HalInputExperiment final : public SourceEndpoint {
  public:
    HalInputExperiment(std::string uid, uint32_t channels)
        : uid_(std::move(uid)), history_(channels) {}
    ~HalInputExperiment() override {
        close();
    }
    uint32_t channels() const noexcept override {
        return history_.channels();
    }
    bool complete() const noexcept override {
        return proc_ && available_ >= required_;
    }
    bool updatePolicy(SourceOpenPolicy policy) noexcept override {
        if (!proc_ || !policy.requiredChannels || policy.requiredChannels > channels() ||
            policy.muteOriginal)
            return false;
        required_ = policy.requiredChannels;
        return true;
    }
    bool open(SourceOpenPolicy policy, std::string& error) override {
        close();
        delivery_.reset();
        deliveryEndNs_.store(UINT64_MAX);
        error.clear();
        required_ = policy.requiredChannels;
        if (microphonePermission() != MicrophonePermission::Authorized || !required_ ||
            required_ > channels() || policy.muteOriginal) {
            error = "Authorized physical input is required";
            return false;
        }
        CFStringRef uid = CFStringCreateWithCString(nullptr, uid_.c_str(), kCFStringEncodingUTF8);
        const AudioObjectPropertyAddress translate{kAudioHardwarePropertyTranslateUIDToDevice,
                                                   kAudioObjectPropertyScopeGlobal, 0};
        UInt32 bytes = sizeof(device_);
        auto status = uid ? AudioObjectGetPropertyData(kAudioObjectSystemObject, &translate,
                                                       sizeof(uid), &uid, &bytes, &device_)
                          : kAudioHardwareBadDeviceError;
        if (uid)
            CFRelease(uid);
        if (status || !device_ || !get(kAudioDevicePropertyNominalSampleRate, rate_) ||
            !std::isfinite(rate_) || rate_ < 8000 || rate_ > 192000) {
            error = "Physical input is unavailable";
            return false;
        }
        // Verify the native stream topology before interpreting IOProc bytes.
        const AudioObjectPropertyAddress address{kAudioDevicePropertyStreams,
                                                 kAudioDevicePropertyScopeInput, 0};
        bytes = 0;
        if (AudioObjectGetPropertyDataSize(device_, &address, 0, nullptr, &bytes) || !bytes ||
            bytes % sizeof(AudioStreamID) || bytes > 64 * sizeof(AudioStreamID)) {
            error = "Unsupported input stream topology";
            return false;
        }
        std::array<AudioStreamID, 64> streams{};
        if (AudioObjectGetPropertyData(device_, &address, 0, nullptr, &bytes, streams.data()))
            return false;
        nativeChannels_ = 0;
        for (uint32_t i = 0; i < bytes / sizeof(AudioStreamID); ++i) {
            const AudioObjectPropertyAddress formatAddress{kAudioStreamPropertyVirtualFormat,
                                                           kAudioObjectPropertyScopeGlobal, 0};
            AudioStreamBasicDescription format{};
            UInt32 formatBytes = sizeof(format);
            if (AudioObjectGetPropertyData(streams[i], &formatAddress, 0, nullptr, &formatBytes,
                                           &format) ||
                formatBytes != sizeof(format) || format.mSampleRate != rate_ ||
                format.mFormatID != kAudioFormatLinearPCM ||
                !(format.mFormatFlags & kAudioFormatFlagIsFloat) ||
                !(format.mFormatFlags & kAudioFormatFlagIsPacked) ||
                (format.mFormatFlags & kAudioFormatFlagIsBigEndian) ||
                format.mBitsPerChannel != 32 || !format.mChannelsPerFrame ||
                format.mFramesPerPacket != 1 ||
                format.mBytesPerFrame !=
                    sizeof(float) * ((format.mFormatFlags & kAudioFormatFlagIsNonInterleaved)
                                         ? 1
                                         : format.mChannelsPerFrame)) {
                error = "The diagnostic requires native packed Float32 input";
                return false;
            }
            nativeChannels_ += format.mChannelsPerFrame;
        }
        if (!nativeChannels_ || nativeChannels_ > 64) {
            error = "Unsupported physical input channel count";
            return false;
        }
        available_ = std::min(channels(), nativeChannels_);
        samples_.resize(static_cast<size_t>(available_) * maximumFrames_);
        for (uint32_t c = 0; c < available_; ++c)
            planes_[c] = samples_.data() + c * maximumFrames_;
        error_.store(0);
        openedNs_ = hostNowNs();
        status = AudioDeviceCreateIOProcID(device_, capture, this, &proc_);
        if (!status) {
            gate_.open();
            status = AudioDeviceStart(device_, proc_);
        }
        if (!status)
            return true;
        error = "Direct HAL capture failed (status " + std::to_string(status) + ")";
        close();
        return false;
    }
    void close() noexcept override {
        gate_.beginClose();
        if (proc_) {
            AudioDeviceStop(device_, proc_);
            while (gate_.inFlight())
                std::this_thread::yield();
            AudioDeviceDestroyIOProcID(device_, proc_);
            proc_ = nullptr;
        }
        history_.stop();
        available_ = 0;
    }
    bool healthy(std::string& error) override {
        UInt32 alive = 0;
        double rate = 0;
        const auto last = std::max(openedNs_, history_.status().lastHostNs);
        const auto now = hostNowNs();
        if (!proc_ || error_.load() || microphonePermission() != MicrophonePermission::Authorized ||
            !get(kAudioDevicePropertyDeviceIsAlive, alive) || !alive ||
            !get(kAudioDevicePropertyNominalSampleRate, rate) || rate != rate_ ||
            (now > last && now - last > 2000000000ULL)) {
            error = "Direct HAL capture is unavailable";
            return false;
        }
        return true;
    }
    AudioReadResult read(uint64_t first, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override {
        return history_.read(first, rate, output, frames, cursor);
    }
    std::optional<AudioDeliverySnapshot> stoppedDelivery() const {
        return proc_ ? std::nullopt : std::optional{delivery_.snapshot()};
    }
    void finishDelivery() noexcept {
        deliveryEndNs_.store(hostNowNs());
    }

  private:
    template <class T> bool get(AudioObjectPropertySelector selector, T& value) const {
        const AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, 0};
        UInt32 bytes = sizeof(value);
        return !AudioObjectGetPropertyData(device_, &address, 0, nullptr, &bytes, &value) &&
               bytes == sizeof(value);
    }
    static OSStatus capture(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* input,
                            const AudioTimeStamp* time, AudioBufferList*, const AudioTimeStamp*,
                            void* context) {
        auto& self = *static_cast<HalInputExperiment*>(context);
        singz::AudioInputCallbackScope scope(self.gate_);
        if (!scope)
            return noErr;
        const auto entered = hostNowNs();
        if (!input || !input->mNumberBuffers || input->mNumberBuffers > 64 || !time ||
            !(time->mFlags & kAudioTimeStampHostTimeValid) || !time->mHostTime) {
            self.error_.store(1);
            return noErr;
        }
        uint32_t frames = 0, channels = 0;
        for (uint32_t b = 0; b < input->mNumberBuffers; ++b) {
            const auto& buffer = input->mBuffers[b];
            if (!buffer.mData || !buffer.mNumberChannels || buffer.mNumberChannels > 64 ||
                buffer.mDataByteSize % (sizeof(float) * buffer.mNumberChannels)) {
                self.error_.store(2);
                return noErr;
            }
            const auto count = buffer.mDataByteSize / (sizeof(float) * buffer.mNumberChannels);
            if (!count || count > maximumFrames_ || (frames && frames != count)) {
                self.error_.store(3);
                return noErr;
            }
            frames = static_cast<uint32_t>(count);
            const auto* source = static_cast<const float*>(buffer.mData);
            for (uint32_t c = 0; c < buffer.mNumberChannels && channels + c < self.available_; ++c)
                for (uint32_t f = 0; f < frames; ++f)
                    self.samples_[(channels + c) * maximumFrames_ + f] =
                        source[f * buffer.mNumberChannels + c];
            channels += buffer.mNumberChannels;
        }
        if (channels != self.nativeChannels_ ||
            !self.history_.append({{self.planes_.data(), self.available_}, maximumFrames_}, frames,
                                  ticksToNs(time->mHostTime), self.rate_, time->mSampleTime,
                                  time->mFlags & kAudioTimeStampSampleTimeValid))
            self.error_.store(4);
        if (entered < self.deliveryEndNs_.load())
            self.delivery_.observe(ticksToNs(time->mHostTime), frames, self.rate_, entered,
                                   hostNowNs(), time->mSampleTime,
                                   time->mFlags & kAudioTimeStampSampleTimeValid);
        return noErr;
    }
    static constexpr uint32_t maximumFrames_ = 16384;
    const std::string uid_;
    AudioHistory history_;
    AudioDeviceID device_{};
    AudioDeviceIOProcID proc_{};
    uint32_t required_{}, available_{}, nativeChannels_{};
    double rate_{};
    uint64_t openedNs_{};
    std::vector<float> samples_;
    std::array<const float*, 64> planes_{};
    singz::AudioInputCallbackGate gate_;
    std::atomic<uint32_t> error_{};
    AudioDeliveryTiming delivery_;
    std::atomic<uint64_t> deliveryEndNs_{UINT64_MAX};
};
} // namespace loopz::macos
