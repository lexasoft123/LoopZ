#include <loopz/macos/device_inventory.hpp>
#include <loopz/macos/driver_protocol.hpp>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace loopz::macos {
namespace {
template <class T>
bool read(AudioObjectID object, AudioObjectPropertySelector selector, T& value,
          AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 bytes = sizeof(value);
    return !AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, &value) &&
           bytes == sizeof(value);
}
std::string stringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value = nullptr;
    if (!read(object, selector, value) || !value)
        throw std::runtime_error("Audio device metadata is unavailable");
    NSString* owned = CFBridgingRelease(value);
    if (![owned isKindOfClass:NSString.class] || !owned.UTF8String)
        throw std::runtime_error("Audio device metadata is invalid");
    return owned.UTF8String;
}
uint32_t channelCount(AudioObjectID object, AudioObjectPropertyScope scope) {
    const AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, scope,
                                             kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &bytes))
        return 0;
    if (bytes < offsetof(AudioBufferList, mBuffers) || bytes > 1024 * 1024)
        throw std::runtime_error("Audio device channel metadata exceeds its bound");
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const auto capacity = bytes;
    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, storage.data()) ||
        bytes > capacity || bytes < offsetof(AudioBufferList, mBuffers))
        throw std::runtime_error("Audio device channel metadata changed");
    const auto* list = reinterpret_cast<const AudioBufferList*>(storage.data());
    if (list->mNumberBuffers > (bytes - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer))
        throw std::runtime_error("Audio device channel metadata is invalid");
    uint64_t count = 0;
    for (UInt32 index = 0; index < list->mNumberBuffers; ++index)
        count += list->mBuffers[index].mNumberChannels;
    if (count > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Audio device channel count exceeds its bound");
    return static_cast<uint32_t>(count);
}
singz::AudioHostInventory metadataInventory() {
    // The picker uses counts and buffer facts, not every channel's label.
    // Exclude our virtual devices before inspecting any expensive metadata.
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &bytes) ||
        bytes % sizeof(AudioDeviceID) || bytes > 4096 * sizeof(AudioDeviceID))
        throw std::runtime_error("Audio device catalog is unavailable");
    std::vector<AudioDeviceID> objects(bytes / sizeof(AudioDeviceID));
    const auto capacity = bytes;
    if (bytes && (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &bytes,
                                             objects.data()) ||
                  bytes > capacity || bytes % sizeof(AudioDeviceID)))
        throw std::runtime_error("Audio device catalog changed");
    objects.resize(bytes / sizeof(AudioDeviceID));
    AudioDeviceID defaultInput = 0, defaultOutput = 0;
    (void)read(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, defaultInput);
    (void)read(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, defaultOutput);
    singz::AudioHostInventory result;
    result.devices.reserve(objects.size());
    for (const auto object : objects) {
        singz::AudioHostDeviceInfo device;
        device.uid = stringProperty(object, kAudioDevicePropertyDeviceUID);
        if (device.uid == kDeviceUID || device.uid.starts_with("com.lexasoft.loopz.device."))
            continue;
        device.inputChannels = channelCount(object, kAudioObjectPropertyScopeInput);
        device.outputChannels = channelCount(object, kAudioObjectPropertyScopeOutput);
        if (!device.inputChannels && !device.outputChannels)
            continue;
        device.label = stringProperty(object, kAudioObjectPropertyName);
        (void)read(object, kAudioDevicePropertyNominalSampleRate, device.nominalSampleRate);
        (void)read(object, kAudioDevicePropertyBufferFrameSize,
                   device.bufferFrames.preferredFrames);
        AudioValueRange range{};
        if (read(object, kAudioDevicePropertyBufferFrameSizeRange, range) &&
            std::isfinite(range.mMinimum) && std::isfinite(range.mMaximum) && range.mMinimum >= 1 &&
            range.mMaximum >= range.mMinimum &&
            range.mMaximum <= std::numeric_limits<uint32_t>::max()) {
            device.bufferFrames.minimumFrames = static_cast<uint32_t>(std::ceil(range.mMinimum));
            device.bufferFrames.maximumFrames = static_cast<uint32_t>(std::floor(range.mMaximum));
        }
        device.defaultInput = object == defaultInput;
        device.defaultOutput = object == defaultOutput;
        device.direction = device.inputChannels && device.outputChannels
                               ? singz::AudioHostEndpointDirection::Duplex
                           : device.inputChannels ? singz::AudioHostEndpointDirection::Input
                                                  : singz::AudioHostEndpointDirection::Output;
        if (stringProperty(object, kAudioDevicePropertyDeviceUID) != device.uid)
            throw std::runtime_error("Audio device identity changed");
        if (device.defaultInput)
            result.defaultInputUid = device.uid;
        if (device.defaultOutput)
            result.defaultOutputUid = device.uid;
        result.devices.push_back(std::move(device));
    }
    return result;
}
NSString* text(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding]
               ?: @"Unavailable";
}
NSArray* project(const singz::AudioHostInventory& inventory) {
    auto* result = [NSMutableArray array];
    for (const auto& device : inventory.devices) {
        if (device.uid == kDeviceUID || device.uid.starts_with("com.lexasoft.loopz.device."))
            continue;
        [result addObject:@{
            @"uid" : text(device.uid),
            @"name" : text(device.label),
            @"inputChannels" : @(device.inputChannels),
            @"outputChannels" : @(device.outputChannels),
            @"rate" : @(device.nominalSampleRate),
            @"defaultInput" : @(device.defaultInput),
            @"defaultOutput" : @(device.defaultOutput),
            @"bufferFrames" : @(device.bufferFrames.preferredFrames),
            @"minimumBufferFrames" : @(device.bufferFrames.minimumFrames),
            @"maximumBufferFrames" : @(device.bufferFrames.maximumFrames)
        }];
    }
    return [result sortedArrayUsingComparator:^NSComparisonResult(id a, id b) {
        return [a[@"name"] localizedCaseInsensitiveCompare:b[@"name"]];
    }];
}
AudioDeviceID resolveDevice(const std::string& uid) {
    auto* value = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    if (!value)
        throw std::invalid_argument("The audio device identity is invalid");
    AudioDeviceID device = 0;
    UInt32 bytes = sizeof(device);
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                   sizeof(value), &value, &bytes, &device);
    CFRelease(value);
    if (status || bytes != sizeof(device) || !device)
        throw std::runtime_error("The selected audio device is unavailable");
    return device;
}
void writeBuffer(const std::string& uid, uint32_t expected, uint32_t frames) {
    const auto device = resolveDevice(uid);
    const AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    Boolean settable = false;
    UInt32 current = 0, bytes = sizeof(current);
    if (AudioObjectIsPropertySettable(device, &address, &settable) || !settable)
        throw std::runtime_error("This device does not allow changing its hardware buffer");
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &current) ||
        bytes != sizeof(current) || current != expected || resolveDevice(uid) != device)
        throw std::runtime_error("The hardware buffer changed. Refresh Settings and try again");
    if (AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(frames), &frames))
        throw std::runtime_error("macOS did not accept this hardware buffer size");
    bytes = sizeof(current);
    if (resolveDevice(uid) != device ||
        AudioObjectGetPropertyData(device, &address, 0, nullptr, &bytes, &current) ||
        bytes != sizeof(current) || current != frames)
        throw std::runtime_error("The device did not confirm that buffer size. Refresh Settings");
}
} // namespace

