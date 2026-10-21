#include <loopz/sample_rates.hpp>
#include "state.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>

namespace loopz::macos::hal {
namespace {
bool global(const AudioObjectPropertyAddress& address) {
    return address.mScope == kAudioObjectPropertyScopeGlobal;
}
bool scoped(const AudioObjectPropertyAddress& address) {
    return global(address) || address.mScope == kAudioObjectPropertyScopeInput ||
           address.mScope == kAudioObjectPropertyScopeOutput;
}
std::array<AudioObjectID, 2> streams(const Device& device,
                                     const AudioObjectPropertyAddress& address, size_t& count) {
    std::array<AudioObjectID, 2> result{};
    count = 0;
    if (global(address) || address.mScope == kAudioObjectPropertyScopeInput)
        result[count++] = device.input;
    if (device.hasOutput && (global(address) || address.mScope == kAudioObjectPropertyScopeOutput))
        result[count++] = device.output;
    return result;
}

OSStatus propertySize(const Device* device, AudioObjectID object,
                      const AudioObjectPropertyAddress& address, size_t deviceCount,
                      uint32_t sampleRateMask, UInt32& bytes) {
    bytes = 0;
    if (address.mElement != kAudioObjectPropertyElementMain || !scoped(address))
        return kAudioHardwareUnknownPropertyError;
    const bool plugin = object == kAudioObjectPlugInObject;
    const bool endpoint = device && object == device->object;
    switch (address.mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
        if (!global(address))
            break;
        bytes = sizeof(UInt32);
        return noErr;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
        if (!global(address))
            break;
        bytes = sizeof(CFStringRef);
        return noErr;
    case kAudioObjectPropertyOwnedObjects:
        if (plugin && global(address)) {
            bytes = deviceCount * sizeof(AudioObjectID);
            return noErr;
        }
        if (endpoint) {
            size_t count;
            (void)streams(*device, address, count);
            bytes = count * sizeof(AudioObjectID);
            return noErr;
        }
        if (device && global(address))
            return noErr;
        break;
    }
    if (plugin) {
        if (!global(address))
            return kAudioHardwareUnknownPropertyError;
        switch (address.mSelector) {
        case kAudioPlugInPropertyDeviceList:
            bytes = deviceCount * sizeof(AudioObjectID);
            return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice:
            bytes = sizeof(AudioObjectID);
            return noErr;
        case kAudioPlugInPropertyBundleID:
        case kAudioPlugInPropertyResourceBundle:
            bytes = sizeof(CFStringRef);
            return noErr;
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList:
            return noErr;
        }
    } else if (endpoint) {
        switch (address.mSelector) {
        case kAudioObjectPropertyCustomPropertyInfoList:
            if (global(address)) {
                bytes = 2 * sizeof(AudioServerPlugInCustomPropertyInfo);
                return noErr;
            }
            break;
        case kLoopZDevicePropertyIOTiming:
        case kLoopZDevicePropertyCallbackTiming:
            if (global(address)) {
                bytes = sizeof(CFPropertyListRef);
                return noErr;
            }
            break;
        case kAudioDevicePropertyStreams: {
            size_t count;
            (void)streams(*device, address, count);
            bytes = count * sizeof(AudioObjectID);
        }
            return noErr;
        case kAudioObjectPropertyControlList:
            return noErr;
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            if (!global(address)) {
                bytes = sizeof(UInt32);
                return noErr;
            }
            break;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            if (!global(address) &&
                (address.mScope == kAudioObjectPropertyScopeInput || device->hasOutput)) {
                bytes = 2 * sizeof(UInt32);
                return noErr;
            }
            break;
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            if (global(address)) {
                bytes = sizeof(CFStringRef);
                return noErr;
            }
            break;
        case kAudioDevicePropertyNominalSampleRate:
            if (global(address)) {
                bytes = sizeof(Float64);
                return noErr;
            }
            break;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            if (global(address)) {
                bytes = std::popcount(sampleRateMask |
                                      virtualSampleRateBit(device->runtime->audio.sampleRate())) *
                        sizeof(AudioValueRange);
                return noErr;
            }
            break;
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyClockIsStable:
        case kAudioDevicePropertyIsHidden:
            if (global(address)) {
                bytes = sizeof(UInt32);
                return noErr;
            }
            break;
        }
    } else if (device && global(address)) {
        switch (address.mSelector) {
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
            bytes = sizeof(UInt32);
            return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            bytes = sizeof(AudioStreamBasicDescription);
            return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            bytes = std::popcount(sampleRateMask |
                                  virtualSampleRateBit(device->runtime->audio.sampleRate())) *
                    sizeof(AudioStreamRangedDescription);
            return noErr;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

template <typename T> void scalar(void* destination, const T& value) {
    std::memcpy(destination, &value, sizeof(T));
}
void text(void* destination, CFStringRef value) {
    CFRetain(value);
    scalar(destination, value);
}
AudioStreamBasicDescription format(const Device& device) {
    const auto channels = device.runtime->audio.channels();
    const UInt32 frameBytes = channels * UInt32(sizeof(float));
    return {static_cast<double>(device.runtime->audio.sampleRate()),
            kAudioFormatLinearPCM,
            UInt32(kAudioFormatFlagIsFloat) | UInt32(kAudioFormatFlagsNativeEndian) |
                UInt32(kAudioFormatFlagIsPacked),
            frameBytes,
            1,
            frameBytes,
            channels,
            32,
            0};
}
} // namespace

bool State::has(AudioObjectID object, const AudioObjectPropertyAddress& address) {
    UInt32 bytes;
    return size(object, address, bytes) == noErr;
}
OSStatus State::settable(AudioObjectID object, const AudioObjectPropertyAddress& address,
                         Boolean& result) {
    std::lock_guard guard(mutex_);
    result = false;
    auto* device = findLocked(object);
    if (object != kAudioObjectPlugInObject && !device)
        return kAudioHardwareBadObjectError;
    UInt32 bytes = 0;
    const auto status =
        propertySize(device, object, address, devices_.size(), sampleRateMask_, bytes);
    if (status)
        return status;
    result =
        device && device->runtime->lifecycle->alive.load() && global(address) &&
        (object == device->object ? address.mSelector == kAudioDevicePropertyNominalSampleRate
                                  : address.mSelector == kAudioStreamPropertyVirtualFormat ||
                                        address.mSelector == kAudioStreamPropertyPhysicalFormat);
    return noErr;
}
OSStatus State::set(AudioObjectID object, const AudioObjectPropertyAddress& address, UInt32 bytes,
                    const void* data) {
    std::lock_guard guard(mutex_);
    auto* device = findLocked(object);
    if (object != kAudioObjectPlugInObject && !device)
        return kAudioHardwareBadObjectError;
    UInt32 required = 0;
    const auto status =
        propertySize(device, object, address, devices_.size(), sampleRateMask_, required);
    if (status)
        return status;
    const bool endpoint = device && object == device->object;
    const bool nominal = endpoint && address.mSelector == kAudioDevicePropertyNominalSampleRate;
    const bool stream = device && !endpoint &&
                        (address.mSelector == kAudioStreamPropertyVirtualFormat ||
                         address.mSelector == kAudioStreamPropertyPhysicalFormat);
    if (!global(address) || (!nominal && !stream))
        return kAudioHardwareIllegalOperationError;
    if (!data || bytes != required)
        return kAudioHardwareBadPropertySizeError;
    if (!device->runtime->lifecycle->alive.load())
        return kAudioHardwareNotReadyError;
    double rate;
    if (nominal)
        std::memcpy(&rate, data, sizeof(rate));
    else {
        AudioStreamBasicDescription proposed{};
        std::memcpy(&proposed, data, sizeof(proposed));
        auto expected = format(*device);
        rate = proposed.mSampleRate;
        proposed.mSampleRate = expected.mSampleRate;
        if (std::memcmp(&proposed, &expected, sizeof(expected)))
            return kAudioDeviceUnsupportedFormatError;
    }
    if (!(sampleRateMask_ & virtualSampleRateBit(rate)))
        return kAudioDeviceUnsupportedFormatError;
    if (nextRateRequest_ == UINT64_MAX)
        return kAudioHardwareIllegalOperationError;
    // This includes requests for the currently active rate: a different rate
    // may already be accepted by the helper but not yet visible in HAL.
    device->runtime->lifecycle->rateRequestSequence = nextRateRequest_++;
    device->runtime->lifecycle->requestedRate = static_cast<uint32_t>(rate);
    return noErr;
}
std::vector<DriverRateRequest> State::rateRequests() {
    std::lock_guard guard(mutex_);
    std::vector<DriverRateRequest> result;
    result.reserve(devices_.size());
    for (const auto& device : devices_) {
        const auto& lifecycle = *device->runtime->lifecycle;
        if (lifecycle.rateRequestSequence &&
            (sampleRateMask_ & virtualSampleRateBit(lifecycle.requestedRate)))
            result.push_back({device->runtime->transport.id(), lifecycle.rateRequestSequence,
                              lifecycle.requestedRate, rateOrigin_});
    }
    return result;
}
OSStatus State::acknowledgeRates(std::span<const DriverRateRequest> requests) {
    if (requests.size() > kMaximumVirtualDevices)
        return kAudioHardwareIllegalOperationError;
    for (const auto& request : requests)
        if (request.device.empty() || !request.sequence ||
            !supportedVirtualSampleRate(request.sampleRate))
            return kAudioHardwareIllegalOperationError;
    std::lock_guard guard(mutex_);
    for (const auto& request : requests)
        for (const auto& device : devices_)
            if (device->runtime->transport.id() == request.device) {
                auto& lifecycle = *device->runtime->lifecycle;
                if (request.origin == rateOrigin_ &&
                    lifecycle.rateRequestSequence == request.sequence &&
                    lifecycle.requestedRate == request.sampleRate)
                    lifecycle.rateRequestSequence = 0;
            }
    return noErr;
}
OSStatus State::size(AudioObjectID object, const AudioObjectPropertyAddress& address,
                     UInt32& bytes) {
    std::lock_guard guard(mutex_);
    auto* device = findLocked(object);
    if (object != kAudioObjectPlugInObject && !device)
        return kAudioHardwareBadObjectError;
    return propertySize(device, object, address, devices_.size(), sampleRateMask_, bytes);
}
OSStatus State::get(AudioObjectID object, const AudioObjectPropertyAddress& address,
                    UInt32 qualifierBytes, const void* qualifier, UInt32 capacity, UInt32& bytes,
                    void* output) {
    std::lock_guard guard(mutex_);
    bytes = 0;
    auto* device = findLocked(object);
    if (object != kAudioObjectPlugInObject && !device)
        return kAudioHardwareBadObjectError;
    UInt32 required = 0;
    const auto status =
        propertySize(device, object, address, devices_.size(), sampleRateMask_, required);
    if (status)
        return status;
    const bool plugin = object == kAudioObjectPlugInObject;
    const bool endpoint = device && object == device->object;
    const auto selector = address.mSelector;
    const bool list = selector == kAudioObjectPropertyOwnedObjects ||
                      selector == kAudioPlugInPropertyDeviceList ||
                      selector == kAudioDevicePropertyStreams;
    const bool rates = endpoint && selector == kAudioDevicePropertyAvailableNominalSampleRates;
    const bool formats = device && !endpoint &&
                         (selector == kAudioStreamPropertyAvailableVirtualFormats ||
                          selector == kAudioStreamPropertyAvailablePhysicalFormats);
    if ((required && !output && capacity) || (!list && !rates && !formats && capacity < required))
        return kAudioHardwareBadPropertySizeError;
    if (rates || formats) {
        const size_t stride =
            rates ? sizeof(AudioValueRange) : sizeof(AudioStreamRangedDescription);
        const auto mask =
            sampleRateMask_ | virtualSampleRateBit(device->runtime->audio.sampleRate());
        const size_t count = std::min<size_t>(std::popcount(mask), capacity / stride);
        size_t written = 0;
        for (size_t index = 0; index < kVirtualSampleRates.size() && written < count; ++index) {
            if (!(mask & (1U << index)))
                continue;
            const double rate = kVirtualSampleRates[index];
            auto* destination = static_cast<std::byte*>(output) + written++ * stride;
            if (rates)
                scalar(destination, AudioValueRange{rate, rate});
            else {
                auto value = format(*device);
                value.mSampleRate = rate;
                scalar(destination, AudioStreamRangedDescription{value, {rate, rate}});
            }
        }
        bytes = static_cast<UInt32>(count * stride);
        return noErr;
    }
    if (list) {
        const auto count = std::min(capacity, required) / sizeof(AudioObjectID);
        for (size_t index = 0; index < count; ++index) {
            AudioObjectID value;
            if (plugin) {
                value = devices_[index]->object;
            } else {
                size_t streamCount;
                value = streams(*device, address, streamCount)[index];
            }
            std::memcpy(static_cast<std::byte*>(output) + index * sizeof(value), &value,
                        sizeof(value));
        }
        bytes = count * sizeof(AudioObjectID);
        return noErr;
    }
    if (!required)
        return noErr;
    bytes = required;
    switch (selector) {
    case kAudioObjectPropertyCustomPropertyInfoList: {
        const AudioServerPlugInCustomPropertyInfo info[]{
            {kLoopZDevicePropertyIOTiming, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone},
            {kLoopZDevicePropertyCallbackTiming,
             kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone}};
        std::memcpy(output, info, sizeof(info));
        return noErr;
    }
    case kLoopZDevicePropertyCallbackTiming: {
        const auto timing = CallbackTimingObservation::decode(
            device->runtime->lifecycle->callbackTiming.snapshot());
        const int64_t numbers[]{kLoopZDeviceCallbackTimingVersion, timing.session,
                                timing.maximumWriteFrames, timing.maximumNominalFrames};
        const void* keys[]{CFSTR("version"), CFSTR("session"), CFSTR("maximumWriteFrames"),
                           CFSTR("maximumNominalFrames"), CFSTR("running")};
        const void* values[5]{};
        bool complete = true;
        for (size_t index = 0; index < 4; ++index) {
            values[index] = CFNumberCreate(nullptr, kCFNumberSInt64Type, numbers + index);
            complete &= values[index] != nullptr;
        }
        values[4] = device->runtime->lifecycle->running.load() ? kCFBooleanTrue : kCFBooleanFalse;
        const auto result =
            complete ? CFDictionaryCreate(nullptr, keys, values, 5, &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks)
                     : nullptr;
        for (size_t index = 0; index < 4; ++index)
            if (values[index])
                CFRelease(values[index]);
        if (!result) {
            bytes = 0;
            return kAudioHardwareUnspecifiedError;
        }
        scalar(output, result);
        return noErr;
    }
    case kLoopZDevicePropertyIOTiming: {
        const auto timing = device->runtime->lifecycle->writeTiming.load();
        const int64_t version = kLoopZDeviceIOTimingVersion, session = timing >> 32,
                      period = uint32_t(timing);
        const void* keys[]{CFSTR("version"), CFSTR("session"), CFSTR("writePeriodFrames"),
                           CFSTR("running")};
        const void* values[]{CFNumberCreate(nullptr, kCFNumberSInt64Type, &version),
                             CFNumberCreate(nullptr, kCFNumberSInt64Type, &session),
                             CFNumberCreate(nullptr, kCFNumberSInt64Type, &period),
                             device->runtime->lifecycle->running.load() ? kCFBooleanTrue
                                                                        : kCFBooleanFalse};
        CFDictionaryRef result = nullptr;
        if (values[0] && values[1] && values[2])
            result = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks);
        for (size_t index = 0; index < 3; ++index)
            if (values[index])
                CFRelease(values[index]);
        if (!result) {
            bytes = 0;
            return kAudioHardwareUnspecifiedError;
        }
        scalar(output, result); // The caller owns this retained property list.
        return noErr;
    }
    case kAudioObjectPropertyBaseClass:
        scalar(output, UInt32(kAudioObjectClassID));
        return noErr;
    case kAudioObjectPropertyClass:
        scalar(output, plugin     ? UInt32(kAudioPlugInClassID)
                       : endpoint ? UInt32(kAudioDeviceClassID)
                                  : UInt32(kAudioStreamClassID));
        return noErr;
    case kAudioObjectPropertyOwner:
        scalar(output, UInt32(plugin     ? kAudioObjectUnknown
                              : endpoint ? kAudioObjectPlugInObject
                                         : device->object));
        return noErr;
    case kAudioObjectPropertyName:
        text(output, plugin                    ? CFSTR("LoopZ Routing")
                     : endpoint                ? device->name.get()
                     : object == device->input ? CFSTR("Mixed audio")
                                               : CFSTR("Pass-through"));
        return noErr;
    case kAudioObjectPropertyManufacturer:
        text(output, CFSTR("Lexasoft"));
        return noErr;
    }
    if (plugin) {
        if (selector == kAudioPlugInPropertyBundleID) {
#if defined(LOOPZ_LEGACY_DRIVER_SLOT)
            text(output, CFSTR("com.lexasoft.loopz.driver"));
#else
            text(output, CFSTR("com.lexasoft.loopz.driver.routing"));
#endif
        } else if (selector == kAudioPlugInPropertyResourceBundle) {
            text(output, CFSTR(""));
        } else if (selector == kAudioPlugInPropertyTranslateUIDToDevice) {
            if (qualifierBytes != sizeof(CFStringRef) || !qualifier)
                return kAudioHardwareBadPropertySizeError;
            const auto uid = *static_cast<const CFStringRef*>(qualifier);
            if (!uid || CFGetTypeID(uid) != CFStringGetTypeID())
                return kAudioHardwareIllegalOperationError;
            AudioObjectID result = kAudioObjectUnknown;
            for (const auto& candidate : devices_)
                if (CFEqual(candidate->uid.get(), uid))
                    result = candidate->object;
            scalar(output, result);
        }
        return noErr;
    }
    if (endpoint) {
        UInt32 value = 0;
        switch (selector) {
        case kAudioDevicePropertyDeviceUID:
            text(output, device->uid.get());
            return noErr;
        case kAudioDevicePropertyModelUID:
            text(output, CFSTR("com.lexasoft.loopz.device.model.v2"));
            return noErr;
        case kAudioDevicePropertyNominalSampleRate:
            scalar(output, Float64(device->runtime->audio.sampleRate()));
            return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            const UInt32 stereo[]{1, 2};
            std::memcpy(output, stereo, sizeof(stereo));
            return noErr;
        }
        case kAudioDevicePropertyRelatedDevices:
            value = device->object;
            break;
        case kAudioDevicePropertyLatency:
            if (address.mScope == kAudioObjectPropertyScopeInput)
                value = device->runtime->transport.inputLatencyFrames();
            break;
        case kAudioDevicePropertyTransportType:
            value = kAudioDeviceTransportTypeVirtual;
            break;
        case kAudioDevicePropertyDeviceIsAlive:
            value = device->runtime->lifecycle->alive.load();
            break;
        case kAudioDevicePropertyDeviceIsRunning:
            value = device->runtime->lifecycle->running.load() != 0;
            break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            value = address.mScope == kAudioObjectPropertyScopeInput || device->hasOutput;
            break;
        case kAudioDevicePropertyZeroTimeStampPeriod:
            value = kClockPeriodFrames;
            break;
        case kAudioDevicePropertyClockAlgorithm:
            value = kAudioDeviceClockAlgorithmRaw;
            break;
        case kAudioDevicePropertyClockIsStable:
            value = 1;
            break;
        }
        scalar(output, value);
        return noErr;
    }
    if (selector == kAudioStreamPropertyVirtualFormat ||
        selector == kAudioStreamPropertyPhysicalFormat) {
        scalar(output, format(*device));
    } else {
        UInt32 value = 0;
        switch (selector) {
        case kAudioStreamPropertyIsActive:
            value = device->runtime->lifecycle->alive.load();
            break;
        case kAudioStreamPropertyDirection:
            value = object == device->input;
            break;
        case kAudioStreamPropertyTerminalType:
            value = kAudioStreamTerminalTypeLine;
            break;
        case kAudioStreamPropertyStartingChannel:
            value = 1;
            break;
        }
        scalar(output, value);
    }
    return noErr;
}
} // namespace loopz::macos::hal
