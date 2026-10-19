#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <dlfcn.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>

namespace {
OSStatus changed(AudioServerPlugInHostRef, AudioObjectID, UInt32,
                 const AudioObjectPropertyAddress*) {
    return noErr;
}
OSStatus storage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* result) {
    *result = nullptr;
    return noErr;
}
OSStatus write(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef) {
    return noErr;
}
OSStatus remove(AudioServerPlugInHostRef, CFStringRef) {
    return noErr;
}
std::atomic<UInt64> requestedAction{0};
OSStatus request(AudioServerPlugInHostRef, AudioObjectID, UInt64 action, void*) {
    requestedAction.store(action);
    return noErr;
}
} // namespace
int main(int argc, char** argv) {
    assert(argc == 2);
    auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    auto factory =
        reinterpret_cast<void* (*)(CFAllocatorRef, CFUUIDRef)>(dlsym(library, "LoopZAudio_Create"));
    assert(factory);
    auto driver =
        static_cast<AudioServerPlugInDriverRef>(factory(nullptr, kAudioServerPlugInTypeUUID));
    assert(driver);
    AudioServerPlugInHostInterface host{changed, storage, write, remove, request};
    assert((*driver)->Initialize(driver, &host) == noErr);
    AudioObjectPropertyAddress address{kAudioPlugInPropertyDeviceList,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectID device = 0;
    UInt32 bytes = 0;
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &address, 0, nullptr,
                                      sizeof(device), &bytes, &device) == noErr &&
           bytes == sizeof(device) && device);
    address.mSelector = kAudioDevicePropertyStreams;
    address.mScope = kAudioObjectPropertyScopeInput;
    AudioObjectID input = 0;
    assert((*driver)->GetPropertyData(driver, device, 0, &address, 0, nullptr, sizeof(input),
                                      &bytes, &input) == noErr &&
           input);
    address.mSelector = kAudioStreamPropertyVirtualFormat;
    address.mScope = kAudioObjectPropertyScopeGlobal;
    AudioStreamBasicDescription format{};
    assert((*driver)->GetPropertyData(driver, input, 0, &address, 0, nullptr, sizeof(format),
                                      &bytes, &format) == noErr);
    assert(format.mChannelsPerFrame == 2 && format.mBitsPerChannel == 32 &&
           format.mSampleRate == 48000);
    // Exercise the actual CFPropertyList marshalling contract and asynchronous
    // device configuration request, independently varying period and read queue.
    constexpr AudioObjectPropertySelector timing = 0x4c5a746d;
    auto configure = [&](UInt32 period, UInt32 queue, UInt32 source) {
        const void* keys[] = {CFSTR("periodFrames"), CFSTR("queueFrames"), CFSTR("sourceFrames")};
        UInt32 values[] = {period, queue, source};
        const void* objects[3];
        for (unsigned i = 0; i < 3; ++i)
            objects[i] = CFNumberCreate(nullptr, kCFNumberSInt32Type, &values[i]);
        CFPropertyListRef config =
            CFDictionaryCreate(nullptr, keys, objects, 3, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        for (auto object : objects)
            CFRelease(object);
        address = {timing, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        requestedAction.store(0);
        auto status = (*driver)->SetPropertyData(driver, device, 0, &address, 0, nullptr,
                                                 sizeof(config), &config);
        CFRelease(config);
        if (status)
            return status;
        for (unsigned retry = 0; retry < 100 && !requestedAction.load(); ++retry)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(requestedAction.load());
        return (*driver)->PerformDeviceConfigurationChange(driver, device, requestedAction.load(),
                                                           nullptr);
    };
    assert(configure(1024, 512, 1024) == noErr);
    address = {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput,
               kAudioObjectPropertyElementMain};
    UInt32 latency = 0;
    assert((*driver)->GetPropertyData(driver, device, 0, &address, 0, nullptr, sizeof(latency),
                                      &bytes, &latency) == noErr &&
           latency == 1536);
    address = {kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal,
               kAudioObjectPropertyElementMain};
    UInt32 period = 0;
    assert((*driver)->GetPropertyData(driver, device, 0, &address, 0, nullptr, sizeof(period),
                                      &bytes, &period) == noErr &&
           period == 1024);
    assert(configure(128, 128, 1024) != noErr);
    assert(configure(129, 512, 1024) != noErr);
    assert(configure(128, 512, 4097) != noErr);
    for (auto source : {0u, 63u, 65u, 193u, 4130u, 4160u})
        assert(configure(128, 0, source) != noErr);
    // Source lookback metadata follows the helper's smaller render grid. The
    // driver read queue keeps its original, independently validated grid.
    for (auto source : {64u, 128u, 192u, 256u, 4096u}) {
        assert(configure(128, 0, source) == noErr);
        address = {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput,
                   kAudioObjectPropertyElementMain};
        assert((*driver)->GetPropertyData(driver, device, 0, &address, 0, nullptr, sizeof(latency),
                                          &bytes, &latency) == noErr &&
               latency == source);
    }
    assert(configure(128, 256, 192) == noErr);
    address = {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput,
               kAudioObjectPropertyElementMain};
    assert((*driver)->GetPropertyData(driver, device, 0, &address, 0, nullptr, sizeof(latency),
                                      &bytes, &latency) == noErr &&
           latency == 448);
    assert(configure(128, 1024, 2048) == noErr);
    address = {timing, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFPropertyListRef config = nullptr;
    assert((*driver)->GetPropertyData(driver, device, 0, &address, 0, nullptr, sizeof(config),
                                      &bytes, &config) == noErr);
    assert(config && CFGetTypeID(config) == CFDictionaryGetTypeID());
    assert(CFEqual(CFDictionaryGetValue((CFDictionaryRef)config, CFSTR("build")),
                   CFSTR("loopz-timing-3")));
    CFRelease(config);
    assert((*driver)->StartIO(driver, device, 1) == noErr);
    assert((*driver)->StartIO(driver, device, 2) == noErr);
    double sampleA = 0, sampleB = 0;
    UInt64 hostA = 0, hostB = 0, seedA = 0, seedB = 0;
    assert((*driver)->GetZeroTimeStamp(driver, device, 1, &sampleA, &hostA, &seedA) == noErr);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert((*driver)->GetZeroTimeStamp(driver, device, 2, &sampleB, &hostB, &seedB) == noErr);
    assert(sampleB >= sampleA && hostB >= hostA && seedA == seedB && seedA);
    AudioServerPlugInIOCycleInfo cycle{};
    std::array<float, 256> samples;
    samples.fill(0.7f);
    assert((*driver)->DoIOOperation(driver, device, input, 1,
                                    kAudioServerPlugInIOOperationReadInput, 128, &cycle,
                                    samples.data(), nullptr) == noErr);
    for (auto sample : samples)
        assert(sample == 0);
    assert((*driver)->StopIO(driver, device, 1) == noErr);
    assert((*driver)->StopIO(driver, device, 2) == noErr);
    std::puts(
        "PASS: actual driver factory, device/stream properties, stereo format, two starts, "
        "monotonic clock, silence without helper, clean stops (fake host; not installed evidence)");
    // The plug-in owns process-lifetime dispatch sources; intentionally do not
    // unload its code while those sources exist.
}