struct DeviceInventory::AsyncDiscovery {
    struct Result {
        uint64_t generation;
        std::optional<singz::AudioHostInventory> inventory;
    };
    std::mutex mutex;
    uint64_t generation{0};
    bool running{false};
    std::optional<Result> completed;
};

DeviceInventory::DeviceInventory(Provider provider, BufferWriter bufferWriter, DiscoveryMode mode)
    : async_(mode == DiscoveryMode::Background || (mode == DiscoveryMode::Automatic && !provider)
                 ? std::make_shared<AsyncDiscovery>()
                 : nullptr),
      provider_(provider ? std::move(provider) : metadataInventory),
      bufferWriter_(bufferWriter ? std::move(bufferWriter) : writeBuffer) {}

void DeviceInventory::validateBufferFrames(const std::string& uid, uint32_t expected,
                                           uint32_t frames) {
    if (uid.empty() || uid.size() > 4096 || uid.find('\0') != std::string::npos ||
        uid == kDeviceUID || uid.starts_with("com.lexasoft.loopz.device.") || !expected ||
        !frames || frames > 4096 || (frames & (frames - 1)))
        throw std::invalid_argument("Invalid audio device or hardware buffer size");
    const auto inventory = provider_();
    const auto device =
        std::find_if(inventory.devices.begin(), inventory.devices.end(), [&](const auto& item) {
            return item.uid == uid;
        });
    if (device == inventory.devices.end() || (!device->inputChannels && !device->outputChannels))
        throw std::runtime_error("The selected audio device is unavailable");
    if (device->bufferFrames.preferredFrames != expected)
        throw std::runtime_error("The hardware buffer changed. Refresh Settings and try again");
    if (!device->bufferFrames.minimumFrames || !device->bufferFrames.maximumFrames ||
        frames < device->bufferFrames.minimumFrames ||
        frames > device->bufferFrames.maximumFrames ||
        (device->bufferFrames.fundamentalFrames && frames % device->bufferFrames.fundamentalFrames))
        throw std::invalid_argument("This hardware buffer size is outside the device's range");
}
void DeviceInventory::setBufferFrames(const std::string& uid, uint32_t expected, uint32_t frames) {
    validateBufferFrames(uid, expected, frames);
    invalidate();
    cached_ = nil;
    if (frames != expected)
        bufferWriter_(uid, expected, frames);
}

