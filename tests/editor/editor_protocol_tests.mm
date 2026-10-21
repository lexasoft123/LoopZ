#import <Foundation/Foundation.h>
#include <loopz/macos/editor_protocol.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <set>

namespace {
using namespace loopz;
using namespace loopz::macos;

std::string json(id object) {
    NSError* error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:object options:0 error:&error];
    assert(data);
    return {static_cast<const char*>(data.bytes), data.length};
}

id parsed(const std::string& value) {
    NSError* error = nil;
    id result = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:value.data()
                                                                       length:value.size()]
                                                options:NSJSONReadingMutableContainers
                                                  error:&error];
    assert(result);
    return result;
}

NSMutableDictionary* envelope(NSArray* commands) {
    return [@{
        @"version" : @1,
        @"expected" :
            [@{@"epoch" : @"engine-epoch", @"generation" : @"9007199254740993"} mutableCopy],
        @"commands" : commands
    } mutableCopy];
}

NSDictionary* rename() {
    return @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @"New Mix"};
}

template <typename Operation> void invalid(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void everyCommandAndRealDocument() {
    NSArray* commands = @[
        @{@"type" : @"createDevice",
          @"id" : @"mix",
          @"name" : @"混音 🎤",
          @"channels" : @4},
        @{@"type" : @"createDevice",
          @"id" : @"call",
          @"name" : @"Call",
          @"passThrough" : @NO},
        @{
            @"type" : @"addSource",
            @"device" : @"mix",
            @"id" : @"app",
            @"kind" : @"application",
            @"selector" : @"com.example.music",
            @"channels" : @1,
            @"muteOriginal" : @NO
        },
        @{
            @"type" : @"addSource",
            @"device" : @"mix",
            @"id" : @"mic",
            @"kind" : @"input-device",
            @"selector" : @"input-uid",
            @"channels" : @1
        },
        @{
            @"type" : @"addSource",
            @"device" : @"call",
            @"id" : @"stream",
            @"kind" : @"virtual-device",
            @"selector" : @"mix",
            @"connectByDefault" : @NO
        },
        @{
            @"type" : @"addMonitor",
            @"device" : @"mix",
            @"id" : @"phones",
            @"selector" : @"output-uid",
            @"channels" : @4,
            @"connectByDefault" : @NO
        },
        @{
            @"type" : @"setDeviceControls",
            @"device" : @"mix",
            @"controls" : @{@"gain" : @0.5, @"muted" : @NO},
            @"safetyLimiter" : @NO
        },
        @{@"type" : @"setDeviceRate",
          @"device" : @"mix",
          @"sampleRate" : @44100},
        @{
            @"type" : @"setSourceControls",
            @"device" : @"mix",
            @"source" : @"app",
            @"controls" : @{@"enabled" : @NO, @"gain" : @0.25, @"muted" : @YES},
            @"muteOriginal" : @YES
        },
        @{
            @"type" : @"setSourceWiring",
            @"device" : @"mix",
            @"source" : @"mic",
            @"connections" : @[
                @{@"sourceChannel" : @0,
                  @"outputChannel" : @3,
                  @"gain" : @0.5}
            ]
        },
        @{@"type" : @"setInputChannels",
          @"device" : @"mix",
          @"source" : @"mic",
          @"channels" : @3},
        @{
            @"type" : @"setSourceSelector",
            @"device" : @"mix",
            @"source" : @"app",
            @"selector" : @"other.app"
        },
        @{
            @"type" : @"setMonitorControls",
            @"device" : @"mix",
            @"monitor" : @"phones",
            @"controls" : @{@"gain" : @0.75, @"enabled" : @YES, @"muted" : @NO}
        },
        @{
            @"type" : @"setMonitorWiring",
            @"device" : @"mix",
            @"monitor" : @"phones",
            @"connections" : @[
                @{@"sourceChannel" : @3,
                  @"outputChannel" : @3,
                  @"gain" : @0.25}
            ]
        },
        @{
            @"type" : @"setMonitorEndpoint",
            @"device" : @"mix",
            @"monitor" : @"phones",
            @"selector" : @"other-output",
            @"channels" : @8
        },
        @{@"type" : @"resizeDevice",
          @"device" : @"mix",
          @"channels" : @8},
        @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @"Renamed"},
        @{@"type" : @"duplicateDevice", @"device" : @"mix", @"id" : @"copy", @"name" : @"Copied"},
        @{@"type" : @"reorderDevices",
          @"ids" : @[ @"copy", @"call", @"mix" ]},
        @{
            @"type" : @"reorderSources",
            @"device" : @"mix",
            @"ids" : @[ @"mic", @"app", @"pass-through" ]
        },
        @{@"type" : @"reorderMonitors",
          @"device" : @"mix",
          @"ids" : @[ @"phones" ]},
        @{@"type" : @"removeMonitor", @"device" : @"copy", @"monitor" : @"phones"},
        @{@"type" : @"resizeDevice",
          @"device" : @"call",
          @"channels" : @4},
        @{@"type" : @"removeOutputPair",
          @"device" : @"call",
          @"firstChannel" : @0},
        @{@"type" : @"removeSource", @"device" : @"call", @"source" : @"stream"},
        @{@"type" : @"deleteDevice", @"device" : @"call"},
    ];
    const auto request = decodeEditorRequest(json(envelope(commands)));
    assert(request.expected.generation == UINT64_C(9007199254740993));
    std::set<size_t> types;
    for (const auto& command : request.commands)
        types.insert(command.index());
    assert(types.size() == std::variant_size_v<edit::Command>);
    const auto document = edit::applyCommands({}, request.commands);
    assert(document.devices.size() == 2 && document.devices[0].id == "copy");
    const auto& mix = document.devices[1];
    assert(mix.id == "mix" && mix.name == "Renamed" && mix.sampleRate == 44100);
    assert(mix.routing.outputChannels == 8 && mix.routing.gain == 0.5f &&
           !mix.routing.safetyLimiter);
    assert(mix.routing.sources[0].id == "mic" && mix.routing.sources[0].channels == 3);
    assert(mix.routing.sources[0].connections[0].outputChannel == 3);
    assert(mix.routing.sources[0].connections[0].gain == 0.5f);
    assert(mix.bindings[1].selector == "other.app" && mix.bindings[1].muteOriginal);
    assert(mix.routing.sources[1].channels == 2 && !mix.routing.sources[1].enabled);
    assert(mix.monitors[0].channels == 8 && mix.monitors[0].connections[0].gain == 0.25f);
    assert(document.devices[0].monitors.empty());

    const auto pass = decodeEditorRequest(json(envelope(@[ @{
        @"type" : @"addSource",
        @"device" : @"mix",
        @"id" : @"pass",
        @"kind" : @"pass-through",
        @"selector" : @""
    } ])));
    assert(std::get<edit::AddSource>(pass.commands[0]).kind == SourceKind::PassThrough);
}

