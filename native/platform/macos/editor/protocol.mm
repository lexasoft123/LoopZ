#import <Foundation/Foundation.h>
#include <loopz/macos/editor_protocol.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace loopz::macos {
namespace {
[[noreturn]] void invalid(const char* message) {
    throw std::invalid_argument(message);
}

NSDictionary* fields(id value, std::initializer_list<NSString*> required,
                     std::initializer_list<NSString*> optional = {}) {
    if (![value isKindOfClass:[NSDictionary class]])
        invalid("Editor command must be an object");
    for (NSString* name : required)
        if (!value[name])
            invalid("Editor command is missing a field");
    for (id name in value) {
        auto known = [&](auto names) {
            return std::any_of(names.begin(), names.end(), [&](NSString* candidate) {
                return [candidate isEqual:name];
            });
        };
        if (!known(required) && !known(optional))
            invalid("Editor command contains an unknown field");
    }
    return value;
}

NSArray* array(id value, size_t maximum) {
    if (![value isKindOfClass:[NSArray class]] || [value count] > maximum)
        invalid("Editor array has an invalid type or size");
    return value;
}

std::string text(id value, size_t maximum = 4096, bool allowEmpty = false) {
    if (![value isKindOfClass:[NSString class]])
        invalid("Editor text must be a string");
    const auto length = [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    const char* bytes = [value UTF8String];
    if (!bytes || length > maximum || (!length && !allowEmpty))
        invalid("Editor text has an invalid length");
    std::string result(bytes, length);
    if (result.find('\0') != std::string::npos)
        invalid("Editor text contains a null character");
    return result;
}

double number(id value) {
    if (![value isKindOfClass:[NSNumber class]] ||
        CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID())
        invalid("Editor number has an invalid type");
    const auto result = [value doubleValue];
    if (!std::isfinite(result))
        invalid("Editor number must be finite");
    return result;
}

uint32_t integer(id value) {
    const auto result = number(value);
    if (result < 0 || result > UINT32_MAX || std::floor(result) != result)
        invalid("Editor integer is outside its range");
    return static_cast<uint32_t>(result);
}

float gain(id value) {
    const auto result = number(value);
    if (result < 0 || result > 1)
        invalid("Editor gain must be between zero and one");
    return static_cast<float>(result);
}

bool boolean(id value) {
    if (!value || CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID())
        invalid("Editor flag must be a boolean");
    return [value boolValue];
}

uint64_t generation(id value) {
    const auto digits = text(value, 20);
    if ((digits.size() > 1 && digits.front() == '0') ||
        !std::all_of(digits.begin(), digits.end(), [](char c) {
            return c >= '0' && c <= '9';
        }))
        invalid("Editor generation must be a canonical unsigned decimal string");
    uint64_t result;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
        invalid("Editor generation is outside its range");
    return result;
}

edit::Controls controls(id value) {
    const auto object = fields(value, {}, {@"gain", @"enabled", @"muted"});
    edit::Controls result;
    if (object[@"gain"])
        result.gain = gain(object[@"gain"]);
    if (object[@"enabled"])
        result.enabled = boolean(object[@"enabled"]);
    if (object[@"muted"])
        result.muted = boolean(object[@"muted"]);
    return result;
}

std::vector<ChannelConnection> connections(id value) {
    std::vector<ChannelConnection> result;
    for (id entry in array(value, kMaximumRoutingChannels * kMaximumRoutingChannels)) {
        const auto wire = fields(entry, {@"sourceChannel", @"outputChannel", @"gain"});
        result.push_back({integer(wire[@"sourceChannel"]), integer(wire[@"outputChannel"]),
                          gain(wire[@"gain"])});
    }
    return result;
}

std::vector<std::string> identities(id value, size_t maximum) {
    std::vector<std::string> result;
    for (id entry in array(value, maximum))
        result.push_back(text(entry));
    return result;
}

SourceKind sourceKind(id value) {
    const auto kind = text(value);
    if (kind == "application")
        return SourceKind::Application;
    if (kind == "input-device")
        return SourceKind::InputDevice;
    if (kind == "virtual-device")
        return SourceKind::VirtualDevice;
    if (kind == "pass-through")
        return SourceKind::PassThrough;
    invalid("Unknown editor source kind");
}

edit::Command command(id value) {
    using namespace edit;
    if (![value isKindOfClass:[NSDictionary class]])
        invalid("Editor command must be an object");
    const auto type = text(value[@"type"]);
    if (type == "createDevice") {
        const auto item =
            fields(value, {@"type", @"id", @"name"}, {@"channels", @"sampleRate", @"passThrough"});
        CreateDevice result{text(item[@"id"]), text(item[@"name"])};
        if (item[@"channels"])
            result.channels = integer(item[@"channels"]);
        if (item[@"sampleRate"])
            result.sampleRate = number(item[@"sampleRate"]);
        if (item[@"passThrough"])
            result.passThrough = boolean(item[@"passThrough"]);
        return result;
    }
    if (type == "duplicateDevice") {
        const auto item = fields(value, {@"type", @"device", @"id", @"name"});
        return DuplicateDevice{text(item[@"device"]), text(item[@"id"]), text(item[@"name"])};
    }
    if (type == "renameDevice") {
        const auto item = fields(value, {@"type", @"device", @"name"});
        return RenameDevice{text(item[@"device"]), text(item[@"name"])};
    }
    if (type == "deleteDevice") {
        const auto item = fields(value, {@"type", @"device"});
        return DeleteDevice{text(item[@"device"])};
    }
    if (type == "resizeDevice") {
        const auto item = fields(value, {@"type", @"device", @"channels"});
        return ResizeDevice{text(item[@"device"]), integer(item[@"channels"])};
    }
    if (type == "removeOutputPair") {
        const auto item = fields(value, {@"type", @"device", @"firstChannel"});
        return RemoveOutputPair{text(item[@"device"]), integer(item[@"firstChannel"])};
    }
    if (type == "setDeviceControls") {
        const auto item = fields(value, {@"type", @"device", @"controls"}, {@"safetyLimiter"});
        SetDeviceControls result{text(item[@"device"]), controls(item[@"controls"]), {}};
        if (item[@"safetyLimiter"])
            result.safetyLimiter = boolean(item[@"safetyLimiter"]);
        return result;
    }
    if (type == "setDeviceRate") {
        const auto item = fields(value, {@"type", @"device", @"sampleRate"});
        return SetDeviceRate{text(item[@"device"]), number(item[@"sampleRate"])};
    }
    if (type == "addSource") {
        const auto item = fields(value, {@"type", @"device", @"id", @"kind", @"selector"},
                                 {@"channels", @"muteOriginal", @"connectByDefault"});
        AddSource result{text(item[@"device"]), text(item[@"id"]), sourceKind(item[@"kind"]),
                         text(item[@"selector"], 4096, true)};
        if (item[@"channels"])
            result.channels = integer(item[@"channels"]);
        if (item[@"muteOriginal"])
            result.muteOriginal = boolean(item[@"muteOriginal"]);
        if (item[@"connectByDefault"])
            result.connectByDefault = boolean(item[@"connectByDefault"]);
        return result;
    }
    if (type == "removeSource") {
        const auto item = fields(value, {@"type", @"device", @"source"});
        return RemoveSource{text(item[@"device"]), text(item[@"source"])};
    }
    if (type == "setSourceControls") {
        const auto item =
            fields(value, {@"type", @"device", @"source", @"controls"}, {@"muteOriginal"});
        SetSourceControls result{
            text(item[@"device"]), text(item[@"source"]), controls(item[@"controls"]), {}};
        if (item[@"muteOriginal"])
            result.muteOriginal = boolean(item[@"muteOriginal"]);
        return result;
    }
    if (type == "setSourceWiring") {
        const auto item = fields(value, {@"type", @"device", @"source", @"connections"});
        return SetSourceWiring{text(item[@"device"]), text(item[@"source"]),
                               connections(item[@"connections"])};
    }
    if (type == "setInputChannels") {
        const auto item = fields(value, {@"type", @"device", @"source", @"channels"});
        return SetInputChannels{text(item[@"device"]), text(item[@"source"]),
                                integer(item[@"channels"])};
    }
    if (type == "setSourceSelector") {
        const auto item = fields(value, {@"type", @"device", @"source", @"selector"});
        return SetSourceSelector{text(item[@"device"]), text(item[@"source"]),
                                 text(item[@"selector"])};
    }
    if (type == "addMonitor") {
        const auto item = fields(value, {@"type", @"device", @"id", @"selector"},
                                 {@"channels", @"connectByDefault"});
        AddMonitor result{text(item[@"device"]), text(item[@"id"]), text(item[@"selector"])};
        if (item[@"channels"])
            result.channels = integer(item[@"channels"]);
        if (item[@"connectByDefault"])
            result.connectByDefault = boolean(item[@"connectByDefault"]);
        return result;
    }
    if (type == "removeMonitor") {
        const auto item = fields(value, {@"type", @"device", @"monitor"});
        return RemoveMonitor{text(item[@"device"]), text(item[@"monitor"])};
    }
    if (type == "setMonitorControls") {
        const auto item = fields(value, {@"type", @"device", @"monitor", @"controls"});
        return SetMonitorControls{text(item[@"device"]), text(item[@"monitor"]),
                                  controls(item[@"controls"])};
    }
    if (type == "setMonitorWiring") {
        const auto item = fields(value, {@"type", @"device", @"monitor", @"connections"});
        return SetMonitorWiring{text(item[@"device"]), text(item[@"monitor"]),
                                connections(item[@"connections"])};
    }
    if (type == "setMonitorEndpoint") {
        const auto item = fields(value, {@"type", @"device", @"monitor", @"selector", @"channels"});
        return SetMonitorEndpoint{text(item[@"device"]), text(item[@"monitor"]),
                                  text(item[@"selector"]), integer(item[@"channels"])};
    }
    if (type == "reorderDevices") {
        const auto item = fields(value, {@"type", @"ids"});
        return ReorderDevices{identities(item[@"ids"], kMaximumVirtualDevices)};
    }
    if (type == "reorderSources") {
        const auto item = fields(value, {@"type", @"device", @"ids"});
        return ReorderSources{text(item[@"device"]),
                              identities(item[@"ids"], kMaximumRoutingSources)};
    }
    if (type == "reorderMonitors") {
        const auto item = fields(value, {@"type", @"device", @"ids"});
        return ReorderMonitors{text(item[@"device"]), identities(item[@"ids"], 32)};
    }
    invalid("Unknown editor command");
}

NSString* string(const std::string& value) {
    auto* result = [[NSString alloc] initWithBytes:value.data()
                                            length:value.size()
                                          encoding:NSUTF8StringEncoding];
    if (!result)
        invalid("Editor response text is not UTF-8");
    return result;
}

NSDictionary* revision(const EditorRevision& value) {
    return
        @{@"epoch" : string(value.epoch), @"generation" : string(std::to_string(value.generation))};
}

NSString* phase(RestorationPhase value) {
    switch (value) {
    case RestorationPhase::NoDesiredDocument:
        return @"no-document";
    case RestorationPhase::WaitingForDriver:
        return @"waiting-for-driver";
    case RestorationPhase::Reconciling:
        return @"reconciling";
    case RestorationPhase::Applying:
        return @"applying";
    case RestorationPhase::Cancelling:
        return @"cancelling";
    case RestorationPhase::Retry:
        return @"retry";
    case RestorationPhase::Restored:
        return @"restored";
    }
    invalid("Unknown editor restoration phase");
}

std::string json(id object) {
    NSError* error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:object
                                                   options:NSJSONWritingSortedKeys
                                                     error:&error];
    if (!data)
        throw std::runtime_error("Editor response could not be encoded");
    return {static_cast<const char*>(data.bytes), data.length};
}
} // namespace

