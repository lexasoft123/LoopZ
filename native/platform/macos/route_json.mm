#import <Foundation/Foundation.h>
#include <loopz/macos/route_store.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace loopz::macos {
namespace {
[[noreturn]] void invalid(const char* message) {
    throw std::invalid_argument(message);
}

NSDictionary* object(id value, size_t fields) {
    if (![value isKindOfClass:[NSDictionary class]] || [value count] != fields)
        invalid("Routing object has missing or unknown fields");
    return value;
}

NSArray* array(id value, size_t maximum) {
    if (![value isKindOfClass:[NSArray class]] || [value count] > maximum)
        invalid("Routing array has an invalid type or size");
    return value;
}

std::string text(id value) {
    if (![value isKindOfClass:[NSString class]])
        invalid("Routing text has an invalid type");
    const char* bytes = [value UTF8String];
    if (!bytes || [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 4096)
        invalid("Routing text is invalid or too long");
    return {bytes, [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding]};
}

double number(id value) {
    if (![value isKindOfClass:[NSNumber class]] ||
        CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID())
        invalid("Routing number has an invalid type");
    const double result = [value doubleValue];
    if (!std::isfinite(result))
        invalid("Routing number must be finite");
    return result;
}

uint32_t integer(id value) {
    const double result = number(value);
    if (result < 0 || result > std::numeric_limits<uint32_t>::max() || std::floor(result) != result)
        invalid("Routing integer is outside its range");
    return static_cast<uint32_t>(result);
}

bool boolean(id value) {
    if (!value || CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID())
        invalid("Routing flag must be a boolean");
    return [value boolValue];
}

NSString* string(const std::string& value) {
    NSString* result = [[NSString alloc] initWithBytes:value.data()
                                                length:value.size()
                                              encoding:NSUTF8StringEncoding];
    if (!result)
        invalid("Routing text must use valid UTF-8");
    return result;
}

std::vector<ChannelConnection> decodeConnections(id value) {
    std::vector<ChannelConnection> result;
    for (id item in array(value, kMaximumRoutingChannels * kMaximumRoutingChannels)) {
        const auto wire = object(item, 3);
        result.push_back({integer(wire[@"sourceChannel"]), integer(wire[@"outputChannel"]),
                          static_cast<float>(number(wire[@"gain"]))});
    }
    return result;
}

NSArray* encodeConnections(const std::vector<ChannelConnection>& connections) {
    auto* result = [NSMutableArray arrayWithCapacity:connections.size()];
    for (const auto& wire : connections)
        [result addObject:@{
            @"sourceChannel" : @(wire.sourceChannel),
            @"outputChannel" : @(wire.outputChannel),
            @"gain" : @(wire.gain)
        }];
    return result;
}

SourceKind decodeKind(id value) {
    const auto name = text(value);
    if (name == "application")
        return SourceKind::Application;
    if (name == "input-device")
        return SourceKind::InputDevice;
    if (name == "virtual-device")
        return SourceKind::VirtualDevice;
    if (name == "pass-through")
        return SourceKind::PassThrough;
    invalid("Unknown routing source kind");
}

NSString* encodeKind(SourceKind kind) {
    switch (kind) {
    case SourceKind::Application:
        return @"application";
    case SourceKind::InputDevice:
        return @"input-device";
    case SourceKind::VirtualDevice:
        return @"virtual-device";
    case SourceKind::PassThrough:
        return @"pass-through";
    }
    invalid("Unknown routing source kind");
}

RoutingConfiguration decodeRouting(id value) {
    const auto item = object(value, 6);
    RoutingConfiguration result;
    result.outputChannels = integer(item[@"outputChannels"]);
    result.gain = static_cast<float>(number(item[@"gain"]));
    result.enabled = boolean(item[@"enabled"]);
    result.muted = boolean(item[@"muted"]);
    result.safetyLimiter = boolean(item[@"safetyLimiter"]);
    for (id entry in array(item[@"sources"], kMaximumRoutingSources)) {
        const auto source = object(entry, 6);
        result.sources.push_back({text(source[@"id"]), integer(source[@"channels"]),
                                  static_cast<float>(number(source[@"gain"])),
                                  boolean(source[@"enabled"]), boolean(source[@"muted"]),
                                  decodeConnections(source[@"connections"])});
    }
    return result;
}

NSDictionary* encodeRouting(const RoutingConfiguration& routing) {
    auto* sources = [NSMutableArray arrayWithCapacity:routing.sources.size()];
    for (const auto& source : routing.sources)
        [sources addObject:@{
            @"id" : string(source.id),
            @"channels" : @(source.channels),
            @"gain" : @(source.gain),
            @"enabled" : @(source.enabled),
            @"muted" : @(source.muted),
            @"connections" : encodeConnections(source.connections)
        }];
    return @{
        @"outputChannels" : @(routing.outputChannels),
        @"gain" : @(routing.gain),
        @"enabled" : @(routing.enabled),
        @"muted" : @(routing.muted),
        @"safetyLimiter" : @(routing.safetyLimiter),
        @"sources" : sources
    };
}
} // namespace

RouteDocument decodeRouteDocumentJSON(std::string_view json) {
    @autoreleasepool {
        if (json.empty() || json.size() > kMaximumRouteFileBytes)
            invalid("Routing document is empty or exceeds 16 MiB");
        NSData* data = [NSData dataWithBytes:json.data() length:json.size()];
        NSError* error = nil;
        id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        if (!parsed)
            invalid("Routing document is not valid JSON");
        if (![parsed isKindOfClass:[NSDictionary class]])
            invalid("Routing document must be an object");
        RouteDocument document;
        document.version = integer(parsed[@"version"]);
        // A newer schema may add or remove root fields. Identify its version
        // before applying this version's exact-field validation, so storage
        // never mistakes a future document for corruption and downgrades it.
        if (document.version != 1 && document.version != kRouteDocumentVersion)
            throw UnsupportedRouteVersion(document.version);
        const auto root = object(parsed, 2);
        for (id entry in array(root[@"devices"], kMaximumVirtualDevices)) {
            const auto item = object(entry, document.version >= 2 ? 7 : 6);
            VirtualDeviceDefinition device;
            device.id = text(item[@"id"]);
            device.name = text(item[@"name"]);
            device.sampleRate = number(item[@"sampleRate"]);
            if (document.version >= 2)
                device.formatRequestReceipt = text(item[@"formatRequestReceipt"]);
            device.routing = decodeRouting(item[@"routing"]);
            for (id value in array(item[@"bindings"], kMaximumRoutingSources)) {
                const auto binding = object(value, 4);
                device.bindings.push_back({text(binding[@"id"]), decodeKind(binding[@"kind"]),
                                           text(binding[@"selector"]),
                                           boolean(binding[@"muteOriginal"])});
            }
            for (id value in array(item[@"monitors"], 32)) {
                const auto monitor = object(value, 7);
                device.monitors.push_back({text(monitor[@"id"]), text(monitor[@"deviceSelector"]),
                                           integer(monitor[@"channels"]),
                                           static_cast<float>(number(monitor[@"gain"])),
                                           boolean(monitor[@"enabled"]), boolean(monitor[@"muted"]),
                                           decodeConnections(monitor[@"connections"])});
            }
            document.devices.push_back(std::move(device));
        }
        validateRouteDocument(document);
        return document;
    }
}

std::string encodeRouteDocumentJSON(const RouteDocument& document) {
    @autoreleasepool {
        validateRouteDocument(document);
        auto* devices = [NSMutableArray arrayWithCapacity:document.devices.size()];
        for (const auto& device : document.devices) {
            auto* bindings = [NSMutableArray arrayWithCapacity:device.bindings.size()];
            for (const auto& binding : device.bindings)
                [bindings addObject:@{
                    @"id" : string(binding.id),
                    @"kind" : encodeKind(binding.kind),
                    @"selector" : string(binding.selector),
                    @"muteOriginal" : @(binding.muteOriginal)
                }];
            auto* monitors = [NSMutableArray arrayWithCapacity:device.monitors.size()];
            for (const auto& monitor : device.monitors)
                [monitors addObject:@{
                    @"id" : string(monitor.id),
                    @"deviceSelector" : string(monitor.deviceSelector),
                    @"channels" : @(monitor.channels),
                    @"gain" : @(monitor.gain),
                    @"enabled" : @(monitor.enabled),
                    @"muted" : @(monitor.muted),
                    @"connections" : encodeConnections(monitor.connections)
                }];
            auto* encoded = [NSMutableDictionary dictionaryWithDictionary:@{
                @"id" : string(device.id),
                @"name" : string(device.name),
                @"sampleRate" : @(device.sampleRate),
                @"routing" : encodeRouting(device.routing),
                @"bindings" : bindings,
                @"monitors" : monitors
            }];
            if (document.version >= 2)
                encoded[@"formatRequestReceipt"] = string(device.formatRequestReceipt);
            [devices addObject:encoded];
        }
        NSError* error = nil;
        NSData* result = [NSJSONSerialization
            dataWithJSONObject:@{
                @"version" : @(document.version),
                @"devices" : devices
            }
                       options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                         error:&error];
        if (!result || result.length > kMaximumRouteFileBytes)
            invalid("Routing document cannot be encoded within 16 MiB");
        return {static_cast<const char*>(result.bytes), result.length};
    }
}
} // namespace loopz::macos