void strictEnvelopeAndCounters() {
    for (id version in @[ @YES, @0, @2, @1.5, @"1", [NSNull null] ]) {
        auto* value = envelope(@[ rename() ]);
        value[@"version"] = version;
        invalid([&] {
            decodeEditorRequest(json(value));
        });
    }
    for (id counter in @[
             @1, @YES, @"", @"01", @"-1", @"+1", @"1e3", @"1.0", @"18446744073709551616",
             [NSNull null]
         ]) {
        auto* value = envelope(@[ rename() ]);
        value[@"expected"][@"generation"] = counter;
        invalid([&] {
            decodeEditorRequest(json(value));
        });
    }
    auto* maximum = envelope(@[ rename() ]);
    maximum[@"expected"][@"generation"] = @"18446744073709551615";
    assert(decodeEditorRequest(json(maximum)).expected.generation == UINT64_MAX);
    maximum[@"expected"][@"generation"] = @"0";
    assert(decodeEditorRequest(json(maximum)).expected.generation == 0);
    auto* extra = envelope(@[ rename() ]);
    extra[@"path"] = @"/arbitrary/routes.json";
    invalid([&] {
        decodeEditorRequest(json(extra));
    });
    extra = envelope(@[ rename() ]);
    extra[@"expected"][@"extra"] = @YES;
    invalid([&] {
        decodeEditorRequest(json(extra));
    });
    extra = envelope(@[ rename() ]);
    [extra removeObjectForKey:@"expected"];
    invalid([&] {
        decodeEditorRequest(json(extra));
    });
    for (const std::string input : {"", "[]", "null", "{", "42"})
        invalid([&] {
            decodeEditorRequest(input);
        });
    invalid([&] {
        decodeEditorRequest(std::string(kMaximumEditorRequestBytes + 1, ' '));
    });
    invalid([&] {
        decodeEditorRequest(json(envelope(@[])));
    });
    auto* many = [NSMutableArray array];
    for (size_t index = 0; index <= edit::kMaximumCommands; ++index)
        [many addObject:rename()];
    invalid([&] {
        decodeEditorRequest(json(envelope(many)));
    });
}