EditorRequest decodeEditorRequest(std::string_view input) {
    @autoreleasepool {
        if (input.empty() || input.size() > kMaximumEditorRequestBytes)
            invalid("Editor request is empty or exceeds 1 MiB");
        NSError* error = nil;
        id parsed = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:input.data()
                                                                           length:input.size()]
                                                    options:0
                                                      error:&error];
        const bool history = [parsed isKindOfClass:NSDictionary.class] && parsed[@"history"];
        const auto root = history ? fields(parsed, {@"version", @"expected", @"history"})
                                  : fields(parsed, {@"version", @"expected", @"commands"});
        if (integer(root[@"version"]) != kEditorProtocolVersion)
            invalid("Unsupported editor protocol version");
        const auto expected = fields(root[@"expected"], {@"epoch", @"generation"});
        EditorRequest result{{text(expected[@"epoch"], 128), generation(expected[@"generation"])},
                             {}};
        if (history) {
            const auto direction = text(root[@"history"], 4);
            if (direction == "undo")
                result.history = EditorHistoryDirection::Undo;
            else if (direction == "redo")
                result.history = EditorHistoryDirection::Redo;
            else
                invalid("Unknown editor history action");
            return result;
        }
        const auto commands = array(root[@"commands"], edit::kMaximumCommands);
        if (!commands.count)
            invalid("Editor request must contain a command");
        for (id item in commands)
            result.commands.push_back(command(item));
        return result;
    }
}

