#include <loopz/macos/transport_message.hpp>
#include <loopz/macos/driver_configuration.hpp>
#include <loopz/macos/configuration_controller.hpp>
#include <loopz/macos/routing_service.hpp>
#include <loopz/macos/scene_renderer.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/public_timing.hpp>
#include <loopz/macos/driver_timing.hpp>
#include <loopz/callback_timing.hpp>
#include "../../../native/platform/macos/routing_service/wire.hpp"
#include "test_code_identity.hpp"
#include <unistd.h>
#include <loopz/timeline.hpp>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <dlfcn.h>
#include <filesystem>
#include <cstdlib>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <latch>
#include <functional>
#include <thread>

namespace {
using namespace loopz;
using namespace loopz::macos;
AudioServerPlugInDriverRef driver;
using Apply = OSStatus (*)(xpc_object_t);
Apply applySnapshot;
using Status = OSStatus (*)(uint64_t, DriverConfigurationResult*);
using Cancel = OSStatus (*)(uint64_t);
Status configurationStatus;
Cancel cancelConfiguration;
using Availability = OSStatus (*)(Boolean);
Availability serviceAvailability;
using RateCapabilities = OSStatus (*)(uint32_t);
RateCapabilities rateCapabilities;
using Rates = xpc_object_t (*)();
using AcknowledgeRates = OSStatus (*)(xpc_object_t);
Rates pendingRates;
AcknowledgeRates acknowledgeRates;
using Phase = DriverConfigurationPhase;
enum class HostReply { Reject, Defer, InlineApproval };
std::atomic<HostReply> hostReply{HostReply::Reject};
struct Request {
    AudioObjectID device;
    uint64_t action;
};
std::mutex requestMutex;
std::condition_variable requestReady;
std::deque<Request> requests;
std::atomic<uint32_t> notifications{0};
std::atomic<uint32_t> configurationRequests{0};
std::mutex latencyMutex;
std::vector<std::pair<AudioObjectID, UInt32>> latencyNotifications;
std::mutex runningMutex;
std::vector<std::pair<AudioObjectID, UInt32>> runningNotifications;
uint64_t revision = 0;
mach_timebase_info_data_t timebase{};

AudioObjectPropertyAddress
address(AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
OSStatus changed(AudioServerPlugInHostRef, AudioObjectID object, UInt32 count,
                 const AudioObjectPropertyAddress* notices) {
    ++notifications;
    // Reentrant host queries must not deadlock against the publisher's mutex.
    auto property = address(kAudioPlugInPropertyDeviceList);
    UInt32 bytes = 0;
    assert((*driver)->GetPropertyDataSize(driver, kAudioObjectPlugInObject, 0, &property, 0,
                                          nullptr, &bytes) == noErr);
    for (UInt32 index = 0; index < count; ++index) {
        if (notices[index].mSelector == kAudioDevicePropertyDeviceIsRunning) {
            UInt32 running = UINT32_MAX;
            assert((*driver)->GetPropertyData(driver, object, 0, &notices[index], 0, nullptr,
                                              sizeof(running), &bytes, &running) == noErr);
            assert(bytes == sizeof(running) && running <= 1);
            std::lock_guard guard(runningMutex);
            runningNotifications.emplace_back(object, running);
        }
        if (notices[index].mSelector == kAudioDevicePropertyLatency) {
            assert(notices[index].mScope == kAudioObjectPropertyScopeInput &&
                   notices[index].mElement == kAudioObjectPropertyElementMain);
            UInt32 latency = UINT32_MAX;
            assert((*driver)->GetPropertyData(driver, object, 0, &notices[index], 0, nullptr,
                                              sizeof(latency), &bytes, &latency) == noErr);
            assert(bytes == sizeof(latency));
            std::lock_guard guard(latencyMutex);
            latencyNotifications.emplace_back(object, latency);
        }
        if (notices[index].mSelector != kAudioDevicePropertyDeviceIsAlive)
            continue;
        UInt32 alive = 1;
        assert((*driver)->GetPropertyData(driver, object, 0, &notices[index], 0, nullptr,
                                          sizeof(alive), &bytes, &alive) == noErr);
        assert(!alive);
    }
    return noErr;
}
OSStatus storage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* output) {
    *output = nullptr;
    return noErr;
}
OSStatus writeStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef) {
    return noErr;
}
OSStatus removeStorage(AudioServerPlugInHostRef, CFStringRef) {
    return noErr;
}
OSStatus request(AudioServerPlugInHostRef, AudioObjectID device, UInt64 action, void* info) {
    assert(!info);
    ++configurationRequests;
    switch (hostReply.load()) {
    case HostReply::Reject:
        return kAudioHardwareUnsupportedOperationError;
    case HostReply::InlineApproval:
        return (*driver)->PerformDeviceConfigurationChange(driver, device, action, nullptr);
    case HostReply::Defer: {
        std::lock_guard guard(requestMutex);
        requests.push_back({device, action});
    }
        requestReady.notify_one();
        return noErr;
    }
    std::abort();
}
Request nextRequest() {
    std::unique_lock guard(requestMutex);
    const bool ready = requestReady.wait_for(guard, std::chrono::seconds(3), [] {
        return !requests.empty();
    });
    assert(ready);
    const auto value = requests.front();
    requests.pop_front();
    return value;
}
DriverConfigurationResult result(uint64_t revision) {
    DriverConfigurationResult value;
    assert(configurationStatus(revision, &value) == noErr);
    return value;
}
DriverConfigurationResult waitPhase(uint64_t revision, Phase phase,
                                    std::chrono::seconds limit = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    DriverConfigurationResult value;
    do {
        value = result(revision);
        if (value.phase == phase)
            return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "Expected phase %u, got %u for revision %llu\n", unsigned(phase),
                 unsigned(value.phase), static_cast<unsigned long long>(revision));
    std::abort();
}
void approve(Request request) {
    assert((*driver)->PerformDeviceConfigurationChange(driver, request.device, request.action,
                                                       nullptr) == noErr);
}
void reject(Request request) {
    assert((*driver)->AbortDeviceConfigurationChange(driver, request.device, request.action,
                                                     nullptr) == noErr);
}

template <typename T>
T property(AudioObjectID object, AudioObjectPropertySelector selector,
           AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    auto location = address(selector, scope);
    T value{};
    UInt32 bytes = 0;
    assert((*driver)->GetPropertyData(driver, object, 0, &location, 0, nullptr, sizeof(value),
                                      &bytes, &value) == noErr);
    assert(bytes == sizeof(value));
    return value;
}
PublicDeviceTiming driverTiming(AudioObjectID object) {
    const auto raw = property<CFPropertyListRef>(object, kLoopZDevicePropertyIOTiming);
    const auto decoded = decodePublicDeviceTiming(raw);
    CFRelease(raw);
    return decoded;
}
CallbackTimingValues callbackTiming(AudioObjectID object) {
    const auto raw = property<CFPropertyListRef>(object, kLoopZDevicePropertyCallbackTiming);
    assert(CFGetTypeID(raw) == CFDictionaryGetTypeID());
    const auto dictionary = static_cast<CFDictionaryRef>(raw);
    assert(CFDictionaryGetCount(dictionary) == 5);
    const auto number = [&](CFStringRef key) {
        const auto item = CFDictionaryGetValue(dictionary, key);
        int64_t value = -1;
        assert(item && CFGetTypeID(item) == CFNumberGetTypeID() &&
               CFNumberGetValue(static_cast<CFNumberRef>(item), kCFNumberSInt64Type, &value) &&
               value >= 0 && value <= UINT32_MAX);
        return static_cast<uint32_t>(value);
    };
    assert(number(CFSTR("version")) == kLoopZDeviceCallbackTimingVersion);
    const CallbackTimingValues result{number(CFSTR("session")), number(CFSTR("maximumWriteFrames")),
                                      number(CFSTR("maximumNominalFrames"))};
    CFRelease(raw);
    return result;
}
std::vector<AudioObjectID>
objects(AudioObjectID object, AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const auto location = address(selector, scope);
    UInt32 size = 0, bytes = 0;
    assert((*driver)->GetPropertyDataSize(driver, object, 0, &location, 0, nullptr, &size) ==
           noErr);
    std::vector<AudioObjectID> result(size / sizeof(AudioObjectID));
    assert((*driver)->GetPropertyData(driver, object, 0, &location, 0, nullptr, size, &bytes,
                                      result.data()) == noErr);
    assert(bytes == size);
    return result;
}
std::vector<AudioObjectID> devices() {
    return objects(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList);
}

uint64_t ns(uint64_t ticks) {
    return ticks / timebase.denom * timebase.numer +
           ticks % timebase.denom * timebase.numer / timebase.denom;
}
uint64_t now() {
    return ns(mach_absolute_time());
}