void invalidCommandsAndFields() {
    NSArray* bad = @[
        @{@"type" : @"writeDocument",
          @"document" : @{}},
        @{
            @"type" : @"createDevice",
            @"id" : @"id",
            @"name" : @"Name",
            @"formatRequestReceipt" : @"injected"
        },
        @{@"type" : @"createDevice",
          @"id" : @"id",
          @"name" : @"Name",
          @"channels" : @YES},
        @{@"type" : @"resizeDevice",
          @"device" : @"mix",
          @"channels" : @2.5},
        @{@"type" : @"resizeDevice",
          @"device" : @"mix",
          @"channels" : @(-1)},
        @{@"type" : @"setDeviceControls",
          @"device" : @"mix",
          @"controls" : @{@"enabled" : @1}},
        @{@"type" : @"setDeviceControls",
          @"device" : @"mix",
          @"controls" : @{@"gain" : @YES}},
        @{
            @"type" : @"setDeviceControls",
            @"device" : @"mix",
            @"controls" : @{@"gain" : @1.000000001}
        },
        @{
            @"type" : @"setDeviceControls",
            @"device" : @"mix",
            @"controls" : @{@"gain" : @(-0.000000001)}
        },
        @{
            @"type" : @"setDeviceControls",
            @"device" : @"mix",
            @"controls" : @{@"gain" : [NSNull null]}
        },
        @{@"type" : @"setDeviceControls",
          @"device" : @"mix",
          @"controls" : @{@"unknown" : @NO}},
        @{
            @"type" : @"setSourceWiring",
            @"device" : @"mix",
            @"source" : @"mic",
            @"connections" : @[
                @{@"sourceChannel" : @0,
                  @"outputChannel" : @0}
            ]
        },
        @{
            @"type" : @"addSource",
            @"device" : @"mix",
            @"id" : @"app",
            @"kind" : @"unknown",
            @"selector" : @"uid"
        },
        @{@"type" : @"addMonitor", @"device" : @"mix", @"id" : @"monitor", @"selector" : @""},
        @{@"type" : @"renameDevice", @"device" : @"mix"},
        @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @""},
        @{@"type" : @"deleteDevice",
          @"device" : @"mix",
          @"unknown" : @NO},
        @{@"type" : @"reorderDevices",
          @"ids" : @[ @1 ]},
        @{},
        @[],
        [NSNull null]
    ];
    for (id command in bad)
        invalid([&] {
            decodeEditorRequest(json(envelope(@[ command ])));
        });
    NSMutableDictionary* tooLong = [rename() mutableCopy];
    tooLong[@"name"] = [@"x" stringByPaddingToLength:4097 withString:@"x" startingAtIndex:0];
    invalid([&] {
        decodeEditorRequest(json(envelope(@[ tooLong ])));
    });
    unichar chars[]{'n', 0, 'l'};
    tooLong[@"name"] = [NSString stringWithCharacters:chars length:3];
    invalid([&] {
        decodeEditorRequest(json(envelope(@[ tooLong ])));
    });
    // Syntax can be valid while topology is not. The portable reducer remains
    // authoritative and refuses the whole candidate before it reaches storage.
    const auto topology = decodeEditorRequest(json(envelope(@[
        @{@"type" : @"createDevice", @"id" : @"mix", @"name" : @"Mix"},
        @{@"type" : @"resizeDevice",
          @"device" : @"mix",
          @"channels" : @3}
    ])));
    invalid([&] {
        edit::applyCommands({}, topology.commands);
    });
}

