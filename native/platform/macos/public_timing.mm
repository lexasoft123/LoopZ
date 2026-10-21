#include <loopz/macos/public_timing.hpp>
#include <loopz/macos/public_timing_observer.hpp>
#include <loopz/macos/driver_timing.hpp>
#include <loopz/macos/device_transport.hpp>
#include <loopz/sinc_kernel.hpp>
#include <loopz/timeline.hpp>
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <chrono>

namespace loopz::macos {
namespace {
uint32_t resolve(const std::string& uid) {
    CFStringRef text = (__bridge CFStringRef) @(uid.c_str());
    AudioObjectID device = 0;
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 bytes = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(text), &text, &bytes,
                                   &device) ||
        bytes != sizeof(device))
        throw std::runtime_error("Cannot inspect public device timing identity");
    return device;
}
bool validateIdentity(uint32_t device) {
    CFStringRef model = nullptr;
    AudioObjectPropertyAddress address{kAudioDevicePropertyModelUID,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 bytes = sizeof(model);
    const auto modelStatus =
        AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &model);
    const bool ours = !modelStatus && bytes == sizeof(model) && model &&
                      CFGetTypeID(model) == CFStringGetTypeID() &&
                      CFEqual(model, CFSTR("com.lexasoft.loopz.device.model.v2"));
    if (model)
        CFRelease(model);
    UInt32 transport = 0;
    address.mSelector = kAudioDevicePropertyTransportType;
    bytes = sizeof(transport);
    if (!ours || AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &transport) ||
        bytes != sizeof(transport) || transport != kAudioDeviceTransportTypeVirtual)
        throw std::runtime_error("Public timing identity is not a LoopZ virtual endpoint");
    address.mSelector = kLoopZDevicePropertyIOTiming;
    return AudioObjectHasProperty(device, &address);
}
PublicDeviceTiming readTiming(uint32_t device, bool reportsTiming) {
    AudioObjectPropertyAddress address{kLoopZDevicePropertyIOTiming,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (reportsTiming) {
        CFPropertyListRef value = nullptr;
        bytes = sizeof(value);
        const auto status =
            AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &value);
        if (status || bytes != sizeof(value) || !value) {
            if (value)
                CFRelease(value);
            throw std::runtime_error("Cannot inspect driver callback timing");
        }
        try {
            const auto result = decodePublicDeviceTiming(value);
            CFRelease(value);
            return result;
        } catch (...) {
            CFRelease(value);
            throw;
        }
    }
    // Older drivers do not expose callback timing. A caller's buffer-frame
    // property is not evidence of another process's IO period; retain the
    // tested 512-frame fallback until the driver reports actual callbacks.
    UInt32 running = 0;
    address.mSelector = kAudioDevicePropertyDeviceIsRunningSomewhere;
    bytes = sizeof(running);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &running) ||
        bytes != sizeof(running))
        throw std::runtime_error("Cannot inspect public device IO activity");
    return PublicDeviceTiming{512, running != 0, 0, false};
}
} // namespace
PublicTimingCatalog::PublicTimingCatalog(Operations operations)
    : operations_(std::move(operations)) {
    if (!operations_.resolve || !operations_.validate || !operations_.read)
        throw std::invalid_argument("Public timing discovery needs all native operations");
}
std::optional<PublicDeviceTiming> PublicTimingCatalog::probe(const std::string& uid) {
    try {
        const auto object = operations_.resolve(uid);
        if (!object) {
            identities_.erase(uid);
            return std::nullopt;
        }
        auto found = identities_.find(uid);
        if (found == identities_.end() || found->second.object != object) {
            // Bound discovery storage independently of document churn. Clearing
            // validation cannot discard a running queue or any timing sample.
            if (identities_.size() >= 256)
                identities_.clear();
            const auto reportsTiming = operations_.validate(object);
            found = identities_.insert_or_assign(uid, Identity{object, reportsTiming}).first;
        }
        return operations_.read(object, found->second.reportsTiming);
    } catch (...) {
        identities_.erase(uid);
        throw;
    }
}
void PublicTimingCatalog::invalidate() noexcept {
    identities_.clear();
}
PublicDeviceTiming decodePublicDeviceTiming(CFPropertyListRef value) {
    if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID() ||
        CFDictionaryGetCount(static_cast<CFDictionaryRef>(value)) != 4)
        throw std::runtime_error("Invalid driver timing dictionary");
    const auto dictionary = static_cast<CFDictionaryRef>(value);
    const auto number = [&](CFStringRef key, int64_t maximum) {
        const auto item = CFDictionaryGetValue(dictionary, key);
        int64_t result = -1;
        if (!item || CFGetTypeID(item) != CFNumberGetTypeID() ||
            CFNumberIsFloatType(static_cast<CFNumberRef>(item)) ||
            !CFNumberGetValue(static_cast<CFNumberRef>(item), kCFNumberSInt64Type, &result) ||
            result < 0 || result > maximum)
            throw std::runtime_error("Invalid driver timing integer");
        return static_cast<uint32_t>(result);
    };
    if (number(CFSTR("version"), UINT32_MAX) != kLoopZDeviceIOTimingVersion)
        throw std::runtime_error("Unsupported driver timing version");
    const auto session = number(CFSTR("session"), UINT32_MAX);
    const auto period = number(CFSTR("writePeriodFrames"), kDeviceAudioCapacity);
    const auto running = CFDictionaryGetValue(dictionary, CFSTR("running"));
    if (!running || CFGetTypeID(running) != CFBooleanGetTypeID() || (period && !session))
        throw std::runtime_error("Invalid driver timing session");
    return {period ? period : 512U, CFBooleanGetValue(static_cast<CFBooleanRef>(running)) != 0,
            session, period != 0};
}
PublicPassThroughTiming::PublicPassThroughTiming(SceneTiming timing, Probe probe,
                                                 uint32_t renderLeadFrames)
    : timing_(timing),
      observer_(probe ? nullptr
                      : std::make_shared<PublicTimingObserver>(PublicTimingCatalog::Operations{
                            resolve, validateIdentity, readTiming})),
      probe_(std::move(probe)), lead_(renderLeadFrames) {
    if (lead_ > kCapacity / 2)
        throw std::invalid_argument("Public timing render lead exceeds history capacity");
}
void PublicPassThroughTiming::invalidateNativeIdentities() noexcept {
    if (observer_)
        observer_->invalidate();
}
std::shared_ptr<const PublicTimingObservationBatch>
PublicPassThroughTiming::observations() const noexcept {
    return observer_ ? observer_->latest() : nullptr;
}
std::vector<DevicePassThroughDelay> PublicPassThroughTiming::floors(const RouteDocument& document) {
    std::vector<std::string> targets;
    for (const auto& device : document.devices)
        if (device.routing.enabled &&
            std::any_of(device.bindings.begin(), device.bindings.end(), [](const auto& binding) {
                return binding.kind == SourceKind::PassThrough;
            }))
            targets.push_back("com.lexasoft.loopz.device." + device.id);
    for (const auto& [id, _] : retained_)
        targets.push_back("com.lexasoft.loopz.device." + id);
    const auto nowNs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
    const auto batch = observer_ ? observer_->snapshot(targets, nowNs) : nullptr;
    const auto observe = [&](const std::string& uid) -> std::optional<PublicDeviceTiming> {
        if (!observer_)
            return probe_(uid);
        if (!batch || !batch->devices.contains(uid))
            return std::nullopt;
        const auto& observation = batch->devices.at(uid);
        if (!observation.error.empty())
            throw std::runtime_error(observation.error);
        return observation.value;
    };
    std::unordered_set<std::string> retainedIDs;
    std::vector<DevicePassThroughDelay> result;
    for (const auto& device : document.devices) {
        if (!device.routing.enabled ||
            std::none_of(device.bindings.begin(), device.bindings.end(), [](const auto& binding) {
                return binding.kind == SourceKind::PassThrough;
            }))
            continue;
        retainedIDs.insert(device.id);
        const auto observed = observe("com.lexasoft.loopz.device." + device.id);
        const auto previous = retained_.find(device.id);
        // Before publication, use the 512-frame default observed on supported
        // macOS hosts. Subsequent control polls reconcile the actual period.
        auto period = observed ? observed->bufferFrames : 512U;
        if (!period || period > kCapacity / 2 || device.sampleRate < 8000 ||
            device.sampleRate > 192000)
            throw std::runtime_error("Public IO period exceeds the supported timing bounds");
        // StartIO clears the callback observation before the first WriteMix of
        // its new session. This is not evidence that a known 64-frame writer
        // became a 512-frame writer. Retain its last actual observation until
        // a callback supplies a new period; otherwise latency edits can trigger
        // their own fallback/reconfiguration cycle during native IO restarts.
        uint32_t measuredPeriod = 0;
        if (observed) {
            if (observed->measured)
                measuredPeriod = observed->bufferFrames;
            else if (observed->session && previous != retained_.end())
                measuredPeriod = previous->second.measuredPeriod;
            if (!observed->measured && measuredPeriod)
                period = measuredPeriod;
            if (observed->measured && observed->running && previous != retained_.end() &&
                previous->second.running && previous->second.session == observed->session)
                measuredPeriod = std::max(measuredPeriod, previous->second.measuredPeriod);
        }
        const auto onePeriod = double(period) * timing_.renderRate / device.sampleRate;
        const auto converted = std::ceil(onePeriod + std::max(onePeriod, double(lead_)));
        double wing = 0;
        if (device.sampleRate != timing_.renderRate) {
            const auto cutoff =
                SincKernel::kCutoff * std::min(1.0, timing_.renderRate / device.sampleRate);
            wing = std::ceil(std::ceil(SincKernel::kRadius / cutoff) * timing_.renderRate /
                             device.sampleRate);
        }
        const auto required = converted + timing_.blockFrames + wing;
        if (!std::isfinite(required) || required > kCapacity / 2)
            throw std::runtime_error("Public IO queue exceeds the bounded history capacity");
        auto delay = std::max(timing_.passThroughDelayFrames, static_cast<uint32_t>(required));
        const bool running = observed && observed->running && observed->measured;
        if (running) {
            if (previous != retained_.end() && previous->second.running &&
                previous->second.session == observed->session)
                delay = std::max(delay, previous->second.frames);
        }
        retained_[device.id] = {delay, running, observed ? observed->session : 0, measuredPeriod};
        result.push_back({device.id, delay});
    }
    // A rejected/removal-in-progress document must not forget the larger queue
    // of an old endpoint that is still running. This cache never owns IO.
    for (auto entry = retained_.begin(); entry != retained_.end();) {
        if (retainedIDs.contains(entry->first)) {
            ++entry;
            continue;
        }
        const auto observed = observe("com.lexasoft.loopz.device." + entry->first);
        if (!observed || !observed->running)
            entry = retained_.erase(entry);
        else
            ++entry;
    }
    if (retained_.size() > 256)
        throw std::runtime_error("Too many live public timing identities awaiting retirement");
    return result;
}
} // namespace loopz::macos