VirtualDeviceDefinition device(std::string id, uint32_t channels, bool playback = true) {
    VirtualDeviceDefinition result;
    result.id = std::move(id);
    result.name = result.id;
    result.routing.outputChannels = channels;
    if (playback) {
        result.routing.sources = {
            {"pass", channels, 1, true, false, defaultChannelConnections(channels, channels)}};
        result.bindings = {{"pass", SourceKind::PassThrough, "", false}};
    }
    return result;
}
void publish(DeviceTransportPool& pool, const RouteDocument& document) {
    auto pending = pool.prepare(document);
    assert(applySnapshot(encodeDeviceTransports(pending.devices(), ++revision)) == noErr);
    assert(result(revision).phase == Phase::Committed);
    pool.commit(std::move(pending));
}
std::vector<DriverRateRequest> clientRates() {
    return routing::rateRequests(pendingRates());
}
void acknowledge(std::span<const DriverRateRequest> requests) {
    assert(acknowledgeRates(routing::encodeRateRequests(requests)) == noErr);
}
template <typename T>
OSStatus set(AudioObjectID object, AudioObjectPropertySelector selector, const T& value) {
    const auto location = address(selector);
    return (*driver)->SetPropertyData(driver, object, 0, &location, 0, nullptr, sizeof(value),
                                      &value);
}
void nativeClientRateProperties() {
    DeviceTransportPool pool;
    RouteDocument desired{kRouteDocumentVersion, {device("client-rate", 8)}};
    hostReply.store(HostReply::InlineApproval);
    publish(pool, desired);
    const auto endpoint = devices()[0];
    const auto streams = objects(endpoint, kAudioDevicePropertyStreams);
    assert(streams.size() == 2);
    auto nominal = address(kAudioDevicePropertyNominalSampleRate);
    Boolean writable = false;
    assert((*driver)->IsPropertySettable(driver, endpoint, 0, &nominal, &writable) == noErr &&
           writable);
    auto available = address(kAudioDevicePropertyAvailableNominalSampleRates);
    UInt32 bytes = 0;
    assert((*driver)->GetPropertyDataSize(driver, endpoint, 0, &available, 0, nullptr, &bytes) ==
               noErr &&
           bytes == kVirtualSampleRates.size() * sizeof(AudioValueRange));
    std::array<AudioValueRange, kVirtualSampleRates.size() + 1> complete{};
    complete.back() = {123, 456};
    assert((*driver)->GetPropertyData(driver, endpoint, 0, &available, 0, nullptr,
                                      kVirtualSampleRates.size() * sizeof(AudioValueRange), &bytes,
                                      complete.data()) == noErr);
    assert(bytes == kVirtualSampleRates.size() * sizeof(AudioValueRange) &&
           complete.back().mMinimum == 123);
    for (size_t index = 0; index < kVirtualSampleRates.size(); ++index)
        assert(complete[index].mMinimum == kVirtualSampleRates[index] &&
               complete[index].mMaximum == kVirtualSampleRates[index]);
    std::array<AudioValueRange, 3> ranges{{{}, {}, {123, 456}}};
    assert((*driver)->GetPropertyData(driver, endpoint, 0, &available, 0, nullptr,
                                      2 * sizeof(AudioValueRange), &bytes, ranges.data()) == noErr);
    assert(bytes == 2 * sizeof(AudioValueRange) && ranges[0].mMinimum == 44100 &&
           ranges[0].mMaximum == 44100 && ranges[1].mMinimum == 48000 &&
           ranges[1].mMaximum == 48000 && ranges[2].mMinimum == 123);
    assert((*driver)->GetPropertyData(driver, endpoint, 0, &available, 0, nullptr,
                                      sizeof(AudioValueRange) - 1, &bytes,
                                      ranges.data()) == noErr &&
           !bytes);
    assert((*driver)->GetPropertyData(driver, endpoint, 0, &available, 0, nullptr, 0, &bytes,
                                      ranges.data()) == noErr &&
           !bytes);
    assert(clientRates().empty());
    for (const auto rate : kVirtualSampleRates) {
        assert(set(endpoint, nominal.mSelector, double(rate)) == noErr);
        const auto request = clientRates();
        assert(request.size() == 1 && request[0].sampleRate == rate);
        assert(property<Float64>(endpoint, nominal.mSelector) == 48000);
        acknowledge(request);
        assert(clientRates().empty());
    }
    for (double invalid : {0.0, 44099.0, 44100.5, 96001.0, double(NAN), double(INFINITY)})
        assert(set(endpoint, nominal.mSelector, invalid) == kAudioDeviceUnsupportedFormatError);
    assert(set(endpoint, nominal.mSelector, UInt32(44100)) == kAudioHardwareBadPropertySizeError);
    assert(clientRates().empty());
    assert(set(endpoint, nominal.mSelector, 44100.0) == noErr);
    auto first = clientRates();
    assert(first.size() == 1 && first[0].device == "client-rate" && first[0].sampleRate == 44100);
    assert(property<Float64>(endpoint, nominal.mSelector) == 48000);
    const auto malformed = [&](auto mutation) {
        auto array = routing::encodeRateRequests(first);
        mutation(array, xpc_array_get_value(array, 0));
        assert(acknowledgeRates(array) != noErr);
        assert(clientRates()[0].sequence == first[0].sequence);
    };
    malformed([](auto, auto item) {
        xpc_dictionary_set_uint64(item, "sequence", 0);
    });
    malformed([](auto, auto item) {
        xpc_dictionary_set_int64(item, "sequence", 1);
    });
    malformed([](auto, auto item) {
        xpc_dictionary_set_uint64(item, "sampleRate", 96001);
    });
    malformed([](auto, auto item) {
        xpc_dictionary_set_string(item, "origin", "not-a-uuid");
    });
    malformed([](auto, auto item) {
        xpc_dictionary_set_string(item, "device", "");
    });
    malformed([](auto, auto item) {
        xpc_dictionary_set_bool(item, "extra", true);
    });
    malformed([](auto array, auto item) {
        xpc_array_append_value(array, item);
    });
    malformed([](auto array, auto item) {
        for (size_t i = 0; i < kMaximumVirtualDevices; ++i)
            xpc_array_append_value(array, item);
    });

    for (const auto stream : streams) {
        for (const auto selector : {kAudioStreamPropertyAvailablePhysicalFormats,
                                    kAudioStreamPropertyAvailableVirtualFormats}) {
            auto location = address(selector);
            std::array<AudioStreamRangedDescription, kVirtualSampleRates.size()> fullFormats{};
            assert((*driver)->GetPropertyDataSize(driver, stream, 0, &location, 0, nullptr,
                                                  &bytes) == noErr &&
                   bytes == sizeof(fullFormats));
            assert((*driver)->GetPropertyData(driver, stream, 0, &location, 0, nullptr,
                                              sizeof(fullFormats), &bytes,
                                              fullFormats.data()) == noErr &&
                   bytes == sizeof(fullFormats));
            for (size_t index = 0; index < kVirtualSampleRates.size(); ++index)
                assert(fullFormats[index].mFormat.mSampleRate == kVirtualSampleRates[index] &&
                       fullFormats[index].mSampleRateRange.mMinimum == kVirtualSampleRates[index] &&
                       fullFormats[index].mSampleRateRange.mMaximum == kVirtualSampleRates[index]);
        }
        const auto selector = kAudioStreamPropertyAvailablePhysicalFormats;
        auto location = address(selector);
        std::array<AudioStreamRangedDescription, 2> formats{};
        assert((*driver)->GetPropertyData(driver, stream, 0, &location, 0, nullptr, sizeof(formats),
                                          &bytes, formats.data()) == noErr &&
               bytes == sizeof(formats));
        assert(formats[0].mFormat.mSampleRate == 44100 && formats[1].mFormat.mSampleRate == 48000 &&
               formats[0].mFormat.mChannelsPerFrame == 8);
        auto format =
            property<AudioStreamBasicDescription>(stream, kAudioStreamPropertyPhysicalFormat);
        auto invalid = format;
        invalid.mFormatFlags ^= kAudioFormatFlagIsNonInterleaved;
        assert(set(stream, kAudioStreamPropertyPhysicalFormat, invalid) ==
               kAudioDeviceUnsupportedFormatError);
        format.mSampleRate = 48000;
        assert(set(stream, kAudioStreamPropertyVirtualFormat, format) == noErr);
    }
    auto latest = clientRates();
    assert(latest.size() == 1 && latest[0].sequence > first[0].sequence &&
           latest[0].sampleRate == 48000);
    acknowledge(first);
    assert(clientRates()[0].sequence == latest[0].sequence);
    auto wrongOrigin = latest;
    wrongOrigin[0].origin = "00000000-0000-0000-0000-000000000000";
    acknowledge(wrongOrigin);
    assert(!clientRates().empty());

    // Native format publication swaps mappings without losing a later client request.
    desired.devices[0].sampleRate = 44100;
    auto change = pool.prepare(desired);
    assert(applySnapshot(encodeDeviceTransports(change.devices(), ++revision)) == noErr);
    waitPhase(revision, Phase::Committed);
    pool.commit(std::move(change));
    assert(property<Float64>(endpoint, nominal.mSelector) == 44100 &&
           clientRates()[0].sequence == latest[0].sequence);
    acknowledge(latest);
    assert(clientRates().empty());
    assert(set(endpoint, nominal.mSelector, 48000.0) == noErr);
    publish(pool, {kRouteDocumentVersion, {}});
    assert(clientRates().empty());
    writable = true;
    assert((*driver)->IsPropertySettable(driver, endpoint, 0, &nominal, &writable) ==
           kAudioHardwareBadObjectError);
}
AudioObjectID lookup(CFStringRef uid) {
    auto location = address(kAudioPlugInPropertyTranslateUIDToDevice);
    AudioObjectID value = 999;
    UInt32 bytes = 0;
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &location, sizeof(uid),
                                      &uid, sizeof(value), &bytes, &value) == noErr);
    return value;
}

struct Client {
    AudioObjectID device;
    AudioServerPlugInClientInfo info;
    Client(AudioObjectID object, UInt32 id) : device(object), info{id, 0, true, nullptr} {
        assert((*driver)->AddDeviceClient(driver, device, &info) == noErr);
        assert((*driver)->StartIO(driver, device, id) == noErr);
    }
    ~Client() {
        assert((*driver)->StopIO(driver, device, info.mClientID) == noErr);
        assert((*driver)->RemoveDeviceClient(driver, device, &info) == noErr);
    }
};

