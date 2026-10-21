#include <loopz/macos/device_settings.hpp>
#include <loopz/macos/device_inventory.hpp>
#include <loopz/macos/driver_protocol.hpp>
#include <loopz/macos/local_file.hpp>
#include <loopz/timeline.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace loopz::macos {
namespace {
using Preferences = std::map<std::string, uint32_t>;
constexpr size_t kMaximumBytes = 1024 * 1024, kMaximumDevices = 64;
struct UnsupportedSettings : std::runtime_error {
    UnsupportedSettings() : std::runtime_error("Audio settings were saved by a newer version") {}
};
void validate(const std::string& uid, uint32_t frames) {
    if (uid.empty() || uid.size() > 4096 || uid.find('\0') != std::string::npos ||
        uid == kDeviceUID || uid.starts_with("com.lexasoft.loopz.device.") || !frames ||
        frames > 4096 || (frames & (frames - 1)))
        throw std::invalid_argument("Invalid saved device buffer");
}
id parsed(std::string_view bytes) {
    auto* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    id result = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (!result)
        throw std::invalid_argument("Audio settings are not valid JSON");
    return result;
}
std::string encoded(id value) {
    auto* data = [NSJSONSerialization dataWithJSONObject:value
                                                 options:NSJSONWritingSortedKeys
                                                   error:nil];
    if (!data || data.length > kMaximumBytes)
        throw std::invalid_argument("Audio settings exceed their size limit");
    return {static_cast<const char*>(data.bytes), data.length};
}
Preferences decode(std::string_view bytes, uint32_t* routingFrames = nullptr) {
    id value = parsed(bytes);
    if (![value isKindOfClass:NSDictionary.class])
        throw std::invalid_argument("Invalid audio settings document");
    id version = value[@"version"];
    if (![version isKindOfClass:NSNumber.class] ||
        CFGetTypeID((__bridge CFTypeRef)version) == CFBooleanGetTypeID() ||
        [version doubleValue] < 1 || [version doubleValue] != [version unsignedIntValue])
        throw std::invalid_argument("Invalid audio settings version");
    if ([version unsignedIntValue] > 2)
        throw UnsupportedSettings();
    const bool global = [version unsignedIntValue] == 2;
    if ([value count] != (global ? 3 : 2) || ![value[@"buffers"] isKindOfClass:NSArray.class] ||
        [value[@"buffers"] count] > kMaximumDevices)
        throw std::invalid_argument("Invalid audio settings document");
    uint32_t frames = 0;
    if (global) {
        const auto request = decodeDeviceBufferRequest(encoded(@{
            @"version" : @1,
            @"uid" : @"routing",
            @"expectedFrames" : @1,
            @"frames" : value[@"routingBufferFrames"] ?: [NSNull null]
        }));
        validate(request.uid, request.frames);
        frames = request.frames;
    }
    if (routingFrames)
        *routingFrames = frames;
    Preferences result;
    for (id entry in value[@"buffers"]) {
        if (![entry isKindOfClass:NSDictionary.class] || [entry count] != 2)
            throw std::invalid_argument("Invalid saved buffer entry");
        const auto request = decodeDeviceBufferRequest(encoded(@{
            @"version" : @1,
            @"uid" : entry[@"uid"] ?: [NSNull null],
            @"expectedFrames" : @1,
            @"frames" : entry[@"frames"] ?: [NSNull null]
        }));
        validate(request.uid, request.frames);
        if (!result.emplace(request.uid, request.frames).second)
            throw std::invalid_argument("Duplicate saved device buffer");
    }
    return result;
}
std::string encode(const Preferences& desired, uint32_t routingFrames) {
    if (desired.size() > kMaximumDevices)
        throw std::invalid_argument("Too many saved device buffers");
    auto* entries = [NSMutableArray array];
    for (const auto& [uid, frames] : desired) {
        validate(uid, frames);
        [entries addObject:@{@"uid" : @(uid.c_str()), @"frames" : @(frames)}];
    }
    if (routingFrames)
        return encoded(
            @{@"version" : @2,
              @"buffers" : entries,
              @"routingBufferFrames" : @(routingFrames)});
    return encoded(@{@"version" : @1, @"buffers" : entries});
}
bool present(const std::filesystem::path& path) {
    return std::filesystem::symlink_status(path).type() != std::filesystem::file_type::not_found;
}
void requireRegular(const std::filesystem::path& path) {
    const auto type = std::filesystem::symlink_status(path).type();
    if (type != std::filesystem::file_type::not_found &&
        type != std::filesystem::file_type::regular)
        throw std::runtime_error("Audio settings path is not a regular file");
}
} // namespace

