#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <AudioUnit/AudioUnit.h>
#include <loopz/macos/driver_protocol.hpp>
#include <xpc/xpc.h>
#include <atomic>
#include <array>
#include <functional>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdio>
#include <string>
#include <cmath>
#include <cstring>

namespace {
template <class T>
bool get(AudioObjectID id, AudioObjectPropertySelector selector, T& value,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 bytes = sizeof(value);
    return AudioObjectGetPropertyData(id, &address, 0, nullptr, &bytes, &value) == noErr &&
           bytes == sizeof(value);
}
NSString* uid(AudioObjectID id) {
    CFStringRef text = nullptr;
    return id && get(id, kAudioDevicePropertyDeviceUID, text) && text ? CFBridgingRelease(text)
                                                                      : nil;
}
AudioObjectID device(NSString* text) {
    CFStringRef value = (__bridge CFStringRef)text;
    AudioObjectID id = 0;
    UInt32 bytes = sizeof(id);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(value), &value,
                                      &bytes, &id) == noErr
               ? id
               : 0;
}
NSDictionary* routingPlugIn() {
    CFStringRef bundle = CFSTR("com.lexasoft.loopz.driver.routing");
    AudioObjectID object = kAudioObjectUnknown;
    UInt32 bytes = sizeof(object);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateBundleIDToPlugIn,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    const auto error = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                  sizeof(bundle), &bundle, &bytes, &object);
    return @{
        @"queryStatus" : @(error),
        @"registered" : @(error == noErr && object != 0),
        @"objectID" : @(error == noErr ? object : 0)
    };
}
NSDictionary* bridgeStatus() {
    auto connection = xpc_connection_create_mach_service(loopz::kService, nullptr,
                                                         XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    if (!connection)
        return nil;
    xpc_connection_set_event_handler(connection, ^(xpc_object_t){
                                     });
    xpc_connection_activate(connection);
    auto request = [&](const char* op) -> xpc_object_t {
        struct Holder {
            xpc_object_t reply{nullptr};
        };
        auto holder = std::make_shared<Holder>();
        auto done = dispatch_semaphore_create(0);
        auto message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(message, "role", "control");
        xpc_dictionary_set_string(message, "op", op);
        xpc_dictionary_set_uint64(message, "protocol", loopz::kProtocol);
        xpc_connection_send_message_with_reply(
            connection, message, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
            ^(xpc_object_t reply) {
                holder->reply = reply;
                dispatch_semaphore_signal(done);
            });
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)))
            return nullptr;
        return holder->reply;
    };
    auto hello = request("hello");
    NSDictionary* result = nil;
    if (hello && xpc_get_type(hello) == XPC_TYPE_DICTIONARY &&
        xpc_dictionary_get_bool(hello, "ready")) {
        auto reply = request("attach");
        if (reply && xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
            xpc_dictionary_get_bool(reply, "status"))
            result = @{
                @"protocol" : @(xpc_dictionary_get_uint64(reply, "protocol")),
                @"driverPID" : @(xpc_dictionary_get_int64(reply, "driverPID")),
                @"driverAttached" : @(xpc_dictionary_get_bool(reply, "driverAttached")),
                @"helperAttached" : @(xpc_dictionary_get_bool(reply, "helperAttached"))
            };
    }
    xpc_connection_cancel(connection);
    return result;
}
NSDictionary* snapshot(std::string& failure) {
    const auto unavailable = [&](const char* stage) -> NSDictionary* {
        failure = stage;
        return nil;
    };
    AudioObjectID input = 0, output = 0, system = 0;
    if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, input) ||
        !get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, output) ||
        !get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultSystemOutputDevice, system) ||
        !uid(output))
        return unavailable("audio defaults");
    auto virtualDevice = device(@(loopz::kDeviceUID));
    UInt32 alive = 0, running = 0;
    if (virtualDevice &&
        (!get(virtualDevice, kAudioDevicePropertyDeviceIsAlive, alive) ||
         !get(virtualDevice, kAudioDevicePropertyDeviceIsRunningSomewhere, running)))
        return unavailable("legacy device state");
    UInt32 period = 0, latency = 0;
    if (virtualDevice &&
        (!get(virtualDevice, kAudioDevicePropertyZeroTimeStampPeriod, period) ||
         !get(virtualDevice, kAudioDevicePropertyLatency, latency, kAudioObjectPropertyScopeInput)))
        return unavailable("legacy device timing");
    NSDictionary* timing = @{};
    AudioObjectPropertyAddress timingAddress{0x4c5a746d, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    if (virtualDevice && AudioObjectHasProperty(virtualDevice, &timingAddress)) {
        CFPropertyListRef value = nullptr;
        UInt32 size = sizeof(value);
        if (AudioObjectGetPropertyData(virtualDevice, &timingAddress, 0, nullptr, &size, &value) ||
            !value)
            return unavailable("legacy timing property");
        id object = CFBridgingRelease(value);
        if (![object isKindOfClass:NSDictionary.class])
            return unavailable("legacy timing payload");
        timing = object;
    }
    UInt32 listSize = 0;
    AudioObjectPropertyAddress listAddress{kAudioHardwarePropertyDevices,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddress, 0, nullptr,
                                       &listSize) ||
        listSize > 1024 * 1024 || listSize % sizeof(AudioObjectID))
        return unavailable("audio device list size");
    std::vector<AudioObjectID> devices(listSize / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddress, 0, nullptr, &listSize,
                                   devices.data()))
        return unavailable("audio device list");
    NSMutableDictionary<NSString*, NSArray*>* members = [NSMutableDictionary dictionary];
    NSMutableArray* routing = [NSMutableArray array];
    NSMutableSet* busy = [NSMutableSet set];
    for (auto candidate : devices) {
        NSString* candidateUID = uid(candidate);
        if ([candidateUID hasPrefix:@"com.lexasoft.loopz.device."]) {
            UInt32 live = 0, active = 0, inputLatency = 0;
            Float64 rate = 0;
            if (!get(candidate, kAudioDevicePropertyDeviceIsAlive, live) ||
                !get(candidate, kAudioDevicePropertyDeviceIsRunning, active) ||
                !get(candidate, kAudioDevicePropertyNominalSampleRate, rate) ||
                !get(candidate, kAudioDevicePropertyLatency, inputLatency,
                     kAudioObjectPropertyScopeInput))
                return unavailable("routing device state");
            std::array<UInt32, 2> channels{};
            for (size_t direction = 0; direction < channels.size(); ++direction) {
                const auto scope = direction == 0 ? kAudioObjectPropertyScopeInput
                                                  : kAudioObjectPropertyScopeOutput;
                AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, scope,
                                                   kAudioObjectPropertyElementMain};
                UInt32 size = 0;
                if (AudioObjectGetPropertyDataSize(candidate, &streams, 0, nullptr, &size) ||
                    size > 64 * sizeof(AudioObjectID) || size % sizeof(AudioObjectID))
                    return unavailable("routing stream list size");
                std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
                if (size &&
                    AudioObjectGetPropertyData(candidate, &streams, 0, nullptr, &size, ids.data()))
                    return unavailable("routing stream list");
                for (const auto stream : ids) {
                    AudioStreamBasicDescription format{};
                    if (!get(stream, kAudioStreamPropertyVirtualFormat, format) ||
                        format.mChannelsPerFrame > 64 ||
                        channels[direction] > 64 - format.mChannelsPerFrame)
                        return unavailable("routing stream format");
                    channels[direction] += format.mChannelsPerFrame;
                }
            }
            [routing addObject:@{
                @"uid" : candidateUID,
                @"alive" : @(live != 0),
                @"running" : @(active != 0),
                @"sampleRate" : @(rate),
                @"inputChannels" : @(channels[0]),
                @"outputChannels" : @(channels[1]),
                @"inputLatencyFrames" : @(inputLatency)
            }];
        }
        AudioClassID type = 0;
        if (!get(candidate, kAudioObjectPropertyClass, type))
            return unavailable("audio device class");
        if (type != kAudioAggregateDeviceClassID)
            continue;
        NSString* name = uid(candidate);
        CFArrayRef list = nullptr;
        UInt32 active = 0;
        if (!name || !get(candidate, kAudioAggregateDevicePropertyFullSubDeviceList, list) ||
            !list || !get(candidate, kAudioDevicePropertyDeviceIsRunningSomewhere, active)) {
            if (list)
                CFRelease(list);
            return unavailable("aggregate device state");
        }
        id object = CFBridgingRelease(list);
        if (![object isKindOfClass:NSArray.class])
            return unavailable("aggregate device members");
        for (id member in (NSArray*)object)
            if (![member isKindOfClass:NSString.class])
                return unavailable("aggregate member identity");
        members[name] = object;
        if (active || [name isEqualToString:uid(input)] || [name isEqualToString:uid(output)] ||
            [name isEqualToString:uid(system)])
            [busy addObject:name];
    }
    std::function<bool(NSString*, NSMutableSet*, bool)> contains =
        [&](NSString* name, NSMutableSet* visited, bool routingOnly) {
            if (routingOnly ? [name hasPrefix:@"com.lexasoft.loopz.device."]
                            : [name isEqualToString:@(loopz::kDeviceUID)])
                return true;
            if ([visited containsObject:name])
                return false;
            [visited addObject:name];
            for (NSString* member in members[name])
                if (contains(member, visited, routingOnly))
                    return true;
            return false;
        };
    NSMutableArray* blocking = [NSMutableArray array];
    NSMutableArray* routingBlocking = [NSMutableArray array];
    for (NSString* name in busy)
        if (contains(name, [NSMutableSet set], false))
            [blocking addObject:name];
    for (NSString* name in busy)
        if (contains(name, [NSMutableSet set], true))
            [routingBlocking addObject:name];
    return @{
        @"driverTiming" : timing,
        @"routingDevices" : routing,
        @"routingPlugIn" : routingPlugIn(),
        @"routingBlockingRoutes" : routingBlocking,
        @"clockPeriodFrames" : @(period),
        @"inputLatencyFrames" : @(latency),
        @"loopzBlockingRoutes" : blocking,
        @"defaultInput" : uid(input) ?: @"",
        @"defaultOutput" : uid(output) ?: @"",
        @"defaultSystemOutput" : uid(system) ?: @"",
        @"loopzVisible" : @(virtualDevice != 0),
        @"loopzAlive" : @(alive != 0),
        @"loopzRunning" : @(running != 0)
    };
}
bool emit(NSDictionary* object, const std::string& path = "") {
    if (!object)
        return false;
    NSData* data = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    if (!path.empty())
        return [data writeToFile:@(path.c_str()) atomically:YES];
    std::fwrite(data.bytes, 1, data.length, stdout);
    std::puts("");
    std::fflush(stdout);
    return true;
}
struct Guard {
    AudioUnit unit{nullptr};
    std::atomic<uint64_t> frames{0}, errors{0};
    double expected{0};
    bool previous{false};
    static OSStatus render(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp* time,
                           UInt32, UInt32 frames, AudioBufferList* output) {
        auto& self = *static_cast<Guard*>(context);
        if (output)
            for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
                if (output->mBuffers[b].mData)
                    std::memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);
        if (!time || !(time->mFlags & kAudioTimeStampHostTimeValid) ||
            !(time->mFlags & kAudioTimeStampSampleTimeValid) || !std::isfinite(time->mSampleTime))
            self.errors.fetch_add(1);
        else {
            if (self.previous && std::fabs(time->mSampleTime - self.expected) > 0.5)
                self.errors.fetch_add(1);
            self.expected = time->mSampleTime + frames;
            self.previous = true;
        }
        self.frames.fetch_add(frames);
        return noErr;
    }
    ~Guard() {
        if (unit) {
            AudioOutputUnitStop(unit);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
        }
    }
};
} // namespace
int main(int argc, char** argv) {
    @autoreleasepool {
        std::string guardUID, stopFile, report;
        double seconds = 90;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--bridge-status" && argc == 2)
                return emit(bridgeStatus()) ? 0 : 1;
            if (arg == "--snapshot" && argc == 2) {
                // HAL object IDs can disappear while a virtual device is withdrawn.
                // Retry the entire catalog; never admit a partial safety snapshot.
                std::string failure;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                do {
                    if (NSDictionary* state = snapshot(failure))
                        return emit(state) ? 0 : 1;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } while (std::chrono::steady_clock::now() < deadline);
                std::fprintf(stderr, "Audio catalog did not settle: %s\n", failure.c_str());
                return 1;
            }
            if (i + 1 >= argc)
                return 2;
            if (arg == "--guard")
                guardUID = argv[++i];
            else if (arg == "--stop-file")
                stopFile = argv[++i];
            else if (arg == "--report")
                report = argv[++i];
            else if (arg == "--seconds")
                seconds = std::strtod(argv[++i], nullptr);
            else
                return 2;
        }
        if (guardUID.empty() || guardUID == loopz::kDeviceUID ||
            guardUID.starts_with("com.lexasoft.loopz.device.") || stopFile.empty() ||
            report.empty() || !std::isfinite(seconds) || seconds < 1 || seconds > 300)
            return 2;
        const auto id = device(@(guardUID.c_str()));
        double rate = 0;
        if (!id || !get(id, kAudioDevicePropertyNominalSampleRate, rate) || !std::isfinite(rate) ||
            rate < 8000)
            return 1;
        Guard guard;
        AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                              kAudioUnitManufacturer_Apple, 0, 0};
        auto component = AudioComponentFindNext(nullptr, &description);
        auto status = component ? AudioComponentInstanceNew(component, &guard.unit) : -1;
        UInt32 enabled = 0;
        if (!status)
            status = AudioUnitSetProperty(guard.unit, kAudioOutputUnitProperty_EnableIO,
                                          kAudioUnitScope_Input, 1, &enabled, sizeof(enabled));
        if (!status)
            status = AudioUnitSetProperty(guard.unit, kAudioOutputUnitProperty_CurrentDevice,
                                          kAudioUnitScope_Global, 0, &id, sizeof(id));
        AudioStreamBasicDescription format{
            rate,
            kAudioFormatLinearPCM,
            static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) |
                static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved),
            4,
            1,
            4,
            2,
            32,
            0};
        if (!status)
            status = AudioUnitSetProperty(guard.unit, kAudioUnitProperty_StreamFormat,
                                          kAudioUnitScope_Input, 0, &format, sizeof(format));
        AURenderCallbackStruct callback{Guard::render, &guard};
        if (!status)
            status = AudioUnitSetProperty(guard.unit, kAudioUnitProperty_SetRenderCallback,
                                          kAudioUnitScope_Input, 0, &callback, sizeof(callback));
        if (!status)
            status = AudioUnitInitialize(guard.unit);
        if (!status)
            status = AudioOutputUnitStart(guard.unit);
        if (status) {
            std::fprintf(stderr, "Hardware continuity guard could not start (%d)\n",
                         static_cast<int>(status));
            return 1;
        }
        auto started = std::chrono::steady_clock::now();
        bool ready = false, requested = false;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() <
               seconds) {
            if ([NSFileManager.defaultManager fileExistsAtPath:@(stopFile.c_str())]) {
                requested = true;
                break;
            }
            if (!ready && guard.frames.load() > 0) {
                ready = emit(
                    @{@"state" : @"running",
                      @"uid" : @(guardUID.c_str()),
                      @"rate" : @(rate)},
                    report);
                if (!ready)
                    return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        AudioOutputUnitStop(guard.unit);
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto frames = guard.frames.load(), errors = guard.errors.load();
        // Includes start/stop scheduling uncertainty; sample-counter gaps still fail.
        const bool passed =
            requested && ready && !errors && std::fabs(frames - elapsed * rate) < rate * 0.1 + 8192;
        return emit(
                   @{
                       @"state" : @"stopped",
                       @"passed" : @(passed),
                       @"stoppedByRequest" : @(requested),
                       @"frames" : @(frames),
                       @"timestampErrors" : @(errors),
                       @"seconds" : @(elapsed),
                       @"rate" : @(rate),
                       @"uid" : @(guardUID.c_str())
                   },
                   report) &&
                       passed
                   ? 0
                   : 1;
    }
}
