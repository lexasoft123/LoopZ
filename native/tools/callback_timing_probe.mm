#include <loopz/callback_timing.hpp>
#include <loopz/macos/driver_timing.hpp>
#include <loopz/macos/device_transport.hpp>
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
using namespace loopz::macos;
AudioObjectPropertyAddress address(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}
template <typename T> T scalar(AudioObjectID object, AudioObjectPropertySelector selector) {
    T result{};
    auto property = address(selector);
    UInt32 bytes = sizeof(result);
    if (AudioObjectGetPropertyData(object, &property, 0, nullptr, &bytes, &result) ||
        bytes != sizeof(result))
        throw std::runtime_error("Cannot read the LoopZ device metadata");
    return result;
}
uint32_t number(NSDictionary* dictionary, CFStringRef key, uint32_t maximum) {
    const auto value = (__bridge CFTypeRef)dictionary[(__bridge NSString*)key];
    int64_t result = -1;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
        CFNumberIsFloatType(static_cast<CFNumberRef>(value)) ||
        !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result) ||
        result < 0 || result > maximum)
        throw std::runtime_error("Invalid callback timing integer");
    return static_cast<uint32_t>(result);
}
NSDictionary* timing(AudioObjectID object, bool callback) {
    const auto raw = scalar<CFPropertyListRef>(object, callback ? kLoopZDevicePropertyCallbackTiming
                                                                : kLoopZDevicePropertyIOTiming);
    if (!raw)
        throw std::runtime_error("Empty driver timing metadata");
    id value = CFBridgingRelease(raw);
    if (![value isKindOfClass:[NSDictionary class]] || [value count] != (callback ? 5U : 4U))
        throw std::runtime_error("Invalid driver timing dictionary");
    NSDictionary* dictionary = value;
    const auto version = number(dictionary, CFSTR("version"), UINT32_MAX);
    if (version != (callback ? kLoopZDeviceCallbackTimingVersion : kLoopZDeviceIOTimingVersion))
        throw std::runtime_error("Unsupported driver timing version");
    const auto session = number(dictionary, CFSTR("session"), UINT32_MAX);
    const auto running = (__bridge CFTypeRef)dictionary[@"running"];
    if (!running || CFGetTypeID(running) != CFBooleanGetTypeID())
        throw std::runtime_error("Invalid driver IO state");
    const auto first =
        number(dictionary, callback ? CFSTR("maximumWriteFrames") : CFSTR("writePeriodFrames"),
               callback ? loopz::CallbackTimingObservation::maximumFrames : kDeviceAudioCapacity);
    const auto nominal = callback ? number(dictionary, CFSTR("maximumNominalFrames"),
                                           loopz::CallbackTimingObservation::maximumFrames)
                                  : 0;
    if (((first || nominal) && !session) || (callback && nominal && !first))
        throw std::runtime_error("Invalid driver callback session");
    return dictionary;
}
AudioObjectID resolve(NSString* uid) {
    AudioObjectID object = 0;
    auto property = address(kAudioHardwarePropertyTranslateUIDToDevice);
    const auto text = (__bridge CFStringRef)uid;
    UInt32 bytes = sizeof(object);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, sizeof(text), &text, &bytes,
                                   &object) ||
        bytes != sizeof(object))
        throw std::runtime_error("Cannot resolve the LoopZ device");
    return object;
}
NSDictionary* inspect(NSString* uid) {
    const auto object = resolve(uid);
    if (!object)
        return @{@"uid" : uid, @"available" : @NO, @"supported" : @NO};
    const auto model = scalar<CFStringRef>(object, kAudioDevicePropertyModelUID);
    const bool ours = model && CFGetTypeID(model) == CFStringGetTypeID() &&
                      CFEqual(model, CFSTR("com.lexasoft.loopz.device.model.v2"));
    if (model)
        CFRelease(model);
    if (!ours || scalar<UInt32>(object, kAudioDevicePropertyTransportType) !=
                     kAudioDeviceTransportTypeVirtual)
        throw std::runtime_error("Callback timing probe requires a LoopZ virtual endpoint");
    const auto legacy = timing(object, false);
    auto property = address(kLoopZDevicePropertyCallbackTiming);
    const bool supported = AudioObjectHasProperty(object, &property);
    auto* result = [NSMutableDictionary dictionaryWithDictionary:@{
        @"uid" : uid,
        @"available" : @YES,
        @"supported" : @(supported),
        @"legacy" : legacy
    }];
    if (supported) {
        const auto callback = timing(object, true);
        result[@"callbacks"] = callback;
        result[@"sameSession"] = @([legacy[@"session"] isEqual:callback[@"session"]]);
    }
    return result;
}
std::vector<std::string> arguments(int argc, char** argv) {
    if (argc < 3 || argc > 65 || argc % 2 != 1)
        throw std::invalid_argument(
            "Usage: callback timing probe --device LoopZ-uid [--device LoopZ-uid ...]");
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (int index = 1; index < argc; index += 2) {
        const std::string key = argv[index], uid = argv[index + 1];
        if (key != "--device" || !uid.starts_with("com.lexasoft.loopz.device.") ||
            uid.size() > 4096 || !seen.insert(uid).second)
            throw std::invalid_argument(
                "Callback timing probe requires up to 32 distinct LoopZ device UIDs");
        result.push_back(uid);
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const auto requested = arguments(argc, argv);
            auto* devices = [NSMutableArray array];
            for (const auto& value : requested) {
                auto* uid = [NSString stringWithUTF8String:value.c_str()];
                if (!uid)
                    throw std::invalid_argument("Invalid device UID encoding");
                [devices addObject:inspect(uid)];
            }
            auto* data = [NSJSONSerialization dataWithJSONObject:@{
                @"version" : @1,
                @"devices" : devices,
                @"audioIOOpened" : @NO,
                @"permissionRequested" : @NO,
                @"scope" : @"per-session maximum actual WriteMix frames and nominal IO period"
            }
                                                         options:0
                                                           error:nil];
            if (!data || std::fwrite(data.bytes, 1, data.length, stdout) != data.length ||
                std::putchar('\n') == EOF)
                throw std::runtime_error("Cannot write callback timing metadata");
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 2;
        }
    }
}