NSDictionary* DeviceInventory::snapshot(uint64_t now) {
    if (async_) {
        std::optional<AsyncDiscovery::Result> completed;
        uint64_t generation = 0;
        bool start = false, running;
        {
            std::lock_guard lock(async_->mutex);
            if (async_->completed) {
                if (async_->completed->generation == async_->generation)
                    completed = std::move(async_->completed);
                async_->completed.reset();
            }
            if (!completed && (!cached_ || now >= nextRefresh_) && !async_->running) {
                generation = async_->generation;
                async_->running = start = true;
            }
            running = async_->running;
        }
        if (completed) {
            nextRefresh_ = now + 2000;
            if (completed->inventory)
                cached_ = @{
                    @"devices" : project(*completed->inventory),
                    @"devicesFresh" : @YES,
                    @"devicesRefreshing" : @NO,
                    @"devicesError" : @""
                };
            else
                cached_ = @{
                    @"devices" : cached_[@"devices"] ?: @[],
                    @"devicesFresh" : @NO,
                    @"devicesRefreshing" : @NO,
                    @"devicesError" :
                        @"Input and output devices could not be refreshed. Retrying automatically."
                };
        }
        if (start) {
            try {
                // Only the provider copy and shared result state cross threads.
                // An in-flight native metadata call can outlive this inventory;
                // teardown never joins a potentially blocked Core Audio query.
                std::thread([state = async_, provider = provider_, generation] {
                    @autoreleasepool {
                        AsyncDiscovery::Result result{generation, std::nullopt};
                        try {
                            result.inventory = provider();
                        } catch (...) {
                        }
                        std::lock_guard lock(state->mutex);
                        state->completed = std::move(result);
                        state->running = false;
                    }
                }).detach();
            } catch (...) {
                std::lock_guard lock(async_->mutex);
                async_->running = false;
                async_->completed = AsyncDiscovery::Result{generation, std::nullopt};
            }
        }
        if (running)
            return @{
                @"devices" : cached_[@"devices"] ?: @[],
                @"devicesFresh" : @NO,
                @"devicesRefreshing" : @YES,
                @"devicesError" : @""
            };
        return cached_;
    }
    if (cached_ && now < nextRefresh_)
        return cached_;
    nextRefresh_ = now + 2000;
    try {
        cached_ =
            @{@"devices" : project(provider_()),
              @"devicesFresh" : @YES,
              @"devicesError" : @""};
    } catch (const std::exception&) {
        cached_ = @{
            @"devices" : cached_[@"devices"] ?: @[],
            @"devicesFresh" : @NO,
            @"devicesError" :
                @"Input and output devices could not be refreshed. Retrying automatically."
        };
    }
    return cached_;
}
void DeviceInventory::invalidate() noexcept {
    nextRefresh_ = 0;
    if (async_) {
        std::lock_guard lock(async_->mutex);
        ++async_->generation;
        async_->completed.reset();
    }
}
DeviceBufferRequest decodeDeviceBufferRequest(std::string_view request) {
    if (request.empty() || request.size() > 8192)
        throw std::invalid_argument("Invalid buffer request size");
    auto* data = [NSData dataWithBytes:request.data() length:request.size()];
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![value isKindOfClass:NSDictionary.class] || [value count] != 4 ||
        ![value[@"uid"] isKindOfClass:NSString.class])
        throw std::invalid_argument("Invalid buffer request");
    for (NSString* key in value)
        if (![@[ @"version", @"uid", @"expectedFrames", @"frames" ] containsObject:key])
            throw std::invalid_argument("Unknown buffer request field");
    const auto integer = [&](NSString* key) -> uint32_t {
        id number = value[key];
        if (![number isKindOfClass:NSNumber.class] ||
            CFGetTypeID((__bridge CFTypeRef)number) == CFBooleanGetTypeID())
            throw std::invalid_argument("A buffer request requires integer values");
        const auto numeric = [number doubleValue];
        if (!std::isfinite(numeric) || numeric < 1 || numeric > 4096 ||
            std::floor(numeric) != numeric)
            throw std::invalid_argument("A buffer request value is outside its bounds");
        return static_cast<uint32_t>(numeric);
    };
    if (integer(@"version") != 1)
        throw std::invalid_argument("Unsupported buffer request version");
    NSString* uid = value[@"uid"];
    const auto bytes = [uid lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    const auto* utf8 = uid.UTF8String;
    if (!utf8 || !bytes || bytes > 4096)
        throw std::invalid_argument("Invalid audio device identity");
    const auto frames = integer(@"frames");
    return {std::string(utf8, bytes), integer(@"expectedFrames"), frames};
}
std::string DeviceInventory::applyBufferRequest(std::string_view request) {
    const auto value = decodeDeviceBufferRequest(request);
    setBufferFrames(value.uid, value.expectedFrames, value.frames);
    return "{\"version\":1,\"ok\":true,\"bufferFrames\":" + std::to_string(value.frames) + "}";
}
} // namespace loopz::macos