struct DeviceSettings::Impl {
    const std::filesystem::path path;
    DeviceInventory inventory;
    Preferences desired;
    uint32_t routingFrames{0};
    std::vector<std::pair<std::string, bool>> routingSelectors;
    Preferences effective() const {
        if (!routingFrames)
            return desired;
        Preferences result;
        for (const auto& [selector, input] : routingSelectors) {
            const auto uid = resolve(selector, input);
            if (!uid.empty() && uid != kDeviceUID && !uid.starts_with("com.lexasoft.loopz.device."))
                result[uid] = routingFrames;
        }
        return result;
    }
    std::string resolve(std::string selector, bool input) const {
        if (selector == "default") {
            for (NSDictionary* device in latest[@"devices"])
                if ([device[input ? @"defaultInput" : @"defaultOutput"] boolValue])
                    return [device[@"uid"] UTF8String];
            return input ? "default-input" : "default-output";
        }
        return selector;
    }
    std::map<std::string, bool> applied;
    std::map<std::string, std::string> failures;
    std::string loadError;
    bool recoveredBackup{false}, loaded{false};
    uint64_t nextPoll{0}, lastPoll{0}, hardwareGeneration{0};
    NSDictionary* latest{nil};
    Impl(std::filesystem::path file, Provider provider, Writer writer)
        : path(std::move(file)), inventory(std::move(provider), std::move(writer)) {}
    void load() {
        inventory.invalidate();
        if (path.empty()) {
            loaded = true;
            return;
        }
        try {
            const auto backup = std::filesystem::path(path.string() + ".backup");
            if (!present(path) && !present(backup)) {
                desired.clear();
                routingFrames = 0;
                recoveredBackup = false;
            } else {
                try {
                    desired = decode(readLocalFile(path, kMaximumBytes), &routingFrames);
                    recoveredBackup = false;
                } catch (const UnsupportedSettings&) {
                    throw;
                } catch (const std::exception&) {
                    desired = decode(readLocalFile(backup, kMaximumBytes), &routingFrames);
                    recoveredBackup = true;
                }
            }
            loadError.clear();
            loaded = true;
            applied.clear();
            failures.clear();
        } catch (const std::exception& error) {
            loadError = error.what();
            loaded = false;
        }
    }
    void save(const Preferences& candidate, uint32_t frames) {
        const auto bytes = encode(candidate, frames);
        const auto backup = std::filesystem::path(path.string() + ".backup");
        requireRegular(path);
        requireRegular(backup);
        // A future version in either slot must never be overwritten by this Engine.
        if (present(backup)) {
            try {
                (void)decode(readLocalFile(backup, kMaximumBytes));
            } catch (const std::invalid_argument&) {
            }
        }
        if (present(path)) {
            const auto previous = readLocalFile(path, kMaximumBytes);
            try {
                (void)decode(previous);
                writeLocalFileAtomic(backup, previous);
            } catch (const std::invalid_argument&) {
                // Keep the recovered valid backup when the primary is corrupt.
            }
        }
        writeLocalFileAtomic(path, bytes);
    }
    void reconcile(uint64_t now) {
        lastPoll = now;
        if (now < nextPoll)
            return;
        nextPoll = now + 2000;
        if (!loaded)
            load();
        if (!loaded)
            return;
        latest = inventory.snapshot(now);
        if ([latest[@"devicesRefreshing"] boolValue]) {
            // A read-only refresh cannot revoke an already-applied preference
            // or write a buffer from stale metadata. New preferences remain
            // unconfirmed until a current-generation discovery completes.
            nextPoll = now + 50;
            return;
        }
        for (const auto& [uid, frames] : effective()) {
            applied[uid] = false;
            failures[uid] = "Saved device is unavailable; waiting to restore its buffer";
            if (![latest[@"devicesFresh"] boolValue])
                continue;
            for (NSDictionary* device in latest[@"devices"])
                if ([device[@"uid"] isEqualToString:@(uid.c_str())]) {
                    try {
                        const auto current = [device[@"bufferFrames"] unsignedIntValue];
                        if (current != frames) {
                            inventory.setBufferFrames(uid, current, frames);
                            ++hardwareGeneration;
                        }
                        applied[uid] = true;
                        failures[uid].clear();
                    } catch (const std::exception& error) {
                        failures[uid] = error.what();
                    }
                    break;
                }
        }
    }
    bool ready(std::string selector, bool input) const {
        selector = resolve(std::move(selector), input);
        if (!effective().contains(selector))
            return true;
        const auto state = applied.find(selector);
        return state != applied.end() && state->second;
    }
};

