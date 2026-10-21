#include "../../../native/platform/macos/probe/routing_io_support.hpp"
#include <set>

namespace {
template <typename T> T property(AudioObjectID object, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    T value{};
    UInt32 bytes = sizeof(value);
    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, &value) ||
        bytes != sizeof(value))
        throw std::runtime_error("Device metadata changed during observation");
    return value;
}

NSString* name(AudioObjectID object) {
    const auto value = property<CFStringRef>(object, kAudioObjectPropertyName);
    if (!value)
        throw std::runtime_error("Device name is unavailable");
    if (CFGetTypeID(value) != CFStringGetTypeID()) {
        CFRelease(value);
        throw std::runtime_error("Device name has an unexpected type");
    }
    return CFBridgingRelease(value);
}

NSArray* streams(AudioObjectID object, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreams, scope,
                                       kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &bytes) ||
        bytes > 64 * sizeof(AudioObjectID) || bytes % sizeof(AudioObjectID))
        throw std::runtime_error("Device stream list is unavailable");
    std::vector<AudioObjectID> ids(bytes / sizeof(AudioObjectID));
    const auto expected = bytes;
    if (bytes && (AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, ids.data()) ||
                  bytes != expected))
        throw std::runtime_error("Device stream list changed during observation");
    NSMutableArray* result = [NSMutableArray array];
    for (auto id : ids) {
        const auto format =
            property<AudioStreamBasicDescription>(id, kAudioStreamPropertyVirtualFormat);
        [result addObject:@{@"object" : @(id), @"channels" : @(format.mChannelsPerFrame)}];
    }
    return result;
}

NSDictionary* inspect(const std::string& uid) {
    CFStringRef text = (__bridge CFStringRef) @(uid.c_str());
    AudioObjectID object = 0;
    UInt32 bytes = sizeof(object);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(text), &text, &bytes,
                                   &object) ||
        bytes != sizeof(object))
        throw std::runtime_error("Device UID lookup failed");
    if (!object)
        return @{@"uid" : @(uid.c_str()), @"present" : @NO};
    if (loopz::macos::diagnostic::requireRoutingDevice(uid) != object)
        throw std::runtime_error("Device identity changed during observation");
    return @{
        @"uid" : @(uid.c_str()),
        @"present" : @YES,
        @"object" : @(object),
        @"name" : name(object),
        @"alive" : @(property<UInt32>(object, kAudioDevicePropertyDeviceIsAlive) != 0),
        @"rate" : @(property<Float64>(object, kAudioDevicePropertyNominalSampleRate)),
        @"inputs" : streams(object, kAudioObjectPropertyScopeInput),
        @"outputs" : streams(object, kAudioObjectPropertyScopeOutput)
    };
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if (argc < 2 || argc > 17)
                throw std::invalid_argument("Supply 1–16 LoopZ routing UIDs");
            std::set<std::string> uids;
            for (int index = 1; index < argc; ++index) {
                const std::string uid = argv[index];
                if (!uid.starts_with("com.lexasoft.loopz.device.") || uid.size() > 128 ||
                    !uids.insert(uid).second)
                    throw std::invalid_argument("Only distinct LoopZ routing UIDs are accepted");
            }
            NSMutableArray* devices = [NSMutableArray array];
            for (const auto& uid : uids)
                [devices addObject:inspect(uid)];
            NSError* error = nil;
            const auto data = [NSJSONSerialization dataWithJSONObject:@{
                @"devices" : devices,
                @"devicesOpened" : @NO,
                @"permissionRequested" : @NO
            }
                                                              options:0
                                                                error:&error];
            if (!data || std::fwrite(data.bytes, 1, data.length, stdout) != data.length ||
                std::putchar('\n') == EOF)
                throw std::runtime_error("Device metadata could not be written");
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 2;
        }
    }
}
