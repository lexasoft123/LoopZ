#include <loopz/macos/capture_sources.hpp>
#include <loopz/macos/capture.hpp>
#include <loopz/macos/application_routes.hpp>
#include <loopz/capture_mix.hpp>
#include <loopz/realtime_publication.hpp>
#include <loopz/macos/permissions.hpp>
#include <loopz/audio_history.hpp>
#include <zcore/device/audio_input_callback_gate.h>
#include <thread>

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace loopz::macos {
namespace {
void silence(RoutingOutput output, uint32_t frames) noexcept {
    for (auto* plane : output.channels)
        if (plane)
            std::fill_n(plane, std::min(frames, output.capacityFrames), 0);
}
bool ownedUID(const std::string& uid) {
    return uid == kDeviceUID || uid.starts_with("com.lexasoft.loopz.device.");
}
AudioObjectPropertyAddress
property(AudioObjectPropertySelector selector,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template <class T> bool get(AudioObjectID id, AudioObjectPropertySelector selector, T& value) {
    auto address = property(selector);
    UInt32 bytes = sizeof(value);
    return AudioObjectGetPropertyData(id, &address, 0, nullptr, &bytes, &value) == noErr &&
           bytes == sizeof(value);
}
AudioDeviceID findInput(const std::string& selector) {
    AudioDeviceID device = 0;
    if (selector == "default") {
        get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, device);
        return device;
    }
    auto* uid = CFStringCreateWithCString(nullptr, selector.c_str(), kCFStringEncodingUTF8);
    if (!uid)
        return 0;
    UInt32 bytes = sizeof(device);
    const auto address = property(kAudioHardwarePropertyTranslateUIDToDevice);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(uid), &uid, &bytes,
                                   &device) != noErr ||
        bytes != sizeof(device))
        device = 0;
    CFRelease(uid);
    return device;
}
bool ownedDevice(AudioDeviceID device) {
    CFStringRef uid = nullptr;
    if (!get(device, kAudioDevicePropertyDeviceUID, uid) || !uid)
        return true;
    std::array<char, 4097> text{};
    const bool copied = CFStringGetCString(uid, text.data(), text.size(), kCFStringEncodingUTF8);
    CFRelease(uid);
    return !copied || ownedUID(text.data());
}
uint32_t inputChannels(AudioDeviceID device) {
    const auto address =
        property(kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &bytes) != noErr ||
        bytes < offsetof(AudioBufferList, mBuffers) || bytes > 1024 * 1024)
        return 0;
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const auto capacity = bytes;
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, storage.data()) != noErr ||
        bytes > capacity || bytes < offsetof(AudioBufferList, mBuffers))
        return 0;
    const auto* list = reinterpret_cast<const AudioBufferList*>(storage.data());
    if (list->mNumberBuffers > (bytes - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer))
        return 0;
    uint64_t count = 0;
    for (UInt32 index = 0; index < list->mNumberBuffers; ++index)
        count += list->mBuffers[index].mNumberChannels;
    return count <= 4096 ? static_cast<uint32_t>(count) : 0;
}
class ApplicationSource final : public SourceEndpoint {
    struct Group {
        std::vector<ApplicationCaptureRoute> routes;
        std::vector<std::unique_ptr<Capture>> owners;
        std::vector<const CaptureBuffer*> histories;
        // Clock-anchor caches only, owned by the scene's one serial renderer.
        // They reset with this publication and never consume audio or carry a
        // resampling phase from one logical route into another.
        CaptureMixReader reader;
        void stop() noexcept {
            for (const auto& capture : owners)
                capture->stop();
        }
    };