void publicationAndIO() {
    DeviceTransportPool pool;
    auto studio = device("studio", 8);
    auto call = device("calls", 64);
    call.sampleRate = 44100;
    RouteDocument document{kRouteDocumentVersion, {studio, call, device("capture-only", 2, false)}};
    publish(pool, document);
    const auto list = devices();
    assert(list.size() == 3 && list[0] != list[1]);
    // Zoom queries transport on the input scope while classifying devices.
    // A global-only answer makes a valid microphone disappear from its list.
    for (const auto object : list) {
        for (const auto scope : {kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeInput,
                                 kAudioObjectPropertyScopeOutput}) {
            assert(property<UInt32>(object, kAudioDevicePropertyTransportType, scope) ==
                   kAudioDeviceTransportTypeVirtual);
        }
    }
    const auto timing = driverTiming;
    const auto custom = property<std::array<AudioServerPlugInCustomPropertyInfo, 2>>(
        list[0], kAudioObjectPropertyCustomPropertyInfoList);
    assert(custom[0].mSelector == kLoopZDevicePropertyIOTiming &&
           custom[1].mSelector == kLoopZDevicePropertyCallbackTiming);
    for (const auto& info : custom)
        assert(info.mPropertyDataType == kAudioServerPlugInCustomPropertyDataTypeCFPropertyList &&
               info.mQualifierDataType == kAudioServerPlugInCustomPropertyDataTypeNone);
    auto timingAddress = address(kLoopZDevicePropertyIOTiming);
    Boolean timingSettable = true;
    assert((*driver)->IsPropertySettable(driver, list[0], 0, &timingAddress, &timingSettable) ==
               noErr &&
           !timingSettable);
    const auto initialTiming = timing(list[0]);
    assert(!initialTiming.running && !initialTiming.measured && initialTiming.session == 0);
    assert(callbackTiming(list[0]).session == 0);
    auto callbackAddress = address(kLoopZDevicePropertyCallbackTiming);
    assert((*driver)->IsPropertySettable(driver, list[0], 0, &callbackAddress, &timingSettable) ==
               noErr &&
           !timingSettable);
    assert(notifications.load() >= 2);
    const auto noticesBeforeNoOp = notifications.load();
    publish(pool, document);
    assert(notifications.load() == noticesBeforeNoOp && devices() == list);
    assert(objects(kAudioObjectPlugInObject, kAudioObjectPropertyOwnedObjects) == list);
    const auto studioUid = property<CFStringRef>(list[0], kAudioDevicePropertyDeviceUID);
    assert(lookup(studioUid) == list[0]);

    {
        Client studioFirst(list[0], 101), studioSecond(list[0], 102), calls(list[1], 201);
        assert((*driver)->StartIO(driver, list[0], 101) == noErr); // Nested IO reference.
        assert((*driver)->StopIO(driver, list[0], 101) == noErr);
        assert(property<UInt32>(list[0], kAudioDevicePropertyDeviceIsRunning) == 1);
        for (size_t index = 0; index < list.size(); ++index) {
            const auto inputs =
                objects(list[index], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
            const auto outputs =
                objects(list[index], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
            assert(inputs.size() == 1 && outputs.size() == (index == 2 ? 0 : 1));
            const auto format =
                property<AudioStreamBasicDescription>(inputs[0], kAudioStreamPropertyVirtualFormat);
            assert(format.mChannelsPerFrame == document.devices[index].routing.outputChannels);
            assert(format.mBytesPerFrame == format.mChannelsPerFrame * sizeof(float));
            assert(format.mSampleRate == document.devices[index].sampleRate);
            assert(property<UInt32>(list[index], kAudioDevicePropertyZeroTimeStampPeriod) >= 10923);
            Boolean supported = true, inPlace = false;
            assert((*driver)->WillDoIOOperation(driver, list[index], 0,
                                                kAudioServerPlugInIOOperationWriteMix, &supported,
                                                &inPlace) == noErr);
            assert(supported == (index != 2) && inPlace);
            if (index == 2)
                continue;
            Float64 sample = 0;
            UInt64 host = 0, seed = 0;
            assert((*driver)->GetZeroTimeStamp(driver, list[index], 0, &sample, &host, &seed) ==
                   noErr);
            assert(seed && host);
            const auto frame = hostFrame(ns(host), format.mSampleRate);
            auto local = pool.devices()[index].region->view();
            local.header().helperOnline.store(1);
            local.header().helperHeartbeatNs.store(now());
            constexpr uint32_t frames = 64;
            std::array<float, frames * 64> source{}, output{};
            const auto count = frames * format.mChannelsPerFrame;
            for (uint32_t item = 0; item < count; ++item)
                source[item] = static_cast<float>(item % 64 + 1) / 128;
            assert(local.writeCapture(frame, std::span(source).first(count), frames));
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mInputTime.mSampleTime = sample;
            cycle.mOutputTime.mSampleTime = sample;
            cycle.mNominalIOBufferFrameSize = index == 0 ? 512 : frames;
            const auto client = index == 0 ? 101 : 201;
            assert((*driver)->DoIOOperation(driver, list[index], inputs[0], client,
                                            kAudioServerPlugInIOOperationReadInput, frames, &cycle,
                                            output.data(), nullptr) == noErr);
            assert(std::equal(output.begin(), output.begin() + count, source.begin()));
            assert(!timing(list[index]).measured); // Reads do not claim playback timing.
            if (!index) { // A second client reads the same history non-destructively.
                output.fill(0);
                assert((*driver)->DoIOOperation(driver, list[0], inputs[0], 102,
                                                kAudioServerPlugInIOOperationReadInput, frames,
                                                &cycle, output.data(), nullptr) == noErr);
                assert(std::equal(output.begin(), output.begin() + count, source.begin()));
            }
            assert((*driver)->DoIOOperation(driver, list[index], outputs[0], client,
                                            kAudioServerPlugInIOOperationWriteMix, frames, &cycle,
                                            source.data(), nullptr) == noErr);
            const auto observedTiming = timing(list[index]);
            assert(observedTiming.running && observedTiming.measured && observedTiming.session &&
                   observedTiming.bufferFrames == cycle.mNominalIOBufferFrameSize);
            const auto callback = callbackTiming(list[index]);
            assert(callback.session == observedTiming.session &&
                   callback.maximumWriteFrames == frames &&
                   callback.maximumNominalFrames == cycle.mNominalIOBufferFrameSize);
            cycle.mNominalIOBufferFrameSize = 32; // Actual 64 frames still bound the callback.
            assert((*driver)->DoIOOperation(driver, list[index], outputs[0], client,
                                            kAudioServerPlugInIOOperationWriteMix, frames, &cycle,
                                            source.data(), nullptr) == noErr);
            assert(timing(list[index]).bufferFrames == observedTiming.bufferFrames);
            const auto retainedCallback = callbackTiming(list[index]);
            assert(retainedCallback.session == callback.session &&
                   retainedCallback.maximumWriteFrames == frames &&
                   retainedCallback.maximumNominalFrames == callback.maximumNominalFrames);
            output.fill(0);
            assert(local.readPlayback(frame, std::span(output).first(count), frames, now())
                       .missingFrames == 0);
            assert(std::equal(output.begin(), output.begin() + count, source.begin()));
            const auto runningSeed = seed;
            document.devices[index].name += " renamed";
            publish(pool, document);
            assert(devices() == list);
            assert((*driver)->GetZeroTimeStamp(driver, list[index], 0, &sample, &host, &seed) ==
                   noErr);
            assert(seed == runningSeed);
            assert(timing(list[index]).session == observedTiming.session &&
                   timing(list[index]).bufferFrames == observedTiming.bufferFrames);
        }

        Float64 studioSample, callSample;
        UInt64 studioHost, callHost, studioSeed, callSeed;
        assert((*driver)->GetZeroTimeStamp(driver, list[0], 101, &studioSample, &studioHost,
                                           &studioSeed) == noErr);
        assert((*driver)->GetZeroTimeStamp(driver, list[1], 201, &callSample, &callHost,
                                           &callSeed) == noErr);
        assert((*driver)->StopIO(driver, list[1], 201) == noErr);
        const auto stoppedTiming = timing(list[1]);
        assert(!stoppedTiming.running && stoppedTiming.measured &&
               stoppedTiming.bufferFrames == 64);
        assert((*driver)->StartIO(driver, list[1], 201) == noErr);
        const auto restartedTiming = timing(list[1]);
        assert(restartedTiming.running && !restartedTiming.measured &&
               restartedTiming.session == stoppedTiming.session + 1);
        const auto resetCallback = callbackTiming(list[1]);
        assert(resetCallback.session == restartedTiming.session &&
               !resetCallback.maximumWriteFrames && !resetCallback.maximumNominalFrames);
        UInt64 restartedSeed;
        assert((*driver)->GetZeroTimeStamp(driver, list[1], 201, &callSample, &callHost,
                                           &restartedSeed) == noErr);
        assert(restartedSeed != callSeed);
        assert((*driver)->GetZeroTimeStamp(driver, list[0], 101, &studioSample, &studioHost,
                                           &restartedSeed) == noErr);
        assert(restartedSeed == studioSeed);

        // A changed channel layout needs a format handshake. Rejection preserves the
        // old catalog and clock rather than changing an active callback's stride.
        DeviceTransportPool restarted;
        auto incompatible = document;
        resizeDeviceOutputs(incompatible, "studio", 16);
        restarted.apply(incompatible);
        assert(restarted.devices()[0].region->view().generation() ==
               pool.devices()[0].region->view().generation());
        assert(restarted.devices()[0].region->view().mappingIdentity() !=
               pool.devices()[0].region->view().mappingIdentity());
        assert(applySnapshot(encodeDeviceTransports(restarted.devices(), ++revision)) == noErr);
        assert(waitPhase(revision, Phase::Aborted).error ==
               kAudioHardwareUnsupportedOperationError);

        auto changed = document;
        resizeDeviceOutputs(changed, "studio", 16);
        auto pending = pool.prepare(changed);
        assert(applySnapshot(encodeDeviceTransports(pending.devices(), ++revision)) == noErr);
        waitPhase(revision, Phase::Aborted);
        assert(devices() == list);
        const auto input =
            objects(list[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)[0];
        assert(property<AudioStreamBasicDescription>(input, kAudioStreamPropertyVirtualFormat)
                   .mChannelsPerFrame == 8);
        assert(pool.devices()[0].region->header().helperOnline.load());
        assert(!pool.devices()[0].region->header().retired.load());

        removeDevice(document, "studio");
        publish(pool, document);
        assert(devices().size() == 2 && lookup(studioUid) == kAudioObjectUnknown);
        assert(property<UInt32>(list[0], kAudioDevicePropertyDeviceIsAlive) == 0);
        std::array<float, 8 * 64 + 2> guarded;
        guarded.fill(99);
        AudioServerPlugInIOCycleInfo cycle{};
        assert((*driver)->DoIOOperation(driver, list[0], input, 101,
                                        kAudioServerPlugInIOOperationReadInput, 64, &cycle,
                                        guarded.data() + 1, nullptr) == noErr);
        assert(guarded.front() == 99 && guarded.back() == 99);
        for (size_t index = 1; index < guarded.size() - 1; ++index)
            assert(guarded[index] == 0);
        assert((*driver)->StartIO(driver, list[0], 103) != noErr);
    } // Old attached clients can still stop and unregister after removal.

    document.devices.push_back(studio);
    publish(pool, document);
    assert(lookup(studioUid) != kAudioObjectUnknown && lookup(studioUid) != list[0]);
    CFRelease(studioUid);
    publish(pool, {});
    assert(devices().empty());
}

void distinctCallbackDimensions() {
    DeviceTransportPool pool;
    RouteDocument document{kRouteDocumentVersion, {device("callback-facts", 2)}};
    publish(pool, document);
    const auto object = devices().at(0);
    const auto stream =
        objects(object, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput).at(0);
    {
        Client first(object, 631), second(object, 632);
        const auto write = [&](uint32_t client, uint32_t frames, uint32_t nominal) {
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mOutputTime.mFlags = kAudioTimeStampHostTimeValid;
            cycle.mOutputTime.mHostTime = mach_absolute_time();
            cycle.mNominalIOBufferFrameSize = nominal;
            std::array<float, 128 * 2> samples{};
            assert((*driver)->DoIOOperation(driver, object, stream, client,
                                            kAudioServerPlugInIOOperationWriteMix, frames, &cycle,
                                            samples.data(), nullptr) == noErr);
        };
        write(631, 64, 128);
        auto value = callbackTiming(object);
        assert(value.maximumWriteFrames == 64 && value.maximumNominalFrames == 128);
        assert(driverTiming(object).bufferFrames == 128);
        write(632, 128, 64);
        value = callbackTiming(object);
        assert(value.maximumWriteFrames == 128 && value.maximumNominalFrames == 128);
        const auto session = value.session;
        assert((*driver)->StopIO(driver, object, 631) == noErr);
        assert(callbackTiming(object).session == session);
        assert((*driver)->StopIO(driver, object, 632) == noErr);
        assert((*driver)->StartIO(driver, object, 631) == noErr);
        value = callbackTiming(object);
        assert(value.session == session + 1 && !value.maximumWriteFrames &&
               !value.maximumNominalFrames);
        write(631, 128, 64);
        value = callbackTiming(object);
        assert(value.maximumWriteFrames == 128 && value.maximumNominalFrames == 64);
        assert(driverTiming(object).bufferFrames == 128);
        assert((*driver)->StartIO(driver, object, 632) == noErr);
        assert(callbackTiming(object).session == value.session);
    }
    publish(pool, {});
}

void concurrentRename() {
    DeviceTransportPool pool;
    RouteDocument document{kRouteDocumentVersion, {device("concurrent", 64)}};
    publish(pool, document);
    const auto object = devices()[0];
    const auto input =
        objects(object, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)[0];
    {
        Client client(object, 301);
        Float64 sample;
        UInt64 host, seed;
        assert((*driver)->GetZeroTimeStamp(driver, object, 301, &sample, &host, &seed) == noErr);
        const auto frame = hostFrame(ns(host), 48000);
        const auto local = pool.devices()[0].region->view();
        std::array<float, 64 * 64> payload{};
        payload.fill(0.25f);
        assert(local.writeCapture(frame, payload, 64));
        local.header().helperOnline.store(1);
        std::atomic<bool> finished{false};
        std::atomic<uint64_t> calls{0};
        std::jthread audio([&] {
            std::array<float, 64 * 64> output{};
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mInputTime.mSampleTime = sample;
            do {
                local.header().helperHeartbeatNs.store(now());
                assert((*driver)->DoIOOperation(driver, object, input, 301,
                                                kAudioServerPlugInIOOperationReadInput, 64, &cycle,
                                                output.data(), nullptr) == noErr);
                for (float value : output)
                    assert(value == 0.25f);
                ++calls;
            } while (!finished.load());
        });
        for (uint32_t index = 0; index < 100; ++index) {
            document.devices[0].name = "Mix " + std::to_string(index);
            auto pending = pool.prepare(document);
            const auto message = encodeDeviceTransports(pending.devices(), ++revision);
            OSStatus status;
            uint32_t retries = 0;
            do {
                status = applySnapshot(message);
                if (status == kAudioHardwareNotReadyError)
                    std::this_thread::yield();
            } while (status == kAudioHardwareNotReadyError && ++retries < 10000);
            assert(status == noErr);
            pool.commit(std::move(pending));
        }
        finished.store(true);
        audio.join();
        assert(calls.load() > 0 && devices()[0] == object);
    }
    publish(pool, {});
}

void propertyBoundsAndCapacity() {
    DeviceTransportPool pool;
    RouteDocument document;
    for (uint32_t index = 0; index < kMaximumVirtualDevices; ++index)
        document.devices.push_back(device(std::to_string(index), 2));
    publish(pool, document);
    const auto list = devices();
    assert(list.size() == kMaximumVirtualDevices);
    auto location = address(kAudioPlugInPropertyDeviceList);
    std::array<AudioObjectID, 3> guarded{999, 999, 999};
    UInt32 bytes = 0;
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &location, 0, nullptr,
                                      sizeof(AudioObjectID), &bytes, guarded.data() + 1) == noErr);
    assert(bytes == sizeof(AudioObjectID) && guarded[0] == 999 && guarded[1] == list[0] &&
           guarded[2] == 999);
    location = address(kAudioDevicePropertyNominalSampleRate);
    assert((*driver)->GetPropertyData(driver, list[0], 0, &location, 0, nullptr, sizeof(UInt32),
                                      &bytes,
                                      guarded.data()) == kAudioHardwareBadPropertySizeError);
    assert(guarded[0] == 999);
    location.mScope = kAudioObjectPropertyScopeInput;
    assert(!(*driver)->HasProperty(driver, list[0], 0, &location));
    location = address(kAudioDevicePropertyDeviceIsAlive);
    location.mElement = 999;
    assert(!(*driver)->HasProperty(driver, list[0], 0, &location));
    assert(applySnapshot(encodeDeviceTransports(pool.devices(), revision)) ==
           kAudioHardwareNotReadyError);
    publish(pool, {});
}

uint32_t channelCount(AudioObjectID object) {
    const auto input =
        objects(object, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)[0];
    return property<AudioStreamBasicDescription>(input, kAudioStreamPropertyVirtualFormat)
        .mChannelsPerFrame;
}
PreparedDeviceTransports stage(DeviceTransportPool& pool, const RouteDocument& document,
                               std::span<const uint32_t> latencies = {}) {
    auto prepared = pool.prepare(document, latencies);
    const auto message = encodeDeviceTransports(prepared.devices(), ++revision);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    OSStatus status;
    do {
        status = applySnapshot(message);
        if (status == noErr)
            return prepared;
        assert(status == kAudioHardwareNotReadyError);
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    std::abort();
}
void carrier(AudioObjectID object, DeviceAudioView view, bool silent = false,
             bool sampleTimeOnly = false) {
    constexpr uint32_t frames = 64;
    Float64 sample;
    UInt64 host, seed;
    assert((*driver)->GetZeroTimeStamp(driver, object, 0, &sample, &host, &seed) == noErr);
    const auto padding = sampleTimeOnly ? 1U : 0U;
    std::vector<float> input((frames + 2 * padding) * view.channels(), 0.25f);
    std::vector<float> output(frames * view.channels(), -1);
    assert(view.writeCapture(hostFrame(ns(host), view.sampleRate()) - padding, input,
                             frames + 2 * padding));
    view.header().helperOnline.store(1);
    view.header().helperHeartbeatNs.store(now());
    AudioServerPlugInIOCycleInfo cycle{};
    cycle.mInputTime.mSampleTime = sample;
    cycle.mInputTime.mHostTime = host;
    cycle.mInputTime.mFlags = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
    if (sampleTimeOnly)
        cycle.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
    const auto stream =
        objects(object, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)[0];
    assert((*driver)->DoIOOperation(driver, object, stream, 0,
                                    kAudioServerPlugInIOOperationReadInput, frames, &cycle,
                                    output.data(), nullptr) == noErr);
    for (float value : output)
        assert(value == (silent ? 0 : 0.25f));
}

uint64_t clockOrigin(Float64 sample, UInt64 host, uint32_t rate) {
    const auto offset = frameNs(static_cast<uint64_t>(sample), rate);
    return host - (offset / timebase.numer * timebase.denom +
                   offset % timebase.numer * timebase.denom / timebase.numer);
}
std::pair<UInt64, UInt64> clockIdentity(AudioObjectID object, uint32_t rate) {
    Float64 sample;
    UInt64 host, seed;
    assert((*driver)->GetZeroTimeStamp(driver, object, 0, &sample, &host, &seed) == noErr);
    return {seed, clockOrigin(sample, host, rate)};
}

void allRateClockAndIO() {
    DeviceTransportPool pool;
    const auto previousReply = hostReply.exchange(HostReply::InlineApproval);
    for (const auto channels : {2U, 8U, 64U})
        for (const auto rate : kVirtualSampleRates) {
            auto definition = device("all-rate-io", channels);
            definition.sampleRate = rate;
            auto prepared = pool.prepare({kRouteDocumentVersion, {definition}});
            assert(applySnapshot(encodeDeviceTransports(prepared.devices(), ++revision)) == noErr);
            waitPhase(revision, Phase::Committed);
            pool.commit(std::move(prepared));
            const auto object = devices()[0];
            const auto view = pool.devices()[0].region->view();
            AudioServerPlugInClientInfo client{991, 0, true, nullptr};
            assert((*driver)->AddDeviceClient(driver, object, &client) == noErr);
            assert((*driver)->StartIO(driver, object, client.mClientID) == noErr);
            carrier(object, view);
            Float64 firstSample, nextSample;
            UInt64 firstHost, nextHost, firstSeed, nextSeed;
            assert((*driver)->GetZeroTimeStamp(driver, object, 0, &firstSample, &firstHost,
                                               &firstSeed) == noErr);
            const auto period = property<UInt32>(object, kAudioDevicePropertyZeroTimeStampPeriod);
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(uint64_t(period) * 1000000000ULL / rate + 1000000));
            assert((*driver)->GetZeroTimeStamp(driver, object, 0, &nextSample, &nextHost,
                                               &nextSeed) == noErr);
            assert(nextSeed == firstSeed && nextSample > firstSample && nextHost > firstHost);
            const auto expected = (nextSample - firstSample) * 1e9L / rate;
            const auto observed = static_cast<long double>(ns(nextHost)) - ns(firstHost);
            const auto resolution = 2.L * timebase.numer / timebase.denom + 2;
            assert(std::abs(observed - expected) <= resolution);
            carrier(object, view);
            assert((*driver)->StopIO(driver, object, client.mClientID) == noErr);
            assert((*driver)->RemoveDeviceClient(driver, object, &client) == noErr);
        }
    publish(pool, {kRouteDocumentVersion, {}});
    hostReply.store(previousReply);
}

void negotiatedRatePropertiesPreserveActiveClock() {
    DeviceTransportPool pool;
    auto definition = device("negotiated-rate-properties", 2);
    definition.sampleRate = 192000;
    publish(pool, {kRouteDocumentVersion, {definition}});
    const auto object = devices()[0];
    AudioServerPlugInClientInfo client{993, 0, true, nullptr};
    assert((*driver)->AddDeviceClient(driver, object, &client) == noErr);
    assert((*driver)->StartIO(driver, object, client.mClientID) == noErr);
    const auto before = clockIdentity(object, definition.sampleRate);
    const auto requestsBefore = configurationRequests.load();
    assert(set(object, kAudioDevicePropertyNominalSampleRate, 96000.0) == noErr);
    assert(xpc_array_get_count(pendingRates()) == 1);
    assert(rateCapabilities(kLegacyVirtualSampleRates) == noErr);
    assert(clockIdentity(object, definition.sampleRate) == before);
    assert(property<Float64>(object, kAudioDevicePropertyNominalSampleRate) == 192000);
    assert(property<UInt32>(object, kAudioDevicePropertyDeviceIsRunning) == 1);
    assert(set(object, kAudioDevicePropertyNominalSampleRate, 96000.0) ==
           kAudioDeviceUnsupportedFormatError);
    assert(xpc_array_get_count(pendingRates()) == 0); // Retained until compatible peers return.
    const auto available = address(kAudioDevicePropertyAvailableNominalSampleRates);
    UInt32 bytes;
    assert((*driver)->GetPropertyDataSize(driver, object, 0, &available, 0, nullptr, &bytes) ==
           noErr);
    assert(bytes ==
           3 * sizeof(AudioValueRange)); // Legacy choices plus the unchanged active format.
    std::array<AudioValueRange, 4> ranges{};
    ranges.back() = {-1, -1};
    assert((*driver)->GetPropertyData(driver, object, 0, &available, 0, nullptr, sizeof(ranges),
                                      &bytes, ranges.data()) == noErr);
    assert(bytes == 3 * sizeof(AudioValueRange) && ranges.back().mMinimum == -1);
    assert(ranges[0].mMinimum == 44100 && ranges[1].mMinimum == 48000 &&
           ranges[2].mMinimum == 192000);
    for (const auto stream : objects(object, kAudioDevicePropertyStreams))
        for (const auto selector : {kAudioStreamPropertyAvailableVirtualFormats,
                                    kAudioStreamPropertyAvailablePhysicalFormats}) {
            const auto propertyAddress = address(selector);
            assert((*driver)->GetPropertyDataSize(driver, stream, 0, &propertyAddress, 0, nullptr,
                                                  &bytes) == noErr);
            assert(bytes == 3 * sizeof(AudioStreamRangedDescription));
        }
    auto unsupported = definition;
    unsupported.sampleRate = 96000;
    auto prepared = pool.prepare({kRouteDocumentVersion, {unsupported}});
    assert(applySnapshot(encodeDeviceTransports(prepared.devices(), ++revision)) ==
           kAudioDeviceUnsupportedFormatError);
    for (const auto invalid : {0U, 1U, 128U, UINT32_MAX})
        assert(rateCapabilities(invalid) != noErr);
    assert(rateCapabilities(kAllVirtualSampleRates) == noErr);
    assert(xpc_array_get_count(pendingRates()) == 1);
    assert(acknowledgeRates(pendingRates()) == noErr);
    assert(configurationRequests.load() == requestsBefore);
    assert(clockIdentity(object, definition.sampleRate) == before);
    carrier(object, pool.devices()[0].region->view());
    assert((*driver)->StopIO(driver, object, client.mClientID) == noErr);
    assert((*driver)->RemoveDeviceClient(driver, object, &client) == noErr);
    publish(pool, {kRouteDocumentVersion, {}});
}

void sharedClientReferencesAndRemoval() {
    DeviceTransportPool pool;
    publish(pool, {kRouteDocumentVersion, {device("shared-io-references", 2)}});
    const auto object = devices()[0];
    AudioServerPlugInClientInfo first{741, 0, true, nullptr};
    AudioServerPlugInClientInfo second{742, 0, true, nullptr};
    AudioServerPlugInClientInfo prepared{743, 0, true, nullptr};
    for (const auto* info : {&first, &second, &prepared})
        assert((*driver)->AddDeviceClient(driver, object, info) == noErr);
    {
        std::lock_guard guard(runningMutex);
        runningNotifications.clear();
    }
    assert(property<UInt32>(object, kAudioDevicePropertyDeviceIsRunning) == 0);
    assert((*driver)->StartIO(driver, object, first.mClientID) == noErr);
    const auto clock = clockIdentity(object, 48000);
    const auto session = driverTiming(object).session;
    assert((*driver)->StartIO(driver, object, first.mClientID) == noErr);
    assert((*driver)->StartIO(driver, object, second.mClientID) == noErr);
    assert((*driver)->StopIO(driver, object, first.mClientID) == noErr);
    assert(clockIdentity(object, 48000) == clock && driverTiming(object).session == session);
    // A vanished client can be removed with a live reference. Its peer keeps
    // running; the prepared but idle registration never contributes demand.
    assert((*driver)->RemoveDeviceClient(driver, object, &first) == noErr);
    assert(property<UInt32>(object, kAudioDevicePropertyDeviceIsRunning) == 1);
    assert(pool.devices()[0].region->header().driverOnline.load());
    assert(clockIdentity(object, 48000) == clock);
    carrier(object, pool.devices()[0].region->view());
    assert((*driver)->StopIO(driver, object, second.mClientID) == noErr);
    assert(property<UInt32>(object, kAudioDevicePropertyDeviceIsRunning) == 0);
    assert(!pool.devices()[0].region->header().driverOnline.load());
    assert((*driver)->StopIO(driver, object, second.mClientID) != noErr);
    assert((*driver)->StopIO(driver, object, first.mClientID) != noErr);
    assert((*driver)->RemoveDeviceClient(driver, object, &second) == noErr);
    assert((*driver)->StartIO(driver, object, prepared.mClientID) == noErr);
    assert(driverTiming(object).session > session);
    assert(clockIdentity(object, 48000).first > clock.first);
    // Removing the last client releases all its outstanding references once.
    assert((*driver)->StartIO(driver, object, prepared.mClientID) == noErr);
    assert((*driver)->RemoveDeviceClient(driver, object, &prepared) == noErr);
    assert(property<UInt32>(object, kAudioDevicePropertyDeviceIsRunning) == 0);
    {
        std::lock_guard guard(runningMutex);
        const std::vector<std::pair<AudioObjectID, UInt32>> expected{
            {object, 1}, {object, 0}, {object, 1}, {object, 0}};
        assert(runningNotifications == expected);
    }
    publish(pool, {});
}
void playbackVisibilityPreservesInputClock() {
    DeviceTransportPool pool;
    RouteDocument document{kRouteDocumentVersion,
                           {device("visibility-clock", 2), device("visibility-peer", 2)}};
    publish(pool, document);
    const auto list = devices();
    hostReply.store(HostReply::Defer);
    {
        Client input(list[0], 760), peer(list[1], 761);
        const auto clock = clockIdentity(list[0], 48000);
        const auto peerClock = clockIdentity(list[1], 48000);
        const auto inputStreams =
            objects(list[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
        const auto timing = driverTiming(list[0]);
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            const bool playback = cycle % 2;
            document.devices[0].routing.sources[0].enabled = playback;
            auto prepared = stage(pool, document);
            assert(prepared.devices()[0].region == pool.devices()[0].region);
            assert(clockIdentity(list[0], 48000) == clock);
            approve(nextRequest());
            waitPhase(revision, Phase::Committed);
            pool.commit(std::move(prepared));
            assert(clockIdentity(list[0], 48000) == clock);
            assert(clockIdentity(list[1], 48000) == peerClock);
            assert(objects(list[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput) ==
                   inputStreams);
            assert(objects(list[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput)
                       .size() == (playback ? 1 : 0));
            assert(driverTiming(list[0]).session == timing.session);
            carrier(list[0], pool.devices()[0].region->view(), false, true);
        }
        // A later device rejecting its format change must roll back the
        // visibility-only prefix without resetting the surviving input clock.
        auto rejected = document;
        rejected.devices[0].routing.sources[0].enabled = false;
        resizeDeviceOutputs(rejected, "visibility-peer", 8);
        auto pending = stage(pool, rejected);
        const auto transaction = revision;
        approve(nextRequest());
        assert(clockIdentity(list[0], 48000) == clock);
        // This device's input has already received host approval. Waiting
        // for a different device must not mute its unchanged capture mapping.
        carrier(list[0], pool.devices()[0].region->view(), false, true);
        assert((*driver)->StopIO(driver, list[0], 760) == noErr);
        assert((*driver)->StartIO(driver, list[0], 760) == noErr);
        const auto rollbackClock = clockIdentity(list[0], 48000);
        reject(nextRequest());
        // The visibility prefix remains usable while rollback approval is
        // pending; the input storage and timing have not changed either way.
        carrier(list[0], pool.devices()[0].region->view(), false, true);
        approve(nextRequest());
        waitPhase(transaction, Phase::Aborted);
        assert(clockIdentity(list[0], 48000) == rollbackClock);
        assert(clockIdentity(list[1], 48000) == peerClock);
        assert(devices() == list);
        carrier(list[0], pool.devices()[0].region->view(), false, true);

        // The same public visibility change with fresh storage is different:
        // its producer cannot run until the whole transaction commits. Do not
        // expose that tentative mapping while the peer still awaits approval.
        {
            DeviceTransportPool replacementPool;
            auto replacement = stage(replacementPool, rejected);
            const auto freshTransaction = revision;
            assert(replacement.devices()[0].region != pool.devices()[0].region);
            approve(nextRequest());
            carrier(list[0], replacement.devices()[0].region->view(), true, true);
            reject(nextRequest());
            approve(nextRequest());
            waitPhase(freshTransaction, Phase::Aborted);
            carrier(list[0], pool.devices()[0].region->view(), false, true);
            assert(clockIdentity(list[1], 48000) == peerClock);
        }

        // A real stop/start while approval is pending owns a newer clock.
        // Visibility publication must inherit that clock, not the one saved
        // when the request was prepared.
        document.devices[0].routing.sources[0].enabled = false;
        auto restarted = stage(pool, document);
        assert((*driver)->StopIO(driver, list[0], 760) == noErr);
        assert((*driver)->StartIO(driver, list[0], 760) == noErr);
        const auto latestClock = clockIdentity(list[0], 48000);
        assert(latestClock.first > clock.first);
        approve(nextRequest());
        waitPhase(revision, Phase::Committed);
        pool.commit(std::move(restarted));
        assert(clockIdentity(list[0], 48000) == latestClock);
        carrier(list[0], pool.devices()[0].region->view(), false, true);

        // Genuine input format changes retain their original clock-reset
        // behavior; preserving an old slope/stride here would be unsafe.
        resizeDeviceOutputs(document, "visibility-clock", 8);
        auto wider = stage(pool, document);
        approve(nextRequest());
        waitPhase(revision, Phase::Committed);
        pool.commit(std::move(wider));
        const auto wideClock = clockIdentity(list[0], 48000);
        assert(wideClock.first > latestClock.first);
        carrier(list[0], pool.devices()[0].region->view(), false, true);
        document.devices[0].sampleRate = 44100;
        auto rate = stage(pool, document);
        approve(nextRequest());
        waitPhase(revision, Phase::Committed);
        pool.commit(std::move(rate));
        const auto rateClock = clockIdentity(list[0], 44100);
        assert(rateClock.first > wideClock.first);
        const std::array<uint32_t, 2> latency{65, 0};
        auto timingChange = stage(pool, document, latency);
        approve(nextRequest());
        waitPhase(revision, Phase::Committed);
        pool.commit(std::move(timingChange));
        assert(clockIdentity(list[0], 44100).first > rateClock.first);
        carrier(list[0], pool.devices()[0].region->view(), false, true);
    }
    publish(pool, {});
    hostReply.store(HostReply::Reject);
}

void transportHandoffPreservesClients() {
    auto pool = std::make_unique<DeviceTransportPool>();
    RouteDocument document{kRouteDocumentVersion,
                           {device("handoff-stereo", 2), device("handoff-wide", 64),
                            device("handoff-input", 8, false)}};
    document.devices[2].sampleRate = 44100;
    publish(*pool, document);
    const auto list = devices();
    {
        Client stereo(list[0], 701), wide(list[1], 702), input(list[2], 703);
        struct Clock {
            Float64 sample;
            UInt64 host, seed, anchor;
        };
        auto clock = [&](size_t index) {
            Clock value{};
            assert((*driver)->GetZeroTimeStamp(driver, list[index], 0, &value.sample, &value.host,
                                               &value.seed) == noErr);
            value.anchor =
                clockOrigin(value.sample, value.host, document.devices[index].sampleRate);
            return value;
        };
        std::array<Clock, 3> before{};
        for (size_t index = 0; index < list.size(); ++index) {
            before[index] = clock(index);
            carrier(list[index], pool->devices()[index].region->view());
        }
        std::array<float, 128> playback{};
        AudioServerPlugInIOCycleInfo playbackCycle{};
        playbackCycle.mOutputTime.mHostTime = before[0].host;
        playbackCycle.mOutputTime.mFlags = kAudioTimeStampHostTimeValid;
        playbackCycle.mNominalIOBufferFrameSize = 512;
        const auto playbackStream =
            objects(list[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput)[0];
        assert((*driver)->DoIOOperation(driver, list[0], playbackStream, 701,
                                        kAudioServerPlugInIOOperationWriteMix, 64, &playbackCycle,
                                        playback.data(), nullptr) == noErr);
        const auto initialTiming = driverTiming(list[0]);
        assert(initialTiming.measured && initialTiming.bufferFrames == 512);
        const auto requestsBefore = configurationRequests.load();
        const auto noticesBefore = notifications.load();
        std::array<AudioObjectID, 3> streams{};
        for (size_t index = 0; index < list.size(); ++index)
            streams[index] = objects(list[index], kAudioDevicePropertyStreams,
                                     kAudioObjectPropertyScopeInput)[0];
        std::atomic<uint32_t> observations{0};
        std::latch firstRead{1};
        std::jthread reader([&](std::stop_token stop) {
            std::array<float, 64 * 64> samples{};
            while (!stop.stop_requested()) {
                for (size_t index = 0; index < list.size(); ++index) {
                    const auto time = clock(index);
                    assert(time.seed == before[index].seed && time.anchor == before[index].anchor);
                    AudioServerPlugInIOCycleInfo cycle{};
                    cycle.mInputTime.mSampleTime = time.sample;
                    cycle.mInputTime.mHostTime = time.host;
                    cycle.mInputTime.mFlags =
                        kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
                    assert((*driver)->DoIOOperation(driver, list[index], streams[index], 0,
                                                    kAudioServerPlugInIOOperationReadInput, 64,
                                                    &cycle, samples.data(), nullptr) == noErr);
                    for (size_t sample = 0;
                         sample < 64 * document.devices[index].routing.outputChannels; ++sample)
                        assert(samples[sample] == 0 || samples[sample] == 0.25f);
                }
                if (!observations.fetch_add(1))
                    firstRead.count_down();
            }
        });
        firstRead.wait();
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            assert(serviceAvailability(false) == noErr);
            for (size_t index = 0; index < list.size(); ++index) {
                assert(property<UInt32>(list[index], kAudioDevicePropertyDeviceIsAlive) == 1);
                assert(property<UInt32>(list[index], kAudioDevicePropertyDeviceIsRunning) == 1);
                carrier(list[index], pool->devices()[index].region->view(), true);
                pool->devices()[index].region->retire();
            }
            auto replacement = std::make_unique<DeviceTransportPool>();
            replacement->apply(document);
            assert(applySnapshot(encodeDeviceTransports(replacement->devices(), ++revision)) ==
                   noErr);
            waitPhase(revision, Phase::Committed);
            assert(serviceAvailability(true) == noErr);
            assert(devices() == list);
            assert(configurationRequests.load() == requestsBefore);
            assert(notifications.load() == noticesBefore);
            const auto reboundTiming = driverTiming(list[0]);
            assert(reboundTiming.running && reboundTiming.session == initialTiming.session &&
                   reboundTiming.bufferFrames == initialTiming.bufferFrames);
            for (size_t index = 0; index < list.size(); ++index) {
                const auto after = clock(index);
                assert(after.seed == before[index].seed && after.anchor == before[index].anchor);
                assert(after.sample >= before[index].sample && after.host >= before[index].host);
                carrier(list[index], replacement->devices()[index].region->view());
                carrier(list[index], replacement->devices()[index].region->view(), false, true);
            }
            pool = std::move(replacement);
        }
        reader.request_stop();
        reader.join();
        assert(observations.load() > 0);
    }
    publish(*pool, {});
}

void mixedTransportHandoff() {
    auto pool = std::make_unique<DeviceTransportPool>();
    RouteDocument document{kRouteDocumentVersion,
                           {device("mixed-stable", 2), device("mixed-format", 8)}};
    publish(*pool, document);
    const auto list = devices();
    {
        Client stable(list[0], 711), changed(list[1], 712);
        carrier(list[0], pool->devices()[0].region->view());
        auto desired = document;
        resizeDeviceOutputs(desired, "mixed-format", 16);
        auto replacement = std::make_unique<DeviceTransportPool>();
        replacement->apply(desired);
        hostReply.store(HostReply::Defer);
        assert(applySnapshot(encodeDeviceTransports(replacement->devices(), ++revision)) == noErr);
        const auto waiting = nextRequest();
        assert(waiting.device == list[1] && result(revision).phase == Phase::Pending);
        assert(!replacement->devices()[0].region->header().driverOnline.load());
        carrier(list[0], pool->devices()[0].region->view());

        // A client's genuine stop/start while another device awaits approval
        // advances its clock. The pending rebinding must adopt that latest clock.
        assert((*driver)->StopIO(driver, list[0], 711) == noErr);
        assert((*driver)->StartIO(driver, list[0], 711) == noErr);
        const auto latest = clockIdentity(list[0], 48000);
        approve(waiting);
        waitPhase(revision, Phase::Committed);
        assert(devices() == list && channelCount(list[1]) == 16);
        assert(clockIdentity(list[0], 48000) == latest);
        carrier(list[0], replacement->devices()[0].region->view());
        pool = std::move(replacement);
        document = desired;

        auto refused = document;
        refused.devices[0].name += " uncommitted";
        resizeDeviceOutputs(refused, "mixed-format", 32);
        replacement = std::make_unique<DeviceTransportPool>();
        replacement->apply(refused);
        assert(applySnapshot(encodeDeviceTransports(replacement->devices(), ++revision)) == noErr);
        const auto rejected = nextRequest();
        const auto notices = notifications.load();
        reject(rejected);
        waitPhase(revision, Phase::Aborted);
        assert(devices() == list && channelCount(list[1]) == 16);
        assert(clockIdentity(list[0], 48000) == latest);
        assert(notifications.load() == notices);
        assert(!replacement->devices()[0].region->header().driverOnline.load());
        const auto name = property<CFStringRef>(list[0], kAudioObjectPropertyName);
        assert(CFEqual(name, CFSTR("mixed-stable")));
        CFRelease(name);
        carrier(list[0], pool->devices()[0].region->view());
        hostReply.store(HostReply::Reject);
    }
    publish(*pool, {});
}

class ContinuousReader {
  public:
    ContinuousReader(AudioObjectID object, DeviceAudioView view) {
        Float64 sample;
        UInt64 host, seed;
        assert((*driver)->GetZeroTimeStamp(driver, object, 0, &sample, &host, &seed) == noErr);
        const auto stream =
            objects(object, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)[0];
        std::array<float, 64 * 64> input{};
        input.fill(0.25f);
        const auto count = 64 * view.channels();
        assert(view.writeCapture(hostFrame(ns(host), view.sampleRate()),
                                 std::span(input).first(count), 64));
        view.header().helperOnline.store(1);
        worker_ = std::jthread([=, this] {
            std::array<float, 64 * 64> output{};
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mInputTime.mSampleTime = sample;
            cycle.mInputTime.mHostTime = host;
            cycle.mInputTime.mFlags = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
            do {
                view.header().helperHeartbeatNs.store(now());
                assert((*driver)->DoIOOperation(driver, object, stream, 0,
                                                kAudioServerPlugInIOOperationReadInput, 64, &cycle,
                                                output.data(), nullptr) == noErr);
                for (uint32_t index = 0; index < count; ++index)
                    assert(output[index] == 0.25f);
                if (!observations_.fetch_add(1))
                    first_.count_down();
            } while (!finished_.load());
        });
        first_.wait();
    }
    ~ContinuousReader() {
        finished_.store(true);
        worker_.join();
        assert(observations_.load() > 0);
    }

  private:
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> observations_{0};
    std::latch first_{1};
    std::jthread worker_;
};

void nativeLatencyProperties() {
    DeviceTransportPool pool;
    RouteDocument document{
        kRouteDocumentVersion,
        {device("latency-a", 2), device("latency-b", 8), device("latency-untouched", 2)}};
    const std::array<uint32_t, 3> originalLatencies{129, 1025, 129};
    auto initial = stage(pool, document, originalLatencies);
    waitPhase(revision, Phase::Committed);
    pool.commit(std::move(initial));
    const auto list = devices();
    const auto latency = [&](size_t index) {
        return property<UInt32>(list[index], kAudioDevicePropertyLatency,
                                kAudioObjectPropertyScopeInput);
    };
    for (size_t index = 0; index < list.size(); ++index) {
        assert(latency(index) == originalLatencies[index]);
        assert(property<UInt32>(list[index], kAudioDevicePropertyLatency,
                                kAudioObjectPropertyScopeOutput) == 0);
        assert(property<UInt32>(list[index], kAudioDevicePropertySafetyOffset,
                                kAudioObjectPropertyScopeInput) == 0);
        for (const auto stream : objects(list[index], kAudioDevicePropertyStreams))
            assert(property<UInt32>(stream, kAudioStreamPropertyLatency) == 0);
    }
    auto location = address(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
    Boolean writable = true;
    assert((*driver)->IsPropertySettable(driver, list[0], 0, &location, &writable) == noErr &&
           !writable);
    UInt32 bytes = 0, value = 0;
    assert((*driver)->GetPropertyData(driver, list[0], 0, &location, 0, nullptr, 3, &bytes,
                                      &value) == kAudioHardwareBadPropertySizeError);
    location.mScope = kAudioObjectPropertyScopeGlobal;
    assert(!(*driver)->HasProperty(driver, list[0], 0, &location));
    auto aliased = pool.devices();
    aliased[0].inputLatencyFrames = 257;
    assert(applySnapshot(encodeDeviceTransports(aliased, ++revision)) ==
           kAudioHardwareIllegalOperationError);
    assert(latency(0) == 129);
    {
        std::lock_guard guard(latencyMutex);
        latencyNotifications.clear();
    }
    hostReply.store(HostReply::Defer);
    const std::array<uint32_t, 3> changedLatencies{257, 941, 129};
    {
        Client first(list[0], 380), second(list[1], 381), untouched(list[2], 382);
        ContinuousReader continuous(list[2], pool.devices()[2].region->view());
        const auto original = pool.devices();
        auto prepared = stage(pool, document, changedLatencies);
        const auto transaction = revision;
        assert(prepared.devices()[0].region != original[0].region &&
               prepared.devices()[1].region != original[1].region &&
               prepared.devices()[2].region == original[2].region);
        assert(latency(0) == 129 && latency(1) == 1025);
        approve(nextRequest());
        assert(latency(0) == 257 && latency(1) == 1025);
        carrier(list[0], prepared.devices()[0].region->view(), true);
        reject(nextRequest());
        assert(result(transaction).phase == Phase::RollingBack);
        approve(nextRequest());
        waitPhase(transaction, Phase::Aborted);
        assert(latency(0) == 129 && latency(1) == 1025);
        carrier(list[0], original[0].region->view());
        assert(!original[0].region->header().retired.load());
        {
            std::lock_guard guard(latencyMutex);
            const std::vector<std::pair<AudioObjectID, UInt32>> expected{{list[0], 257},
                                                                         {list[0], 129}};
            assert(latencyNotifications == expected);
        }
        auto retry = stage(pool, document, changedLatencies);
        const auto accepted = revision;
        approve(nextRequest());
        approve(nextRequest());
        waitPhase(accepted, Phase::Committed);
        pool.commit(std::move(retry));
        assert(latency(0) == 257 && latency(1) == 941 && latency(2) == 129);
        assert(original[0].region->header().retired.load() &&
               original[1].region->header().retired.load() &&
               !original[2].region->header().retired.load());
        carrier(list[0], pool.devices()[0].region->view());
        {
            std::lock_guard guard(latencyMutex);
            const std::vector<std::pair<AudioObjectID, UInt32>> expected{
                {list[0], 257}, {list[0], 129}, {list[0], 257}, {list[1], 941}};
            assert(latencyNotifications == expected);
        }
    }
    hostReply.store(HostReply::Reject);
    publish(pool, {});
}

void deferredApprovalAndRollback() {
    DeviceTransportPool pool;
    RouteDocument original{kRouteDocumentVersion,
                           {device("first", 2), device("second", 8), device("untouched", 2)}};
    publish(pool, original);
    const auto list = devices();
    hostReply.store(HostReply::Defer);
    {
        Client first(list[0], 401), second(list[1], 402), untouched(list[2], 403);
        ContinuousReader continuous(list[2], pool.devices()[2].region->view());
        const auto originalFirst = pool.devices()[0].region;
        const auto originalSecond = pool.devices()[1].region;
        carrier(list[0], originalFirst->view());
        carrier(list[1], originalSecond->view());
        auto changed = original;
        resizeDeviceOutputs(changed, "first", 16);
        changed.devices[0].sampleRate = 44100;
        changed.devices[0].routing.sources[0].enabled = false;
        resizeDeviceOutputs(changed, "second", 64);
        changed.devices[2].name = "Unrelated rename";
        changed.devices.push_back(device("added-after-approval", 2));
        {
            auto prepared = stage(pool, changed);
            const auto transaction = revision;
            assert(result(transaction).phase == Phase::Pending);
            assert(devices() == list && channelCount(list[0]) == 2 && channelCount(list[1]) == 8);
            carrier(list[0], originalFirst->view());
            carrier(list[1], originalSecond->view());
            const auto firstRequest = nextRequest();
            assert(firstRequest.device == list[0]);
            assert((*driver)->PerformDeviceConfigurationChange(driver, list[1], firstRequest.action,
                                                               nullptr) != noErr);
            approve(firstRequest);
            assert(channelCount(list[0]) == 16 && channelCount(list[1]) == 8);
            assert(objects(list[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput)
                       .empty());
            carrier(list[0], prepared.devices()[0].region->view(), true);

            assert(!originalFirst->header().retired.load() &&
                   !originalSecond->header().retired.load());
            const auto secondRequest = nextRequest();
            assert(secondRequest.device == list[1]);
            reject(secondRequest);
            assert(result(transaction).phase == Phase::RollingBack);
            assert((*driver)->PerformDeviceConfigurationChange(
                       driver, secondRequest.device, secondRequest.action, nullptr) != noErr);
            assert(cancelConfiguration(transaction) ==
                   noErr); // Repeated cancellation cannot abort rollback.
            const auto rollback = nextRequest();
            assert(rollback.device == list[0] && rollback.action != firstRequest.action);
            approve(rollback);
            waitPhase(transaction, Phase::Aborted);
            assert(channelCount(list[0]) == 2 && channelCount(list[1]) == 8 && devices() == list);
            assert(property<Float64>(list[0], kAudioDevicePropertyNominalSampleRate) == 48000);
            assert((*driver)->PerformDeviceConfigurationChange(driver, list[0], firstRequest.action,
                                                               nullptr) != noErr);
            carrier(list[0], originalFirst->view());
            carrier(list[1], originalSecond->view());
        }
        assert(pool.devices()[0].region == originalFirst &&
               !originalFirst->header().retired.load());
        {
            auto prepared = stage(pool, changed);
            const auto transaction = revision;
            approve(nextRequest());
            assert(result(transaction).phase == Phase::Pending && devices().size() == 3);
            approve(nextRequest());
            waitPhase(transaction, Phase::Committed);
            pool.commit(std::move(prepared));
            assert(devices().size() == 4 && channelCount(list[0]) == 16 &&
                   channelCount(list[1]) == 64);
            assert(originalFirst->header().retired.load() &&
                   originalSecond->header().retired.load());
            assert(pool.devices()[2].id == "untouched");
            carrier(list[0], pool.devices()[0].region->view());
            carrier(list[1], pool.devices()[1].region->view());
        }
    }
    publish(pool, {});
    hostReply.store(HostReply::Reject);
}

void cancelledAndInlineApproval() {
    DeviceTransportPool pool;
    RouteDocument original{kRouteDocumentVersion, {device("cancel", 2), device("inline", 8)}};
    publish(pool, original);
    const auto list = devices();
    auto changed = original;
    resizeDeviceOutputs(changed, "cancel", 8);
    resizeDeviceOutputs(changed, "inline", 16);
    hostReply.store(HostReply::Defer);
    {
        auto prepared = stage(pool, changed);
        const auto transaction = revision;
        const auto queued = nextRequest();
        assert(cancelConfiguration(transaction) == noErr);
        waitPhase(transaction, Phase::Aborted);
        assert(channelCount(list[0]) == 2 && channelCount(list[1]) == 8);
        assert((*driver)->PerformDeviceConfigurationChange(driver, queued.device, queued.action,
                                                           nullptr) != noErr);
        assert(!pool.devices()[0].region->header().retired.load());
    }
    hostReply.store(HostReply::InlineApproval);
    {
        auto prepared = stage(pool, changed);
        waitPhase(revision, Phase::Committed);
        pool.commit(std::move(prepared));
        assert(devices() == list && channelCount(list[0]) == 8 && channelCount(list[1]) == 16);
    }
    publish(pool, {});
    hostReply.store(HostReply::Reject);
}

enum class RollbackFailure { HostAbort, RequestError, Timeout };
void failedRollbackAndRecovery(bool visibilityOnly,
                               RollbackFailure failure = RollbackFailure::HostAbort) {
    DeviceTransportPool pool;
    RouteDocument original{kRouteDocumentVersion,
                           {device("quarantine", 2), device("preserved", 8)}};
    publish(pool, original);
    const auto list = devices();
    const auto uid = property<CFStringRef>(list[0], kAudioDevicePropertyDeviceUID);
    hostReply.store(HostReply::Defer);
    {
        Client first(list[0], 501), preserved(list[1], 502);
        auto changed = original;
        if (visibilityOnly)
            changed.devices[0].routing.sources[0].enabled = false;
        else
            resizeDeviceOutputs(changed, "quarantine", 16);
        resizeDeviceOutputs(changed, "preserved", 64);
        {
            auto prepared = stage(pool, changed);
            const auto transaction = revision;
            auto late = nextRequest();
            approve(late);
            const auto second = nextRequest();
            if (failure == RollbackFailure::RequestError)
                hostReply.store(HostReply::Reject);
            reject(second);
            if (failure != RollbackFailure::RequestError) {
                late = nextRequest();
                if (failure == RollbackFailure::HostAbort)
                    reject(late);
            }
            const auto outcome =
                waitPhase(transaction, Phase::Quarantined,
                          failure == RollbackFailure::Timeout ? std::chrono::seconds(15)
                                                              : std::chrono::seconds(3));
            if (failure == RollbackFailure::RequestError)
                assert(outcome.error == kAudioHardwareUnsupportedOperationError);
            if (failure == RollbackFailure::Timeout)
                assert(outcome.error == kAudioHardwareNotReadyError);
            assert(devices().size() == 1 && devices()[0] == list[1]);
            assert(lookup(uid) == kAudioObjectUnknown && channelCount(list[1]) == 8);
            assert(prepared.devices()[0].region->header().retired.load());
            assert(pool.devices()[0].region->header().retired.load() == visibilityOnly);
            carrier(list[1], pool.devices()[1].region->view());
            assert((*driver)->PerformDeviceConfigurationChange(driver, late.device, late.action,
                                                               nullptr) != noErr);
        }
        // A fresh native object restores only the quarantined device; no global
        // audio-service restart or unrelated-device format transition is needed.
        const auto oldIdentity = pool.devices()[0].region->view().mappingIdentity();
        publish(pool, original);
        const auto recovered = lookup(uid);
        assert(recovered && recovered != list[0] && devices()[1] == list[1]);
        if (visibilityOnly)
            assert(pool.devices()[0].region->view().mappingIdentity() != oldIdentity);
        Client recovery(recovered, 503);
        carrier(recovered, pool.devices()[0].region->view());
        carrier(list[1], pool.devices()[1].region->view());
    }
    CFRelease(uid);
    publish(pool, {});
    hostReply.store(HostReply::Reject);
}

void approvalTimeout() {
    DeviceTransportPool pool;
    RouteDocument original{kRouteDocumentVersion, {device("timeout", 2)}};
    publish(pool, original);
    const auto object = devices()[0];
    Client active(object, 601);
    hostReply.store(HostReply::Defer);
    auto changed = original;
    resizeDeviceOutputs(changed, "timeout", 8);
    {
        auto prepared = stage(pool, changed);
        const auto transaction = revision;
        const auto late = nextRequest();
        const auto outcome = waitPhase(transaction, Phase::Aborted, std::chrono::seconds(15));
        assert(outcome.error == kAudioHardwareNotReadyError);
        assert(channelCount(object) == 2 && !pool.devices()[0].region->header().retired.load());
        carrier(object, pool.devices()[0].region->view());
        assert((*driver)->PerformDeviceConfigurationChange(driver, late.device, late.action,
                                                           nullptr) != noErr);
    }
    publish(pool, {});
    hostReply.store(HostReply::Reject);
}

void rollbackPrefixIsolation() {
    DeviceTransportPool pool;
    RouteDocument original{kRouteDocumentVersion,
                           {device("unsafe", 2), device("restored", 2), device("not-applied", 2)}};
    publish(pool, original);
    const auto list = devices();
    hostReply.store(HostReply::Defer);
    {
        Client a(list[0], 701), b(list[1], 702), c(list[2], 703);
        auto changed = original;
        resizeDeviceOutputs(changed, "unsafe", 8);
        resizeDeviceOutputs(changed, "restored", 16);
        resizeDeviceOutputs(changed, "not-applied", 64);
        auto prepared = stage(pool, changed);
        const auto transaction = revision;
        approve(nextRequest());
        approve(nextRequest());
        reject(nextRequest());
        const auto restoreSecond = nextRequest();
        assert(restoreSecond.device == list[1]);
        approve(restoreSecond);
        assert(channelCount(list[1]) == 2);
        carrier(list[1], pool.devices()[1].region->view());
        reject(nextRequest());
        waitPhase(transaction, Phase::Quarantined);
        assert(devices() == std::vector<AudioObjectID>({list[1], list[2]}));
        assert(prepared.devices()[0].region->header().retired.load());
        assert(!prepared.devices()[1].region->header().retired.load());
        carrier(list[1], pool.devices()[1].region->view());
        carrier(list[2], pool.devices()[2].region->view());
    }
    publish(pool, {});
    hostReply.store(HostReply::Reject);
}

void maximumFormatTransaction() {
    DeviceTransportPool pool;
    RouteDocument original;
    for (uint32_t index = 0; index < kMaximumVirtualDevices; ++index)
        original.devices.push_back(device("format-" + std::to_string(index), 2));
    publish(pool, original);
    const auto list = devices();
    {
        Client first(list.front(), 801), last(list.back(), 802);
        auto wide = original;
        for (const auto& item : original.devices)
            resizeDeviceOutputs(wide, item.id, 4);
        hostReply.store(HostReply::InlineApproval);
        {
            auto prepared = stage(pool, wide);
            waitPhase(revision, Phase::Committed);
            pool.commit(std::move(prepared));
            assert(devices() == list);
            for (auto object : list)
                assert(channelCount(object) == 4);
        }
        hostReply.store(HostReply::Defer);
        {
            auto prepared = stage(pool, original);
            const auto transaction = revision;
            for (size_t index = 0; index + 1 < list.size(); ++index) {
                const auto approval = nextRequest();
                assert(approval.device == list[index]);
                approve(approval);
            }
            reject(nextRequest());
            for (size_t remaining = list.size() - 1; remaining > 0; --remaining) {
                const auto rollback = nextRequest();
                assert(rollback.device == list[remaining - 1]);
                approve(rollback);
            }
            waitPhase(transaction, Phase::Aborted);
            assert(devices() == list);
            for (auto object : list)
                assert(channelCount(object) == 4);
            carrier(list.front(), pool.devices().front().region->view());
            carrier(list.back(), pool.devices().back().region->view());
        }
    }
    publish(pool, {});
    hostReply.store(HostReply::Reject);
}
class LocalConfigurationLink final : public DriverConfigurationLink {
  public:
    uint64_t identity{1};
    uint64_t session() const noexcept override {
        return identity;
    }
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override {
        return applySnapshot(encodeDeviceTransports(devices, revision));
    }
    DriverConfigurationResult status(uint64_t revision) override {
        return result(revision);
    }
    int32_t cancel(uint64_t revision) override {
        return cancelConfiguration(revision);
    }
    void invalidate() noexcept override {
        identity = 0;
    }
};
void pollController(ConfigurationController& controller,
                    const std::function<bool(const RestorationStatus&)>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        controller.poll(now() / 1000000);
        if (predicate(controller.status()))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    const auto state = controller.status();
    std::fprintf(stderr, "Controller phase %u, revision %llu, error %d: %s\n",
                 unsigned(state.phase), static_cast<unsigned long long>(state.transactionRevision),
                 state.error, state.detail.c_str());
    std::abort();
}
void restoreController(ConfigurationController& controller) {
    pollController(controller, [](const auto& state) {
        return state.phase == RestorationPhase::Restored;
    });
}
void automaticDocumentRecovery(bool visibilityOnly) {
    LocalConfigurationLink link;
    auto controller = std::make_unique<ConfigurationController>(link);
    RouteDocument desired{kRouteDocumentVersion,
                          {device("controller-first", 2), device("controller-second", 64),
                           device("controller-independent", 8)}};
    controller->setDesired(desired);
    restoreController(*controller);
    const auto originalObjects = devices();
    assert(originalObjects.size() == 3);
    {
        Client first(originalObjects[0], 901), second(originalObjects[1], 902),
            independent(originalObjects[2], 903);
        const auto original = controller->devices();
        auto reader =
            std::make_unique<ContinuousReader>(originalObjects[2], original[2].region->view());
        // A host rejection preserves the complete intent and the old mapping,
        // then succeeds automatically when the host becomes available.
        resizeDeviceOutputs(desired, desired.devices[0].id, 4);
        hostReply.store(HostReply::Reject);
        controller->setDesired(desired);
        pollController(*controller, [](const auto& state) {
            return state.phase == RestorationPhase::Retry;
        });
        assert(controller->devices()[0].region == original[0].region);
        assert(!original[0].region->header().retired.load());
        assert(controller->desired()->devices[0].routing.outputChannels == 4);
        hostReply.store(HostReply::InlineApproval);
        restoreController(*controller);
        assert(devices() == originalObjects && channelCount(originalObjects[0]) == 4);
        assert(original[0].region->header().retired.load());
        assert(controller->devices()[2].region == original[2].region);
        carrier(originalObjects[0], controller->devices()[0].region->view());

        // A newer edit cancels an in-flight approval and cannot be replaced by
        // the older revision's later callback.
        hostReply.store(HostReply::Defer);
        resizeDeviceOutputs(desired, desired.devices[0].id, 8);
        controller->setDesired(desired);
        pollController(*controller, [](const auto& state) {
            return state.phase == RestorationPhase::Applying;
        });
        const auto stale = nextRequest();
        const auto olderRevision = controller->status().transactionRevision;
        resizeDeviceOutputs(desired, desired.devices[0].id, 16);
        controller->setDesired(desired);
        pollController(*controller, [&](const auto&) {
            return result(olderRevision).phase == Phase::Aborted;
        });
        assert((*driver)->PerformDeviceConfigurationChange(driver, stale.device, stale.action,
                                                           nullptr) != noErr);
        hostReply.store(HostReply::InlineApproval);
        restoreController(*controller);
        assert(channelCount(originalObjects[0]) == 16);
        assert(controller->active()->devices[0].routing.outputChannels == 16);

        // Approve one change, reject the next, and fail its rollback. The
        // controller must recreate only the quarantined mapping and eventually
        // restore every device in the saved desired graph without another edit.
        const auto beforeQuarantine = controller->devices();
        hostReply.store(HostReply::Defer);
        if (visibilityOnly)
            desired.devices[0].routing.sources[0].enabled = false;
        else
            resizeDeviceOutputs(desired, desired.devices[0].id, 32);
        resizeDeviceOutputs(desired, desired.devices[1].id, 8);
        controller->setDesired(desired);
        pollController(*controller, [](const auto& state) {
            return state.phase == RestorationPhase::Applying;
        });
        const auto transaction = controller->status().transactionRevision;
        approve(nextRequest());
        reject(nextRequest());
        reject(nextRequest());
        waitPhase(transaction, Phase::Quarantined);
        assert(beforeQuarantine[0].region->header().retired.load() == visibilityOnly);
        assert(!beforeQuarantine[1].region->header().retired.load());
        assert(!beforeQuarantine[2].region->header().retired.load());
        hostReply.store(HostReply::InlineApproval);
        restoreController(*controller);
        const auto restoredObjects = devices();
        assert(restoredObjects.size() == 3 && restoredObjects[0] != originalObjects[0]);
        assert(restoredObjects[1] == originalObjects[1] &&
               restoredObjects[2] == originalObjects[2]);
        assert(channelCount(restoredObjects[0]) == (visibilityOnly ? 16 : 32));
        assert(channelCount(restoredObjects[1]) == 8);
        assert(objects(restoredObjects[0], kAudioDevicePropertyStreams,
                       kAudioObjectPropertyScopeOutput)
                   .empty() == visibilityOnly);
        assert(controller->devices()[2].region == original[2].region);
        {
            Client restoredFirst(restoredObjects[0], 904);
            carrier(restoredObjects[0], controller->devices()[0].region->view());
        }
        carrier(restoredObjects[1], controller->devices()[1].region->view());
        reader.reset();

        // Simulate controller process reconstruction against the surviving
        // native driver and attached clients. A fresh pool restores all desired
        // devices with new mappings and stable public identities.
        const auto beforeRestart = controller->devices();
        controller.reset();
        for (const auto& item : beforeRestart)
            assert(item.region->header().retired.load());
        controller = std::make_unique<ConfigurationController>(link);
        controller->setDesired(desired);
        restoreController(*controller);
        assert(devices() == restoredObjects && controller->devices().size() == 3);
        for (size_t index = 0; index < beforeRestart.size(); ++index)
            assert(controller->devices()[index].region != beforeRestart[index].region);
        carrier(restoredObjects[1], controller->devices()[1].region->view());
        carrier(restoredObjects[2], controller->devices()[2].region->view());
    }
    controller->setDesired({});
    restoreController(*controller);
    assert(devices().empty());
    revision = result(0).revision;
    hostReply.store(HostReply::Reject);
}
class NativeCaptureFixture final : public SourceEndpoint {
  public:
    bool live{true};
    unsigned opens{0}, closes{0};
    std::atomic<bool> active{false};
    uint32_t channels() const noexcept override {
        return 1;
    }
    bool open(SourceOpenPolicy policy, std::string&) override {
        assert(policy.requiredChannels == 1);
        ++opens;
        active.store(live);
        return live;
    }
    void close() noexcept override {
        ++closes;
        active.store(false);
    }
    bool healthy(std::string&) override {
        return live;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0.02f);
        return {0, active.load()};
    }
};
void nativeRateRuntime() {
    const auto requirement = selfCodeRequirement();
    RoutingBrokerPolicy policy{requirement, requirement, static_cast<uint32_t>(geteuid()), false,
                               []() -> std::optional<uint32_t> {
                                   return geteuid();
                               }};
    auto broker = RoutingBroker::anonymous(std::move(policy));
    RoutingServiceEndpoint endpoint;
    endpoint.endpoint = broker->endpoint();
    endpoint.serverRequirement = requirement;
    endpoint.serverUID = geteuid();
    endpoint.privileged = false;
    RoutingDriverOperations operations{[](xpc_object_t snapshot) -> int32_t {
                                           return applySnapshot(snapshot);
                                       },
                                       [](uint64_t token) {
                                           return result(token);
                                       },
                                       [](uint64_t token) -> int32_t {
                                           return cancelConfiguration(token);
                                       },
                                       [](bool available) {
                                           assert(serviceAvailability(available) == noErr);
                                       },
                                       [] {
                                           return clientRates();
                                       },
                                       [](std::span<const DriverRateRequest> requests) -> int32_t {
                                           return acknowledgeRates(
                                               routing::encodeRateRequests(requests));
                                       }};
    RoutingDriverService service(endpoint, operations);
    RoutingDriverLink link(endpoint);
    char directory[] = "/private/tmp/loopz-rate-runtime-XXXXXX";
    assert(::mkdtemp(directory));
    EngineRuntimeOptions options;
    options.documentPath = std::filesystem::path(directory) / "routes.json";
    EngineRuntime runtime(link, options);
    RouteDocument desired{kRouteDocumentVersion, {device("rate-runtime", 2, false)}};
    runtime.setDesired(desired, true);
    runtime.start();
    auto until = [&](auto predicate) {
        const auto deadline = now() + 10000000000ULL;
        do {
            runtime.poll();
            if (predicate())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (now() < deadline);
        std::fprintf(stderr, "Native rate runtime failed: %s %s\n",
                     runtime.status().controlError.c_str(),
                     runtime.status().configuration.detail.c_str());
        assert(false);
    };
    hostReply.store(HostReply::InlineApproval);
    until([&] {
        return runtime.status().restored;
    });
    const auto object = devices()[0];
    assert(property<UInt32>(object, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) ==
           1);
    assert(set(object, kAudioDevicePropertyNominalSampleRate, 44100.0) == noErr);
    runtime.retryNow();
    until([&] {
        return runtime.status().restored &&
               runtime.observe()->document().devices[0].sampleRate == 44100 &&
               property<Float64>(object, kAudioDevicePropertyNominalSampleRate) == 44100;
    });
    assert(link.rateRequests().empty());
    for (const auto rate : kVirtualSampleRates) {
        assert(set(object, kAudioDevicePropertyNominalSampleRate, double(rate)) == noErr);
        runtime.retryNow();
        until([&] {
            return runtime.status().restored &&
                   runtime.observe()->document().devices[0].sampleRate == rate &&
                   property<Float64>(object, kAudioDevicePropertyNominalSampleRate) == rate;
        });
        assert(link.rateRequests().empty() && devices()[0] == object);
        const auto saved = loadRouteDocumentFile(options.documentPath).document;
        assert(saved.devices[0].sampleRate == rate &&
               !saved.devices[0].formatRequestReceipt.empty());
        for (const auto stream : objects(object, kAudioDevicePropertyStreams))
            assert(property<AudioStreamBasicDescription>(stream, kAudioStreamPropertyVirtualFormat)
                       .mSampleRate == rate);
    }
    // Return to the existing 44.1/48 rejection/rollback case below.
    assert(set(object, kAudioDevicePropertyNominalSampleRate, 44100.0) == noErr);
    runtime.retryNow();
    until([&] {
        return runtime.status().restored &&
               property<Float64>(object, kAudioDevicePropertyNominalSampleRate) == 44100;
    });
    // A real HAL rejection preserves desired intent; retry restores it through approval.
    hostReply.store(HostReply::Reject);
    const auto stream = objects(object, kAudioDevicePropertyStreams)[0];
    auto format = property<AudioStreamBasicDescription>(stream, kAudioStreamPropertyPhysicalFormat);
    format.mSampleRate = 48000;
    assert(set(stream, kAudioStreamPropertyPhysicalFormat, format) == noErr);
    runtime.retryNow();
    until([&] {
        return link.rateRequests().empty() && !runtime.status().configuration.detail.empty() &&
               !runtime.status().restored;
    });
    assert(property<Float64>(object, kAudioDevicePropertyNominalSampleRate) == 44100);
    hostReply.store(HostReply::InlineApproval);
    runtime.retryNow();
    until([&] {
        return runtime.status().restored &&
               runtime.observe()->document().devices[0].sampleRate == 48000 &&
               property<Float64>(object, kAudioDevicePropertyNominalSampleRate) == 48000;
    });
    runtime.setDesired({});
    until([&] {
        return runtime.status().restored && devices().empty();
    });
    runtime.stop();
    service.stop();
    assert(serviceAvailability(true) == noErr);
    revision = result(0).revision;
    hostReply.store(HostReply::Reject);
    std::filesystem::remove_all(directory);
}
void nativePairRemoval() {
    const auto requirement = selfCodeRequirement();
    RoutingBrokerPolicy policy{requirement, requirement, static_cast<uint32_t>(geteuid()), false,
                               []() -> std::optional<uint32_t> {
                                   return geteuid();
                               }};
    auto broker = RoutingBroker::anonymous(std::move(policy));
    RoutingServiceEndpoint endpoint;
    endpoint.endpoint = broker->endpoint();
    endpoint.serverRequirement = requirement;
    endpoint.serverUID = geteuid();
    endpoint.privileged = false;
    RoutingDriverService service(endpoint, {
                                               [](xpc_object_t snapshot) {
                                                   return applySnapshot(snapshot);
                                               },
                                               [](uint64_t token) {
                                                   return result(token);
                                               },
                                               [](uint64_t token) {
                                                   return cancelConfiguration(token);
                                               },
                                               [](bool available) {
                                                   assert(serviceAvailability(available) == noErr);
                                               },
                                           });
    RoutingDriverLink link(endpoint);
    char directory[] = "/private/tmp/loopz-pair-native-XXXXXX";
    assert(mkdtemp(directory));
    const auto path = std::filesystem::path(directory) / "routing.json";
    ConfigurationController controller(link, path, nullptr, false);
    const std::array<edit::Command, 4> setup{
        edit::CreateDevice{"pair-native", "Pair Native", 8},
        edit::CreateDevice{"pair-dependent", "Pair Dependent", 2, 48000, false},
        edit::AddSource{"pair-dependent", "virtual", SourceKind::VirtualDevice, "pair-native"},
        edit::SetSourceWiring{"pair-dependent", "virtual", {{2, 0, 0.5f}, {6, 1, 0.75f}}},
    };
    controller.saveDesired(edit::applyCommands({}, setup));
    const auto previousReply = hostReply.exchange(HostReply::InlineApproval);
    const auto restore = [&] {
        const auto deadline = now() + 10000000000ULL;
        do {
            link.poll(now() / 1000000);
            controller.poll(now() / 1000000);
            if (controller.status().phase == RestorationPhase::Restored)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (now() < deadline);
        std::fprintf(stderr, "Native pair restoration: %s\n", controller.status().detail.c_str());
        assert(false);
    };
    restore();
    const auto objectsBefore = devices();
    assert(objectsBefore.size() == 2);
    const auto dependentMapping = controller.devices()[1].region;
    AudioServerPlugInClientInfo first{1779, 0, true, nullptr}, second{1780, 0, true, nullptr};
    assert((*driver)->AddDeviceClient(driver, objectsBefore[0], &first) == noErr);
    assert((*driver)->AddDeviceClient(driver, objectsBefore[1], &second) == noErr);
    assert((*driver)->StartIO(driver, objectsBefore[0], first.mClientID) == noErr);
    assert((*driver)->StartIO(driver, objectsBefore[1], second.mClientID) == noErr);
    const auto dependentClock = clockIdentity(objectsBefore[1], 48000);
    EditorController editor(controller, "pair-native-editor");
    const auto before = editor.snapshot().revision;
    const std::array<edit::Command, 1> removal{edit::RemoveOutputPair{"pair-native", 2}};
    const auto committed = editor.apply(before, removal);
    assert(committed.changed && committed.revision.generation == before.generation + 1);
    const auto check = [&](uint32_t channels) {
        restore();
        assert(devices() == objectsBefore && controller.devices()[1].region == dependentMapping);
        assert(clockIdentity(objectsBefore[1], 48000) == dependentClock);
        assert(property<UInt32>(objectsBefore[0], kAudioDevicePropertyDeviceIsRunning) == 1);
        const auto stream = objects(objectsBefore[0], kAudioDevicePropertyStreams)[0];
        assert(property<AudioStreamBasicDescription>(stream, kAudioStreamPropertyVirtualFormat)
                   .mChannelsPerFrame == channels);
        const auto saved = loadRouteDocumentFile(path).document;
        assert(saved.devices[0].routing.outputChannels == channels);
        assert(saved.devices[1].routing.sources[0].channels == channels);
        carrier(objectsBefore[0], controller.devices()[0].region->view());
        carrier(objectsBefore[1], controller.devices()[1].region->view());
    };
    check(6);
    const auto saved = loadRouteDocumentFile(path).document;
    assert(saved.devices[1].routing.sources[0].connections.size() == 1);
    assert(saved.devices[1].routing.sources[0].connections[0].sourceChannel == 4 &&
           saved.devices[1].routing.sources[0].connections[0].gain == 0.75f);
    editor.moveHistory(editor.snapshot().revision, EditorHistoryDirection::Undo);
    check(8);
    editor.moveHistory(editor.snapshot().revision, EditorHistoryDirection::Redo);
    check(6);
    assert((*driver)->StopIO(driver, objectsBefore[0], first.mClientID) == noErr);
    assert((*driver)->StopIO(driver, objectsBefore[1], second.mClientID) == noErr);
    assert((*driver)->RemoveDeviceClient(driver, objectsBefore[0], &first) == noErr);
    assert((*driver)->RemoveDeviceClient(driver, objectsBefore[1], &second) == noErr);
    controller.setDesired({});
    restore();
    assert(devices().empty());
    service.stop();
    revision = result(0).revision;
    hostReply.store(previousReply);
    std::filesystem::remove_all(directory);
}

void nativeServiceRecovery() {
    const auto requirement = selfCodeRequirement();
    RoutingBrokerPolicy policy{requirement, requirement, static_cast<uint32_t>(geteuid()), false,
                               []() -> std::optional<uint32_t> {
                                   return geteuid();
                               }};
    auto broker = RoutingBroker::anonymous(std::move(policy));
    RoutingServiceEndpoint endpoint;
    endpoint.endpoint = broker->endpoint();
    endpoint.serverRequirement = requirement;
    endpoint.serverUID = geteuid();
    endpoint.privileged = false;
    RoutingDriverOperations operations{[](xpc_object_t snapshot) -> int32_t {
                                           return applySnapshot(snapshot);
                                       },
                                       [](uint64_t revision) {
                                           return result(revision);
                                       },
                                       [](uint64_t revision) -> int32_t {
                                           return cancelConfiguration(revision);
                                       },
                                       [](bool available) {
                                           assert(serviceAvailability(available) == noErr);
                                       }};
    auto service = std::make_unique<RoutingDriverService>(endpoint, operations);
    RoutingDriverLink link(endpoint);
    auto capture = std::make_shared<NativeCaptureFixture>();
    SourceRegistry sources(
        [&](SourceKind kind, const std::string& selector, uint32_t channels) {
            assert(kind == SourceKind::InputDevice && selector == "fake-native-source" &&
                   channels == 1);
            return capture;
        },
        [] {
            return now() / 1000000;
        });
    SceneRenderer renderer({}, sources);
    ConfigurationController controller(link, {}, &renderer);
    RouteDocument desired{kRouteDocumentVersion,
                          {device("xpc-first", 8), device("xpc-second", 64)}};
    desired.devices[0].routing.sources.push_back({"hardware", 1, 0.5f, true, false, {{0, 0}}});
    desired.devices[0].bindings.push_back(
        {"hardware", SourceKind::InputDevice, "fake-native-source", false});
    controller.setDesired(desired);
    auto restore = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        do {
            link.poll(now() / 1000000);
            controller.poll(now() / 1000000);
            if (controller.status().phase == RestorationPhase::Restored)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        std::fprintf(stderr, "Native XPC restoration failed: %s %s\n",
                     controller.status().detail.c_str(), link.lastError().c_str());
        std::abort();
    };
    hostReply.store(HostReply::InlineApproval);
    restore();
    const auto list = devices();
    assert(list.size() == 2);
    auto renderThroughNativeIO = [&] {
        const auto latencies = renderer.inputLatencyFrames(*controller.active());
        for (size_t index = 0; index < list.size(); ++index) {
            assert(controller.devices()[index].inputLatencyFrames == latencies[index]);
            assert(property<UInt32>(list[index], kAudioDevicePropertyLatency,
                                    kAudioObjectPropertyScopeInput) == latencies[index]);
        }
        assert(latencies == std::vector<uint32_t>({1025, 129}));
        const auto first = hostFrame(now(), 48000) + 512;
        auto timestamp = [&](uint64_t frame) {
            // Center of the frame avoids roundoff while converting ns to Mach ticks.
            const auto time = frameNs(frame, 48000) + 10000;
            AudioTimeStamp stamp{};
            stamp.mHostTime = (time / timebase.numer) * timebase.denom +
                              (time % timebase.numer) * timebase.denom / timebase.numer;
            stamp.mFlags = kAudioTimeStampHostTimeValid;
            assert(hostFrame(ns(stamp.mHostTime), 48000) == frame);
            return stamp;
        };
        // Preseed the complete configured lookback, including the mixed path's
        // capture delay; a short pass-only window cannot represent that history.
        const auto playbackFrames = SceneTiming{}.sourceDelayFrames + 256;
        for (size_t index = 0; index < list.size(); ++index) {
            const auto channels = controller.devices()[index].region->view().channels();
            std::vector<float> playback(playbackFrames * channels);
            for (unsigned frame = 0; frame < playbackFrames; ++frame)
                for (unsigned channel = 0; channel < channels; ++channel)
                    playback[frame * channels + channel] = (channel + 1) * 0.005f;
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mOutputTime = timestamp(first - SceneTiming{}.sourceDelayFrames);
            const auto output = objects(list[index], kAudioDevicePropertyStreams,
                                        kAudioObjectPropertyScopeOutput)[0];
            assert((*driver)->DoIOOperation(driver, list[index], output, 0,
                                            kAudioServerPlugInIOOperationWriteMix, playbackFrames,
                                            &cycle, playback.data(), nullptr) == noErr);
        }
        renderer.render(first, 64, now());
        renderer.pollSources(now() / 1000000);
        const auto rendered = renderer.render(first, 64, now());
        assert(rendered.validBlock && rendered.activeDevices == 2 && !rendered.failedGraphs &&
               !rendered.failedSinks);
        for (size_t index = 0; index < list.size(); ++index) {
            const auto channels = controller.devices()[index].region->view().channels();
            std::vector<float> input(32 * channels, -1);
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mInputTime = timestamp(first + 16);
            const auto stream = objects(list[index], kAudioDevicePropertyStreams,
                                        kAudioObjectPropertyScopeInput)[0];
            assert((*driver)->DoIOOperation(driver, list[index], stream, 0,
                                            kAudioServerPlugInIOOperationReadInput, 32, &cycle,
                                            input.data(), nullptr) == noErr);
            for (unsigned frame = 0; frame < 32; ++frame)
                for (unsigned channel = 0; channel < channels; ++channel)
                    assert(std::fabs(input[frame * channels + channel] -
                                     ((channel + 1) * 0.005f +
                                      (index == 0 && channel == 0 && capture->active.load() ? 0.01f
                                                                                            : 0))) <
                           0.00001f);
        }
        renderer.collect();
    };
    {
        Client first(list[0], 1001), second(list[1], 1002);
        renderThroughNativeIO();
        capture->live = false;
        renderer.pollSources(now() / 1000000);
        assert(!sources.restored());
        renderThroughNativeIO(); // Pass-through and the other device continue.
        capture->live = true;
        sources.retryNow(now() / 1000000);
        renderer.pollSources(now() / 1000000);
        assert(sources.restored());
        renderThroughNativeIO();
        carrier(list[0], controller.devices()[0].region->view());
        carrier(list[1], controller.devices()[1].region->view());
        const auto old = controller.devices();
        resizeDeviceOutputs(desired, desired.devices[0].id, 16);
        controller.setDesired(desired);
        restore();
        assert(channelCount(list[0]) == 16 && controller.devices()[1].region == old[1].region);
        renderThroughNativeIO();
        carrier(list[0], controller.devices()[0].region->view());
        // The first native device moves to its tentative mapping while the
        // second still awaits approval. Keep the old scene's capture alive.
        const auto captureOpens = capture->opens;
        resizeDeviceOutputs(desired, desired.devices[0].id, 8);
        resizeDeviceOutputs(desired, desired.devices[1].id, 16);
        hostReply.store(HostReply::Defer);
        controller.setDesired(desired);
        pollController(controller, [](const auto& state) {
            return state.phase == RestorationPhase::Applying;
        });
        approve(nextRequest());
        const auto waiting = nextRequest();
        assert(result(controller.status().transactionRevision).phase == Phase::Pending);
        assert(!controller.devices()[0].region->header().driverOnline.load());
        // Age out the initial lease. A live, configuration-muted IO callback
        // must renew demand while still returning silence to public clients.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::array<float, 64 * 8> pendingInput;
        pendingInput.fill(-1);
        AudioServerPlugInIOCycleInfo pendingCycle{};
        pendingCycle.mInputTime.mHostTime = mach_absolute_time();
        pendingCycle.mInputTime.mFlags = kAudioTimeStampHostTimeValid;
        const auto pendingStream =
            objects(list[0], kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)[0];
        assert((*driver)->DoIOOperation(driver, list[0], pendingStream, 0,
                                        kAudioServerPlugInIOOperationReadInput, 64, &pendingCycle,
                                        pendingInput.data(), nullptr) == noErr);
        assert(std::all_of(pendingInput.begin(), pendingInput.end(), [](float value) {
            return value == 0;
        }));
        renderer.render(hostFrame(now(), 48000) + 512, 64, now());
        renderer.pollSources(now() / 1000000);
        assert(capture->active.load() && capture->opens == captureOpens);
        approve(waiting);
        restore();
        hostReply.store(HostReply::InlineApproval);
        renderThroughNativeIO();
        assert(capture->opens == captureOpens);
        const auto current = controller.devices();
        service->stop();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (link.session() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(!link.session());
        carrier(list[0], current[0].region->view(), true);
        assert(!current[0].region->header().driverOnline.load());
        renderer.render(hostFrame(now(), 48000) + 512, 64, now());
        renderer.pollSources(now() / 1000000);
        assert(!capture->active.load());
        service = std::make_unique<RoutingDriverService>(endpoint, operations);
        restore();
        assert(devices() == list && controller.devices()[0].region == current[0].region);
        renderThroughNativeIO();
        carrier(list[0], controller.devices()[0].region->view());
        carrier(list[1], controller.devices()[1].region->view());
    }
    controller.setDesired({});
    restore();
    assert(devices().empty());
    service->stop();
    assert(serviceAvailability(true) == noErr); // Restore direct-test host ownership.
    revision = result(0).revision;
    hostReply.store(HostReply::Reject);
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        assert(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--legacy-slot"));
        const bool legacySlot = argc == 3;
        auto* library = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            std::fprintf(stderr, "%s\n", ::dlerror());
            return 1;
        }
        auto factory = reinterpret_cast<void* (*)(CFAllocatorRef, CFUUIDRef)>(
            ::dlsym(library, legacySlot ? "LoopZAudio_Create" : "LoopZRoutingAudio_Create"));
        applySnapshot = reinterpret_cast<Apply>(::dlsym(library, "LoopZRoutingApplySnapshot"));
        configurationStatus =
            reinterpret_cast<Status>(::dlsym(library, "LoopZRoutingConfigurationStatus"));
        cancelConfiguration =
            reinterpret_cast<Cancel>(::dlsym(library, "LoopZRoutingCancelConfiguration"));
        serviceAvailability =
            reinterpret_cast<Availability>(::dlsym(library, "LoopZRoutingServiceAvailability"));
        rateCapabilities = reinterpret_cast<RateCapabilities>(
            ::dlsym(library, "LoopZRoutingSampleRateCapabilities"));
        pendingRates = reinterpret_cast<Rates>(::dlsym(library, "LoopZRoutingRateRequests"));
        acknowledgeRates =
            reinterpret_cast<AcknowledgeRates>(::dlsym(library, "LoopZRoutingAcknowledgeRates"));
        assert(factory && applySnapshot && configurationStatus && cancelConfiguration &&
               serviceAvailability && pendingRates && acknowledgeRates && rateCapabilities);
        assert(!factory(nullptr, IUnknownUUID));
        driver =
            static_cast<AudioServerPlugInDriverRef>(factory(nullptr, kAudioServerPlugInTypeUUID));
        assert(driver);
        AudioServerPlugInHostInterface host{changed, storage, writeStorage, removeStorage, request};
        mach_timebase_info(&timebase);
        assert((*driver)->Initialize(driver, &host) == noErr);
        CFStringRef bundle = nullptr;
        UInt32 bytes = 0;
        const auto bundleAddress = address(kAudioPlugInPropertyBundleID);
        assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &bundleAddress, 0,
                                          nullptr, sizeof(bundle), &bytes, &bundle) == noErr);
        assert(bytes == sizeof(bundle) && bundle);
        assert(CFEqual(bundle, legacySlot ? CFSTR("com.lexasoft.loopz.driver")
                                          : CFSTR("com.lexasoft.loopz.driver.routing")));
        CFRelease(bundle);
        assert(devices().empty());
        publicationAndIO();
        distinctCallbackDimensions();
        allRateClockAndIO();
        negotiatedRatePropertiesPreserveActiveClock();
        sharedClientReferencesAndRemoval();
        playbackVisibilityPreservesInputClock();
        transportHandoffPreservesClients();
        mixedTransportHandoff();
        concurrentRename();
        nativeLatencyProperties();
        deferredApprovalAndRollback();
        cancelledAndInlineApproval();
        failedRollbackAndRecovery(false);
        failedRollbackAndRecovery(true);
        failedRollbackAndRecovery(false, RollbackFailure::RequestError);
        failedRollbackAndRecovery(false, RollbackFailure::Timeout);
        approvalTimeout();
        rollbackPrefixIsolation();
        maximumFormatTransaction();
        automaticDocumentRecovery(false);
        automaticDocumentRecovery(true);
        nativeServiceRecovery();
        nativeClientRateProperties();
        nativeRateRuntime();
        nativePairRemoval();
        propertyBoundsAndCapacity();
        std::puts(
            "routing driver: actual plugin factory, 32-device publication, 8/64-channel IO, "
            "independent clocks, concurrent rename, guarded removal and property bounds passed "
            "plus deferred/inline format approvals, cancellation, timeout, rollback, quarantine "
            "and automatic full-document controller recovery "
            "(fake host; not installed HAL evidence)");
        // The host owns plugin lifetime. No unload while a host could call it.
    }
}