void exactResponses() {
    EditorSnapshot snapshot{{"epoch", UINT64_MAX},
                            std::nullopt,
                            {RestorationPhase::WaitingForDriver, UINT64_MAX,
                             UINT64_C(9007199254740993), 42, -50, "Driver disconnected", true}};
    id response = parsed(encodeEditorSnapshot(snapshot));
    assert([response[@"revision"][@"generation"] isEqual:@"18446744073709551615"]);
    assert([response[@"restoration"][@"activeGeneration"] isEqual:@"9007199254740993"]);
    assert([response[@"restoration"][@"phase"] isEqual:@"waiting-for-driver"]);
    assert([response[@"document"] isEqual:[NSNull null]]);
    assert([response[@"restoration"][@"error"] intValue] == -50);
    assert([response[@"restoration"][@"recoveredBackup"] boolValue]);
    assert(![response[@"history"][@"canUndo"] boolValue]);
    snapshot.history = {true, false};
    const std::array<edit::Command, 1> commands{edit::CreateDevice{"mix", "Mix"}};
    snapshot.document = edit::applyCommands({}, commands);
    response = parsed(encodeEditorSnapshot(snapshot));
    assert([response[@"history"][@"canUndo"] boolValue]);
    const auto document = decodeRouteDocumentJSON(json(response[@"document"]));
    assert(document.devices[0].id == "mix");
    response = parsed(encodeEditorCommit({{"epoch", UINT64_MAX}, false}));
    assert([response[@"revision"][@"generation"] isEqual:@"18446744073709551615"]);
    assert(![response[@"changed"] boolValue]);
    assert(!response[@"active"] && !response[@"installed"]);
}

void strictPairRemoval() {
    for (id value in @[ @YES, @-1, @1.5, @4294967296ULL, @"2", NSNull.null ])
        invalid([&] {
            (void)decodeEditorRequest(json(envelope(@[
                @{@"type" : @"removeOutputPair", @"device" : @"mix", @"firstChannel" : value}
            ])));
        });
    for (NSDictionary* command in @[
             @{@"type" : @"removeOutputPair", @"device" : @"mix"},
             @{
                 @"type" : @"removeOutputPair",
                 @"device" : @"mix",
                 @"firstChannel" : @0,
                 @"channels" : @2
             },
         ])
        invalid([&] {
            (void)decodeEditorRequest(json(envelope(@[ command ])));
        });
}

void strictHistoryRequests() {
    for (NSString* action in @[ @"undo", @"redo" ]) {
        const auto request = decodeEditorRequest(json(@{
            @"version" : @1,
            @"expected" : @{@"epoch" : @"epoch", @"generation" : @"8"},
            @"history" : action
        }));
        assert(request.history && request.commands.empty());
        assert(*request.history == ([action isEqual:@"undo"] ? EditorHistoryDirection::Undo
                                                             : EditorHistoryDirection::Redo));
    }
    for (id action in @[ @"", @"Undo", @"delete", @1, @NO, NSNull.null ])
        invalid([&] {
            decodeEditorRequest(json(@{
                @"version" : @1,
                @"expected" : @{@"epoch" : @"epoch", @"generation" : @"8"},
                @"history" : action
            }));
        });
    auto mixed = envelope(@[ rename() ]);
    mixed[@"history"] = @"undo";
    invalid([&] {
        decodeEditorRequest(json(mixed));
    });
}
} // namespace

int main() {
    @autoreleasepool {
        everyCommandAndRealDocument();
        strictEnvelopeAndCounters();
        invalidCommandsAndFields();
        exactResponses();
        strictHistoryRequests();
        strictPairRemoval();
        std::puts("All editor commands, strict wire bounds and lossless revisions passed");
    }
}