DeviceSettings::DeviceSettings(std::filesystem::path path, Provider provider, Writer writer)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(provider), std::move(writer))) {}
DeviceSettings::~DeviceSettings() = default;
void DeviceSettings::reload() {
    impl_->nextPoll = 0;
    impl_->load();
}
void DeviceSettings::setRoutingDocument(const RouteDocument& document) {
    std::vector<std::pair<std::string, bool>> selectors;
    for (const auto& device : document.devices) {
        if (!device.routing.enabled)
            continue;
        for (const auto& binding : device.bindings) {
            const auto source = std::find_if(device.routing.sources.begin(),
                                             device.routing.sources.end(), [&](const auto& value) {
                                                 return value.id == binding.id;
                                             });
            if (binding.kind == SourceKind::InputDevice && source != device.routing.sources.end() &&
                source->enabled)
                selectors.emplace_back(binding.selector, true);
        }
        for (const auto& monitor : device.monitors)
            if (monitor.enabled)
                selectors.emplace_back(monitor.deviceSelector, false);
    }
    std::sort(selectors.begin(), selectors.end());
    selectors.erase(std::unique(selectors.begin(), selectors.end()), selectors.end());
    if (selectors != impl_->routingSelectors) {
        impl_->routingSelectors = std::move(selectors);
        impl_->nextPoll = 0;
    }
}
void DeviceSettings::poll(uint64_t now) {
    impl_->reconcile(now);
}
void DeviceSettings::retryNow() noexcept {
    impl_->nextPoll = 0;
    impl_->inventory.invalidate();
}
std::string DeviceSettings::snapshot(uint64_t now) {
    auto& self = *impl_;
    self.reconcile(now);
    self.latest = self.inventory.snapshot(now);
    NSMutableDictionary* result = [self.latest mutableCopy];
    auto* entries = [NSMutableArray array];
    for (const auto& [uid, frames] : self.effective()) {
        const auto found = self.applied.find(uid);
        [entries addObject:@{
            @"uid" : @(uid.c_str()),
            @"frames" : @(frames),
            @"applied" : @(found != self.applied.end() && found->second),
            @"error" : @(self.failures[uid].c_str())
        }];
    }
    result[@"bufferPreferences"] = entries;
    result[@"routingBufferSettings"] = @YES;
    result[@"routingBufferFrames"] = @(self.routingFrames);
    auto* targets = [NSMutableArray array];
    std::set<std::string> seen;
    for (const auto& [selector, input] : self.routingSelectors) {
        const auto uid = self.resolve(selector, input);
        if (uid.empty() || uid == kDeviceUID || uid.starts_with("com.lexasoft.loopz.device.") ||
            !seen.insert(uid).second)
            continue;
        [targets addObject:@(uid.c_str())];
    }
    result[@"routingBufferDevices"] = targets;
    result[@"bufferSettingsPersistence"] = @(!self.path.empty());
    result[@"bufferSettingsError"] = @(self.loadError.c_str());
    result[@"bufferSettingsRecoveredBackup"] = @(self.recoveredBackup);
    return encoded(result);
}
std::string DeviceSettings::apply(std::string_view json) {
    auto& self = *impl_;
    if (json.empty() || json.size() > 8192)
        throw std::invalid_argument("Invalid buffer request size");
    id value = parsed(json);
    if ([value isKindOfClass:NSDictionary.class] && [value[@"version"] isEqual:@2]) {
        if ([value count] != 2)
            throw std::invalid_argument("Invalid routing buffer request");
        const auto request = decodeDeviceBufferRequest(encoded(@{
            @"version" : @1,
            @"uid" : @"routing",
            @"expectedFrames" : @1,
            @"frames" : value[@"frames"] ?: [NSNull null]
        }));
        validate(request.uid, request.frames);
        if (!self.loaded)
            self.load();
        if (!self.loaded)
            throw std::runtime_error("Saved audio settings need recovery before changing buffers");
        self.latest = self.inventory.snapshot(self.lastPoll);
        if (![self.latest[@"devicesFresh"] boolValue] ||
            [self.latest[@"devicesRefreshing"] boolValue])
            throw std::runtime_error("Waiting for current audio device information");
        // Validate every connected target before persisting or changing any hardware.
        std::set<std::string> targets;
        for (const auto& [selector, input] : self.routingSelectors)
            targets.insert(self.resolve(selector, input));
        for (NSDictionary* device in self.latest[@"devices"]) {
            const std::string uid = [device[@"uid"] UTF8String];
            if (targets.contains(uid) && uid != kDeviceUID &&
                !uid.starts_with("com.lexasoft.loopz.device."))
                self.inventory.validateBufferFrames(uid, [device[@"bufferFrames"] unsignedIntValue],
                                                    request.frames);
        }
        if (!self.path.empty())
            self.save(self.desired, request.frames);
        self.routingFrames = request.frames;
        self.applied.clear();
        self.failures.clear();
        self.recoveredBackup = false;
        self.nextPoll = 0;
        self.reconcile(self.lastPoll);
        bool applied = true;
        for (const auto& [uid, frames] : self.effective())
            applied = applied && self.applied[uid];
        return encoded(@{
            @"version" : @2,
            @"ok" : @YES,
            @"saved" : @(!self.path.empty()),
            @"applied" : @(applied),
            @"bufferFrames" : @(request.frames),
            @"detail" : applied ? @"" : @"Saved; waiting to restore device buffers"
        });
    }
    const auto request = decodeDeviceBufferRequest(json);
    if (!self.loaded)
        self.load();
    if (!self.loaded)
        throw std::runtime_error("Saved audio settings need recovery before changing buffers");
    self.inventory.validateBufferFrames(request.uid, request.expectedFrames, request.frames);
    if (self.path.empty())
        return self.inventory.applyBufferRequest(json);
    auto candidate = self.desired;
    candidate[request.uid] = request.frames;
    if (self.routingFrames)
        throw std::invalid_argument("Use the routing buffer setting for all used devices");
    self.save(candidate, 0); // Save accepted intent before effects or acknowledgement.
    self.desired.swap(candidate);
    self.recoveredBackup = false;
    bool applied = false;
    try {
        self.inventory.setBufferFrames(request.uid, request.expectedFrames, request.frames);
        if (request.expectedFrames != request.frames)
            ++self.hardwareGeneration;
        applied = true;
        self.failures[request.uid].clear();
    } catch (const std::exception& error) {
        // Acquisition/profile loss after validation retains the accepted intent.
        self.failures[request.uid] = error.what();
    }
    self.applied[request.uid] = applied;
    self.nextPoll = 0;
    return encoded(@{
        @"version" : @1,
        @"ok" : @YES,
        @"saved" : @YES,
        @"applied" : @(applied),
        @"bufferFrames" : @(request.frames),
        @"detail" : applied ? @"" : @"Saved; waiting to restore the device buffer"
    });
}
uint64_t DeviceSettings::hardwareGeneration() const noexcept {
    return impl_->hardwareGeneration;
}
bool DeviceSettings::restoredFor(const RouteDocument& document) const {
    const auto& self = *impl_;
    if (!document.devices.empty() && !self.loadError.empty())
        return false;
    for (const auto& device : document.devices) {
        if (!device.routing.enabled)
            continue;
        for (const auto& binding : device.bindings)
            if (binding.kind == SourceKind::InputDevice) {
                const auto source =
                    std::find_if(device.routing.sources.begin(), device.routing.sources.end(),
                                 [&](const auto& value) {
                                     return value.id == binding.id;
                                 });
                if (source != device.routing.sources.end() && source->enabled && !source->muted &&
                    source->gain > 0 && !source->connections.empty() &&
                    !self.ready(binding.selector, true))
                    return false;
            }
        for (const auto& monitor : device.monitors)
            if (monitor.enabled && !monitor.muted && monitor.gain > 0 &&
                !monitor.connections.empty() && !self.ready(monitor.deviceSelector, false))
                return false;
    }
    return true;
}
} // namespace loopz::macos
