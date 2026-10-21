#include "state.hpp"
#include "../../routing_service/wire.hpp"
#include <CoreFoundation/CFPlugInCOM.h>

#include <limits>
#include <stdexcept>

namespace {
using loopz::macos::hal::state;
extern AudioServerPlugInDriverInterface interface;
AudioServerPlugInDriverInterface* interfacePointer = &interface;
AudioServerPlugInDriverRef reference = &interfacePointer;
std::atomic<ULONG> references{1};

template <typename Operation> OSStatus control(Operation operation) noexcept {
    try {
        return operation();
    } catch (const std::invalid_argument&) {
        return kAudioHardwareIllegalOperationError;
    } catch (const std::bad_alloc&) {
        return kAudioHardwareUnspecifiedError;
    } catch (...) {
        return kAudioHardwareNotReadyError;
    }
}

ULONG addRef(void* driver) {
    if (driver != reference)
        return 0;
    auto count = references.load();
    while (count < std::numeric_limits<ULONG>::max())
        if (references.compare_exchange_weak(count, count + 1))
            return count + 1;
    return count;
}
ULONG release(void* driver) {
    if (driver != reference)
        return 0;
    auto count = references.load();
    while (count)
        if (references.compare_exchange_weak(count, count - 1))
            return count - 1;
    return 0;
}
HRESULT query(void* driver, REFIID uuid, LPVOID* output) {
    if (driver != reference || !output)
        return E_NOINTERFACE;
    *output = nullptr;
    const auto type = CFUUIDCreateFromUUIDBytes(nullptr, uuid);
    if (!type)
        return E_NOINTERFACE;
    const bool supported =
        CFEqual(type, IUnknownUUID) || CFEqual(type, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(type);
    if (!supported)
        return E_NOINTERFACE;
    addRef(driver);
    *output = reference;
    return S_OK;
}
OSStatus initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
    if (driver != reference)
        return kAudioHardwareBadObjectError;
    return control([&] {
        const auto result = state().initialize(host);
        if (result == noErr)
            loopz::macos::hal::startService();
        return result;
    });
}
OSStatus create(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*,
                AudioObjectID*) {
    // Device lifecycle is owned by the authenticated routing controller.
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus destroy(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus addClient(AudioServerPlugInDriverRef driver, AudioObjectID device,
                   const AudioServerPlugInClientInfo* client) {
    if (driver != reference)
        return kAudioHardwareBadObjectError;
    return control([&] {
        return state().addClient(device, client);
    });
}
OSStatus removeClient(AudioServerPlugInDriverRef driver, AudioObjectID device,
                      const AudioServerPlugInClientInfo* client) {
    if (driver != reference)
        return kAudioHardwareBadObjectError;
    return control([&] {
        return state().removeClient(device, client);
    });
}
OSStatus perform(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt64 action, void*) {
    if (driver != reference)
        return kAudioHardwareBadObjectError;
    return control([&] {
        return state().performChange(device, action);
    });
}
OSStatus abortChange(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt64 action,
                     void*) {
    if (driver != reference)
        return kAudioHardwareBadObjectError;
    return control([&] {
        return state().abortChange(device, action);
    });
}
Boolean has(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t,
            const AudioObjectPropertyAddress* address) {
    if (driver != reference || !address)
        return false;
    return control([&] {
               return state().has(object, *address) ? OSStatus(noErr)
                                                    : kAudioHardwareUnknownPropertyError;
           }) == noErr;
}
OSStatus settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t,
                  const AudioObjectPropertyAddress* address, Boolean* output) {
    if (!output)
        return kAudioHardwareIllegalOperationError;
    *output = false;
    if (driver != reference || !address)
        return kAudioHardwareBadObjectError;
    return control([&] {
        return state().settable(object, *address, *output);
    });
}
OSStatus size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t,
              const AudioObjectPropertyAddress* address, UInt32, const void*, UInt32* bytes) {
    if (driver != reference || !address || !bytes)
        return kAudioHardwareIllegalOperationError;
    *bytes = 0;
    return control([&] {
        return state().size(object, *address, *bytes);
    });
}
OSStatus get(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t,
             const AudioObjectPropertyAddress* address, UInt32 qualifierBytes,
             const void* qualifier, UInt32 capacity, UInt32* bytes, void* output) {
    if (driver != reference || !address || !bytes)
        return kAudioHardwareIllegalOperationError;
    *bytes = 0;
    return control([&] {
        return state().get(object, *address, qualifierBytes, qualifier, capacity, *bytes, output);
    });
}
OSStatus set(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t,
             const AudioObjectPropertyAddress* address, UInt32 qualifierBytes, const void*,
             UInt32 bytes, const void* data) {
    if (driver != reference || !address)
        return kAudioHardwareBadObjectError;
    if (qualifierBytes)
        return kAudioHardwareIllegalOperationError;
    return control([&] {
        return state().set(object, *address, bytes, data);
    });
}
OSStatus start(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client) {
    if (driver != reference)
        return kAudioHardwareBadObjectError;
    return control([&] {
        return state().start(device, client);
    });
}
OSStatus stop(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client) {
    if (driver != reference)
        return kAudioHardwareBadObjectError;
    return control([&] {
        return state().stop(device, client);
    });
}
OSStatus stamp(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32, Float64* sample,
               UInt64* host, UInt64* seed) {
    return driver == reference ? state().stamp(device, sample, host, seed)
                               : kAudioHardwareBadObjectError;
}
OSStatus willDo(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32, UInt32 operation,
                Boolean* output, Boolean* inPlace) {
    return driver == reference ? state().willDo(device, operation, output, inPlace)
                               : kAudioHardwareBadObjectError;
}
OSStatus boundary(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32, UInt32, UInt32,
                  const AudioServerPlugInIOCycleInfo*) {
    return driver == reference && state().knownDevice(device) ? OSStatus(noErr)
                                                              : kAudioHardwareBadDeviceError;
}
OSStatus io(AudioServerPlugInDriverRef driver, AudioObjectID device, AudioObjectID stream, UInt32,
            UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo* cycle,
            void* buffer, void*) {
    return driver == reference ? state().io(device, stream, operation, frames, cycle, buffer)
                               : kAudioHardwareBadObjectError;
}

AudioServerPlugInDriverInterface interface{
    nullptr,      query,   addRef,      release, initialize, create, destroy, addClient,
    removeClient, perform, abortChange, has,     settable,   size,   get,     set,
    start,        stop,    stamp,       willDo,  boundary,   io,     boundary};
} // namespace