  public:
    explicit ApplicationSource(std::string bundle, CaptureSourceOptions options)
        : bundle_(std::move(bundle)), options_(options) {}
    ~ApplicationSource() override {
        close();
    }
    uint32_t channels() const noexcept override {
        return 2;
    }
    bool open(SourceOpenPolicy policy, std::string& error) override {
        close();
        if (!policy.requiredChannels || policy.requiredChannels > 2) {
            error = "Application capture is stereo";
            return false;
        }
        groups_.checkStageCapacity();
        ApplicationAudioSelection selected;
        if (!selectApplicationAudio(bundle_, selected, error))
            return false;
        if (selected.processes.empty() || !selected.producing || selected.outputs.empty()) {
            error = "Application has no running output; waiting for audio";
            return false;
        }
        auto next = std::make_unique<Group>();
        next->routes = captureRoutes(selected, error);
        if (next->routes.empty())
            return false;
        next->owners.reserve(next->routes.size());
        next->histories.reserve(next->routes.size());
        for (const auto& route : next->routes) {
            auto capture = std::make_unique<Capture>();
            ApplicationCaptureConfig config;
            config.requestedBufferFrames = options_.applicationBufferFrames;
            config.deliveryDiagnostics = options_.deliveryDiagnostics;
            config.requireDeviceStream = true;
            config.deviceUID = route.deviceUID;
            config.streamIndex = route.streamIndex;
            if (!capture->startApplication(bundle_, policy.muteOriginal, error, config))
                return false;
            next->histories.push_back(&capture->app());
            next->owners.push_back(std::move(capture));
        }
        // Validate the shared process snapshot against every newly opened tap.
        // A helper changing identity during acquisition causes a whole retry.
        for (const auto& capture : next->owners)
            if (!capture->healthy(selected, error))
                return false;
        auto* nextControl = next.get();
        auto update = groups_.stage(std::move(next));
        update->commit();
        current_ = nextControl;
        muteOriginal_ = policy.muteOriginal;
        active_.store(true);
        return true;
    }
    bool updatePolicy(SourceOpenPolicy policy) noexcept override {
        if (!active_.load() || !current_ || !policy.requiredChannels || policy.requiredChannels > 2)
            return false;
        if (policy.muteOriginal == muteOriginal_)
            return true;
        size_t changed = 0;
        for (const auto& capture : current_->owners) {
            if (!capture->setApplicationMute(policy.muteOriginal)) {
                for (size_t index = 0; index < changed; ++index)
                    (void)current_->owners[index]->setApplicationMute(muteOriginal_);
                return false; // The registry closes the entire group before reacquiring.
            }
            ++changed;
        }
        muteOriginal_ = policy.muteOriginal;
        return true;
    }
    void close() noexcept override {
        active_.store(false);
        if (current_)
            current_->stop();
        groups_.collect();
    }
    bool healthy(std::string& error) override {
        groups_.collect();
        if (!active_.load() || !current_)
            return false;
        ApplicationAudioSelection selected;
        if (!selectApplicationAudio(bundle_, selected, error))
            return false;
        if (selected.producing || !selected.outputs.empty()) {
            if (captureRoutes(selected, error) != current_->routes) {
                if (error.empty())
                    error = "Application output devices or streams changed";
                return false;
            }
        }
        for (const auto& capture : current_->owners)
            if (!capture->healthy(selected, error))
                return false;
        return true;
    }
    AudioReadResult read(uint64_t time, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        if (!active_.load() || !frames || frames > kBlock || output.capacityFrames < frames ||
            output.channels.empty() || output.channels.size() > 2 ||
            std::any_of(output.channels.begin(), output.channels.end(), [](auto* plane) {
                return !plane;
            })) {
            silence(output, frames);
            return {frames, false};
        }
        auto group = groups_.borrow();
        if (!group) {
            silence(output, frames);
            return {frames, false};
        }
        std::array<float, kBlock> right{};
        auto* outputRight = output.channels.size() == 2 ? output.channels[1] : right.data();
        const auto missing = sampleCaptureMix(group->histories, time, rate, output.channels[0],
                                              outputRight, frames, group->reader);
        return {missing, active_.load()};
    }
    std::vector<InputTimingFacts> inputTiming() const override {
        if (!active_.load() || !current_ || current_->owners.empty())
            return cachedTiming_;
        std::vector<InputTimingFacts> result;
        result.reserve(current_->owners.size());
        for (const auto& capture : current_->owners) {
            const auto facts = capture->applicationInputTiming();
            if (!facts) {
                // Reuse delivery history only for the same routes and an actual
                // reopened private format no more demanding than the verified one.
                if (current_->routes != cachedTimingRoutes_ ||
                    current_->owners.size() != cachedTiming_.size())
                    return {};
                for (size_t index = 0; index < current_->owners.size(); ++index) {
                    const auto actual = current_->owners[index]->applicationInputTiming(false);
                    const auto& cached = cachedTiming_[index];
                    if (!actual || actual->sampleRate != cached.sampleRate ||
                        actual->bufferFrames > cached.bufferFrames ||
                        actual->latencyFrames > cached.latencyFrames)
                        return {};
                }
                return cachedTiming_;
            }
            result.push_back(*facts);
        }
        cachedTiming_ = result;
        cachedTimingRoutes_ = current_->routes;
        return result;
    }
    void clearInputTiming() noexcept override {
        cachedTiming_.clear();
        cachedTimingRoutes_.clear();
    }
    std::vector<InputTimingFacts> retainedInputTiming() const override {
        return cachedTiming_;
    }