std::string encodeEditorSnapshot(const EditorSnapshot& snapshot) {
    @autoreleasepool {
        id document = [NSNull null];
        if (snapshot.document) {
            const auto encoded = encodeRouteDocumentJSON(*snapshot.document);
            document = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:encoded.data()
                                                                              length:encoded.size()]
                                                       options:0
                                                         error:nil];
            if (!document)
                throw std::runtime_error("Routing document could not be encoded for the editor");
        }
        const auto& status = snapshot.restoration;
        return json(@{
            @"version" : @(kEditorProtocolVersion),
            @"revision" : revision(snapshot.revision),
            @"document" : document,
            @"canCreateDocument" : @(snapshot.canCreateDocument),
            @"history" : @{
                @"canUndo" : @(snapshot.history.canUndo),
                @"canRedo" : @(snapshot.history.canRedo)
            },
            @"restoration" : @{
                @"phase" : phase(status.phase),
                @"desiredGeneration" : string(std::to_string(status.desiredGeneration)),
                @"activeGeneration" : string(std::to_string(status.activeGeneration)),
                @"transactionRevision" : string(std::to_string(status.transactionRevision)),
                @"error" : @(status.error),
                @"detail" : string(status.detail),
                @"recoveredBackup" : @(status.recoveredBackup)
            }
        });
    }
}

std::string encodeEditorCommit(const EditorCommit& commit) {
    @autoreleasepool {
        return json(@{
            @"version" : @(kEditorProtocolVersion),
            @"revision" : revision(commit.revision),
            @"changed" : @(commit.changed)
        });
    }
}
} // namespace loopz::macos
