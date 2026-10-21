#include <loopz/macos/input_timing.hpp>
#include <loopz/timeline.hpp>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace loopz::macos {
namespace {
AudioObjectPropertyAddress
property(AudioObjectPropertySelector selector,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template <class T>
bool get(AudioObjectID device, AudioObjectPropertySelector selector, T& result,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const auto address = property(selector, scope);
    UInt32 bytes = sizeof(result);
    return !AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &result) &&
           bytes == sizeof(result);
}
AudioDeviceID resolve(const std::string& selector) {
    AudioDeviceID device = 0;
    if (selector == "default") {
        if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, device))
            return {};
    } else {
        auto uid = CFStringCreateWithCString(nullptr, selector.c_str(), kCFStringEncodingUTF8);
        if (!uid)
            return {};
        UInt32 bytes = sizeof(device);
        const auto address = property(kAudioHardwarePropertyTranslateUIDToDevice);
        const auto error = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                      sizeof(uid), &uid, &bytes, &device);
        CFRelease(uid);
        if (error || bytes != sizeof(device))
            return {};
    }
    return device;
}
std::optional<InputTimingFacts> inspect(const std::string& selector) {
    const auto device = resolve(selector);
    UInt32 alive = 0, transport = 0, period = 0, latency = 0, safety = 0;
    Float64 rate = 0;
    if (!device || !get(device, kAudioDevicePropertyDeviceIsAlive, alive) || !alive ||
        !get(device, kAudioDevicePropertyTransportType, transport) ||
        transport == kAudioDeviceTransportTypeVirtual ||
        !get(device, kAudioDevicePropertyNominalSampleRate, rate) ||
        !get(device, kAudioDevicePropertyBufferFrameSize, period) ||
        !get(device, kAudioDevicePropertyLatency, latency, kAudioDevicePropertyScopeInput) ||
        !get(device, kAudioDevicePropertySafetyOffset, safety, kAudioDevicePropertyScopeInput))
        return {};
    const auto address = property(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &bytes) || !bytes ||
        bytes % sizeof(AudioStreamID) || bytes > 256 * sizeof(AudioStreamID))
        return {};
    std::vector<AudioStreamID> streams(bytes / sizeof(AudioStreamID));
    const auto capacity = bytes;
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, streams.data()) ||
        bytes != capacity)
        return {};
    UInt32 streamLatency = 0;
    for (auto stream : streams) {
        UInt32 value = 0;
        if (!get(stream, kAudioStreamPropertyLatency, value))
            return {};
        streamLatency = std::max(streamLatency, value);
    }
    UInt32 currentPeriod = 0, currentAlive = 0;
    Float64 currentRate = 0;
    if (resolve(selector) != device ||
        !get(device, kAudioDevicePropertyDeviceIsAlive, currentAlive) || !currentAlive ||
        !get(device, kAudioDevicePropertyBufferFrameSize, currentPeriod) ||
        currentPeriod != period ||
        !get(device, kAudioDevicePropertyNominalSampleRate, currentRate) || currentRate != rate)
        return {};
    const auto total = uint64_t(latency) + safety + streamLatency;
    return InputTimingFacts{
        rate, period, uint32_t(std::min(total, uint64_t(std::numeric_limits<uint32_t>::max())))};
}
} // namespace

PhysicalInputTiming::PhysicalInputTiming(SceneTiming timing, uint32_t renderLeadFrames, Probe probe)
    : PhysicalInputTiming(timing, renderLeadFrames, std::move(probe),
                          ResamplingFilter::LinearPhase) {}
PhysicalInputTiming::PhysicalInputTiming(SceneTiming timing, uint32_t renderLeadFrames, Probe probe,
                                         ResamplingFilter filter)
    : timing_(timing), lead_(renderLeadFrames), filter_(filter),
      probe_(probe ? std::move(probe) : Probe{inspect}) {
    (void)planSceneTiming({}, timing);
    if (lead_ > kCapacity / 2)
        throw std::invalid_argument("Input render lead exceeds history capacity");
    if (filter_ != ResamplingFilter::LinearPhase && filter_ != ResamplingFilter::MinimumPhase)
        throw std::invalid_argument("Unsupported input timing filter");
}
std::vector<DeviceInputDelay> PhysicalInputTiming::delays(const RouteDocument& document,
                                                          const RouteDocument* active) {
    if (active) {
        std::unordered_set<std::string> referenced;
        for (const auto* route : {&document, active})
            for (const auto& device : route->devices)
                for (const auto& binding : device.bindings)
                    if (binding.kind == SourceKind::InputDevice)
                        referenced.insert(binding.selector);
        std::erase_if(retained_, [&](const auto& entry) {
            return !referenced.contains(entry.first);
        });
    }
    const auto filters = planPhysicalInputFilters(document, filter_);
    std::map<std::pair<std::string, ResamplingFilter>, uint32_t> current;
    std::unordered_map<std::string, std::optional<InputTimingFacts>> facts;
    std::unordered_map<std::string, uint32_t> verified;
    std::vector<DeviceInputDelay> result;
    for (const auto& device : document.devices) {
        if (!device.routing.enabled)
            continue;
        for (const auto& binding : device.bindings) {
            if (binding.kind != SourceKind::InputDevice)
                continue;
            const auto route = std::find_if(device.routing.sources.begin(),
                                            device.routing.sources.end(), [&](const auto& item) {
                                                return item.id == binding.id;
                                            });
            if (route == device.routing.sources.end() || !route->enabled)
                continue;
            const auto selected =
                std::find_if(filters.begin(), filters.end(), [&](const auto& item) {
                    return item.device == device.id && item.source == binding.id;
                });
            if (selected == filters.end())
                throw std::logic_error("Missing physical input filter plan");
            const auto key = std::pair{binding.selector, selected->filter};
            auto found = current.find(key);
            if (found == current.end()) {
                uint32_t budget = std::max(1024U, timing_.sourceDelayFrames);
                auto cached = facts.find(binding.selector);
                if (cached == facts.end())
                    cached = facts.emplace(binding.selector, probe_(binding.selector)).first;
                if (cached->second) {
                    const auto calculated =
                        inputTimingBudget(*cached->second, timing_, lead_, selected->filter);
                    if (!calculated)
                        throw std::runtime_error("Physical input timing exceeds supported history");
                    budget = *calculated;
                    // Preparation can be rejected while a previous graph still
                    // uses this input. Keep its last verified budget even when
                    // a tentative document removes the binding.
                    constexpr auto maximumIdentities =
                        2 * kMaximumVirtualDevices * kMaximumRoutingSources;
                    if (!retained_.contains(binding.selector) &&
                        retained_.size() >= maximumIdentities)
                        throw std::runtime_error(
                            "Too many physical timing identities in this session");
                    auto& maximum = verified[binding.selector];
                    maximum = std::max(maximum, budget);
                    retained_[binding.selector] = maximum;
                } else if (const auto previous = retained_.find(binding.selector);
                           previous != retained_.end()) {
                    budget = std::max(budget, previous->second);
                }
                found = current.emplace(key, budget).first;
            }
            result.push_back({device.id, binding.id, found->second, selected->filter});
        }
    }
    return result;
}
} // namespace loopz::macos