extern "C" void* LoopZRoutingAudio_Create(CFAllocatorRef, CFUUIDRef type) {
    return type && CFEqual(type, kAudioServerPlugInTypeUUID) ? reference : nullptr;
}

#if defined(LOOPZ_LEGACY_DRIVER_SLOT)
// The existing HAL slot may cache the legacy factory name across a host restart.
// This is the same routing implementation, with only its installation identity
// adapted for a separately guarded migration.
extern "C" void* LoopZAudio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    return LoopZRoutingAudio_Create(allocator, type);
}
#endif

// Internal control entry for the authenticated driver connection and the
// native host tests. It is not exposed as a writable HAL property or a service.
extern "C" OSStatus LoopZRoutingApplySnapshot(xpc_object_t message) {
    return control([&] {
        return state().apply(loopz::macos::DeviceTransportSnapshot::decode(message));
    });
}

extern "C" OSStatus
LoopZRoutingConfigurationStatus(uint64_t revision,
                                loopz::macos::DriverConfigurationResult* result) {
    if (!result)
        return kAudioHardwareIllegalOperationError;
    return control([&] {
        *result = state().configurationResult(revision);
        return OSStatus(noErr);
    });
}

extern "C" OSStatus LoopZRoutingCancelConfiguration(uint64_t revision) {
    return control([&] {
        return state().cancelConfiguration(revision);
    });
}

// Internal ownership gate for the authenticated connection adapter and native
// host tests. It is never exposed as a writable HAL property or XPC command.
extern "C" OSStatus LoopZRoutingSampleRateCapabilities(uint32_t mask) {
    return control([&] {
        state().setSampleRateMask(mask);
        return OSStatus(noErr);
    });
}

extern "C" OSStatus LoopZRoutingServiceAvailability(Boolean available) {
    return control([&] {
        state().setServiceAvailable(available);
        return OSStatus(noErr);
    });
}
extern "C" xpc_object_t LoopZRoutingRateRequests() {
    // Internal fixture/control export; production uses the authenticated role.
    try {
        return loopz::macos::routing::encodeRateRequests(state().rateRequests());
    } catch (...) {
        return nullptr;
    }
}
extern "C" OSStatus LoopZRoutingAcknowledgeRates(xpc_object_t requests) {
    return control([&] {
        return state().acknowledgeRates(loopz::macos::routing::rateRequests(requests));
    });
}