  private:
    bool muteOriginal_{false};
    const std::string bundle_;
    const CaptureSourceOptions options_;
    mutable RealtimePublication<Group> groups_{2};
    Group* current_{nullptr}; // Control owner only; reader-visible membership is immutable.
    std::atomic<bool> active_{false};
    mutable std::vector<InputTimingFacts> cachedTiming_;
    mutable std::vector<ApplicationCaptureRoute> cachedTimingRoutes_;
};
class InputSource final : public SourceEndpoint {
  public:
    InputSource(std::string selector, uint32_t channels, CaptureSourceOptions options)
        : selector_(std::move(selector)),
          history_(channels, AudioHistory::kDefaultCapacityFrames, options.inputFilter,
                   options.deliveryDiagnostics),
          deliveryEnabled_(options.deliveryDiagnostics) {}
    ~InputSource() override {
        close();
    }
    uint32_t channels() const noexcept override {
        return history_.channels();
    }
    bool complete() const noexcept override {
        return available_ >= required_ && required_ != 0 && unit_;
    }
    bool updatePolicy(SourceOpenPolicy policy) noexcept override {
        if (!unit_ || !policy.requiredChannels || policy.requiredChannels > channels() ||
            policy.muteOriginal)
            return false;
        // open() already captures min(native, prepared logical) channels. A
        // second reader changing its demand must not stop this shared AUHAL
        // stream, reset its history or interrupt an existing narrower reader.
        required_ = policy.requiredChannels;
        return true;
    }
    bool open(SourceOpenPolicy policy, std::string& error) override {
        close();
        delivery_.reset();
        deliveryEndNs_.store(UINT64_MAX);
        required_ = policy.requiredChannels;
        if (!required_ || required_ > channels()) {
            error = "Invalid input channel request";
            return false;
        }
        if (microphonePermission() != MicrophonePermission::Authorized) {
            error = "Microphone permission is required; use LoopZ permission setup";
            return false;
        }
        device_ = findInput(selector_);
        if (!device_ || ownedDevice(device_)) {
            error = "Input device is unavailable or is a LoopZ routing endpoint";
            return false;
        }
        const auto nativeChannels = inputChannels(device_);
        UInt32 alive = 0;
        if (!nativeChannels || !get(device_, kAudioDevicePropertyDeviceIsAlive, alive) || !alive ||
            !get(device_, kAudioDevicePropertyNominalSampleRate, rate_) || !std::isfinite(rate_) ||
            rate_ < 8000 || rate_ > 192000) {
            error = "Input device format is unavailable";
            return false;
        }
        physicalChannels_ = nativeChannels;
        available_ = std::min(nativeChannels, channels());
        AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                              kAudioUnitManufacturer_Apple, 0, 0};
        const auto component = AudioComponentFindNext(nullptr, &description);
        OSStatus result = component ? AudioComponentInstanceNew(component, &unit_)
                                    : kAudioHardwareUnspecifiedError;
        const auto fail = [&](OSStatus status) {
            error =
                "Input capture could not start (Core Audio status " + std::to_string(status) + ")";
            close();
            return false;
        };
        if (result)
            return fail(result);
        UInt32 yes = 1, no = 0;
        result = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Input, 1, &yes, sizeof(yes));
        if (!result)
            result = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO,
                                          kAudioUnitScope_Output, 0, &no, sizeof(no));
        if (!result)
            result = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                                          kAudioUnitScope_Global, 0, &device_, sizeof(device_));
        AudioStreamBasicDescription format{
            rate_,
            kAudioFormatLinearPCM,
            static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) |
                static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved),
            4,
            1,
            4,
            available_,
            32,
            0};
        if (!result)
            result = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
                                          kAudioUnitScope_Output, 1, &format, sizeof(format));
        std::array<SInt32, 64> map{};
        for (unsigned channel = 0; channel < available_; ++channel)
            map[channel] = channel;
        if (!result)
            result = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_ChannelMap,
                                          kAudioUnitScope_Output, 1, map.data(),
                                          available_ * sizeof(SInt32));
        maximum_ = 4096;
        UInt32 bytes = sizeof(maximum_);
        if (!result) {
            const auto status =
                AudioUnitGetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice,
                                     kAudioUnitScope_Global, 0, &maximum_, &bytes);
            if (status || bytes != sizeof(maximum_) || !maximum_)
                maximum_ = 4096;
            if (maximum_ > 16384)
                return fail(kAudioHardwareUnsupportedOperationError);
            samples_.resize(static_cast<size_t>(available_) * maximum_);
            list_.count = available_;
            for (unsigned channel = 0; channel < available_; ++channel) {
                auto* plane = samples_.data() + channel * maximum_;
                list_.buffers[channel] = {1, static_cast<UInt32>(maximum_ * sizeof(float)), plane};
                planes_[channel] = plane;
            }
        }
        AURenderCallbackStruct callback{InputSource::capture, this};
        if (!result)
            result = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_SetInputCallback,
                                          kAudioUnitScope_Global, 0, &callback, sizeof(callback));
        if (!result)
            result = AudioUnitInitialize(unit_);
        if (result)
            return fail(result);
        callbackError_.store(0);
        openedAt_ = hostNowNs();
        nextDeviceInspectionNs_ = 0;
        callbackGate_.open();
        result = AudioOutputUnitStart(unit_);
        return result ? fail(result) : true;
    }
    void close() noexcept override {
        if (deliveryEnabled_)
            finishDelivery();
        callbackGate_.beginClose();
        if (unit_) {
            AudioOutputUnitStop(unit_);
            while (callbackGate_.inFlight())
                std::this_thread::yield();
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
        history_.stop();
        available_ = 0;
    }
    void invalidateHealth() noexcept override {
        nextDeviceInspectionNs_ = 0;
    }
    bool healthy(std::string& error) override {
        const auto current = history_.status();
        const auto last = std::max(current.lastHostNs, openedAt_);
        const auto now = hostNowNs();
        // Callback failures, stalled capture and permission changes remain
        // checked on every control poll. Device topology/format queries involve
        // synchronous HAL IPC; cache only those, with an event-driven bypass.
        if (!unit_ || microphonePermission() != MicrophonePermission::Authorized ||
            callbackError_.load() || (now >= last && now - last > 2000000000ULL)) {
            error = "Input permission or callback stream changed";
            return false;
        }
        if (now < nextDeviceInspectionNs_)
            return true;
        UInt32 alive = 0;
        double rate = 0;
        if (findInput(selector_) != device_ ||
            !get(device_, kAudioDevicePropertyDeviceIsAlive, alive) || !alive ||
            !get(device_, kAudioDevicePropertyNominalSampleRate, rate) || rate != rate_ ||
            inputChannels(device_) != physicalChannels_) {
            error = "Input device, channel layout or rate changed";
            return false;
        }
        nextDeviceInspectionNs_ = now + 250000000ULL;
        return true;
    }
    AudioReadResult read(uint64_t first, double rate, RoutingOutput output, uint32_t frames,
                         AudioReadCursor& cursor) const noexcept override {
        return history_.read(first, rate, output, frames, cursor);
    }
    AudioReadResult readWithFilter(uint64_t first, double rate, RoutingOutput output,
                                   uint32_t frames, AudioReadCursor& cursor,
                                   ResamplingFilter filter) const noexcept override {
        return history_.readWithFilter(first, rate, output, frames, cursor, filter);
    }

    std::optional<AudioDeliverySnapshot> stoppedDelivery() const {
        if (unit_ || !deliveryEnabled_)
            return {};
        return delivery_.snapshot();
    }
    void finishDelivery() noexcept {
        deliveryEndNs_.store(std::min(deliveryEndNs_.load(), hostNowNs()));
    }
    std::optional<AudioHistoryReadSnapshot> readDiagnostics() const {
        return deliveryEnabled_ ? std::optional(history_.readDiagnostics()) : std::nullopt;
    }
    std::shared_ptr<const SourceDiagnostics> diagnostics() const override {
        if (!deliveryEnabled_)
            return {};
        auto result = std::make_shared<SourceDiagnostics>();
        result->history = history_.readDiagnostics();
        if (!unit_)
            result->stoppedDelivery = delivery_.snapshot();
        return result;
    }

  private:
    struct InputList {
        UInt32 count;
        AudioBuffer buffers[64];
    } list_{};
    static_assert(offsetof(InputList, buffers) == offsetof(AudioBufferList, mBuffers));
    static OSStatus capture(void* context, AudioUnitRenderActionFlags* flags,
                            const AudioTimeStamp* time, UInt32 bus, UInt32 frames,
                            AudioBufferList*) {
        auto& self = *static_cast<InputSource*>(context);
        singz::AudioInputCallbackScope callback(self.callbackGate_);
        if (!callback)
            return noErr;
        const auto enteredNs = self.deliveryEnabled_ ? hostNowNs() : 0;
        if (!time || !frames || frames > self.maximum_ ||
            !(time->mFlags & kAudioTimeStampHostTimeValid)) {
            self.callbackError_.store(kAudioHardwareBadPropertySizeError);
            return noErr;
        }
        for (unsigned channel = 0; channel < self.available_; ++channel)
            self.list_.buffers[channel].mDataByteSize = frames * sizeof(float);
        const auto result = AudioUnitRender(self.unit_, flags, time, bus, frames,
                                            reinterpret_cast<AudioBufferList*>(&self.list_));
        if (result)
            self.callbackError_.store(result);
        else if (!self.history_.append({{self.planes_.data(), self.available_}, self.maximum_},
                                       frames, ticksToNs(time->mHostTime), self.rate_,
                                       time->mSampleTime,
                                       time->mFlags & kAudioTimeStampSampleTimeValid))
            self.callbackError_.store(kAudioHardwareUnspecifiedError);
        if (!result && self.deliveryEnabled_ && enteredNs < self.deliveryEndNs_.load())
            self.delivery_.observe(ticksToNs(time->mHostTime), frames, self.rate_, enteredNs,
                                   hostNowNs(), time->mSampleTime,
                                   time->mFlags & kAudioTimeStampSampleTimeValid);
        return result;
    }
    const std::string selector_;
    AudioHistory history_;
    AudioUnit unit_{nullptr};
    AudioDeviceID device_{0};
    uint32_t required_{0}, available_{0}, physicalChannels_{0}, maximum_{4096};
    double rate_{0};
    uint64_t openedAt_{0}, nextDeviceInspectionNs_{0};
    std::vector<float> samples_;
    std::array<const float*, 64> planes_{};
    std::atomic<OSStatus> callbackError_{0};
    singz::AudioInputCallbackGate callbackGate_;
    const bool deliveryEnabled_;
    AudioDeliveryTiming delivery_;
    std::atomic<uint64_t> deliveryEndNs_{UINT64_MAX};
};
} // namespace
std::shared_ptr<SourceEndpoint> createCaptureSource(SourceKind kind, const std::string& selector,
                                                    uint32_t channels) {
    return captureSourceFactory()(kind, selector, channels);
}
SourceRegistry::Factory captureSourceFactory(CaptureSourceOptions options) {
    if (options.applicationBufferFrames != 32 && options.applicationBufferFrames != 64 &&
        options.applicationBufferFrames != 128)
        throw std::invalid_argument("Application capture buffer must be 32, 64 or 128 frames");
    if (options.inputFilter != ResamplingFilter::LinearPhase &&
        options.inputFilter != ResamplingFilter::MinimumPhase)
        throw std::invalid_argument("Unsupported input resampling filter");
    return [options](SourceKind kind, const std::string& selector,
                     uint32_t channels) -> std::shared_ptr<SourceEndpoint> {
        if (selector.empty() || selector.size() > 4096 ||
            selector.find('\0') != std::string::npos || !channels || channels > 64)
            throw std::invalid_argument("Invalid capture source identity or channel count");
        if (kind == SourceKind::Application) {
            if (channels != 2 || selector == "com.lexasoft.loopz.engine")
                throw std::invalid_argument("Invalid or recursive application capture");
            return std::make_shared<ApplicationSource>(selector, options);
        }
        if (kind != SourceKind::InputDevice || ownedUID(selector))
            throw std::invalid_argument("Use a virtual source for LoopZ device routing");
        return std::make_shared<InputSource>(selector, channels, options);
    };
}
std::optional<AudioDeliverySnapshot> stoppedInputDelivery(const SourceEndpoint& source) {
    const auto* input = dynamic_cast<const InputSource*>(&source);
    return input ? input->stoppedDelivery() : std::nullopt;
}
std::optional<AudioHistoryReadSnapshot> inputHistoryReadDiagnostics(const SourceEndpoint& source) {
    const auto* input = dynamic_cast<const InputSource*>(&source);
    return input ? input->readDiagnostics() : std::nullopt;
}
void finishInputDelivery(SourceEndpoint& source) noexcept {
    if (auto* input = dynamic_cast<InputSource*>(&source))
        input->finishDelivery();
}
} // namespace loopz::macos
