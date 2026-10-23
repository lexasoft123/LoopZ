#import <Foundation/Foundation.h>
#include <loopz/macos/editor_service.hpp>
#include <loopz/macos/engine_runtime.hpp>
#include <loopz/macos/capture.hpp>
#include "../platform/macos/test_code_identity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <fstream>
#include <future>
#include <thread>
#include <unistd.h>
#include <dispatch/dispatch.h>

namespace {
using namespace loopz;
using namespace loopz::macos;
using namespace std::chrono_literals;

uint64_t now() {
    return hostNowNs() / 1000000;
}
template <typename Predicate> void until(Predicate predicate, uint64_t limit = 10000) {
    const auto deadline = now() + limit;
    while (now() < deadline) {
        if (predicate())
            return;
        std::this_thread::sleep_for(1ms);
    }
    assert(false && "Editor service condition timed out");
}
template <typename Operation>
auto perform(EditorService& service, EngineRuntime* runtime, Operation operation) {
    auto future = std::async(std::launch::async, std::move(operation));
    until([&] {
        if (runtime)
            runtime->poll();
        service.poll();
        return future.wait_for(0ms) == std::future_status::ready;
    });
    return future.get();
}
std::string json(id value) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    assert(data);
    return {static_cast<const char*>(data.bytes), data.length};
}
id parsed(const std::string& value) {
    id result = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:value.data()
                                                                       length:value.size()]
                                                options:0
                                                  error:nil];
    assert(result);
    return result;
}
std::string request(id snapshot, NSArray* commands) {
    return json(@{@"version" : @1, @"expected" : snapshot[@"revision"], @"commands" : commands});
}
EditorServiceEndpoint endpoint(EditorService& service, const std::string& requirement) {
    return {kEditorServiceName, requirement, static_cast<uint32_t>(geteuid()), service.endpoint()};
}
EditorServicePolicy policy(const std::string& requirement) {
    return {requirement, static_cast<uint32_t>(geteuid())};
}

class Link final : public DriverConfigurationLink {
  public:
    uint64_t identity{1}, submissions{0};
    std::vector<DeviceTransport> mappings;
    DriverConfigurationResult outcome{0, DriverConfigurationPhase::Committed, 0};
    void poll(uint64_t) override {
        for (auto& mapping : mappings) {
            mapping.region->header().driverOnline.store(identity != 0);
            mapping.region->header().driverHeartbeatNs.store(hostNowNs());
        }
    }
    uint64_t session() const noexcept override {
        return identity;
    }
    int32_t submit(std::span<const DeviceTransport> devices, uint64_t revision) override {
        ++submissions;
        mappings.assign(devices.begin(), devices.end());
        outcome = {revision, DriverConfigurationPhase::Committed, 0};
        return 0;
    }
    DriverConfigurationResult status(uint64_t) override {
        return outcome;
    }
    int32_t cancel(uint64_t) override {
        return 0;
    }
    void invalidate() noexcept override {
        identity = 0;
    }
};
class Input final : public SourceEndpoint {
  public:
    explicit Input(uint32_t channels) : count_(channels) {}
    uint32_t channels() const noexcept override {
        return count_;
    }
    bool open(SourceOpenPolicy, std::string&) override {
        return true;
    }
    void close() noexcept override {}
    bool healthy(std::string&) override {
        return true;
    }
    AudioReadResult read(uint64_t, double, RoutingOutput output, uint32_t frames,
                         AudioReadCursor&) const noexcept override {
        for (auto* plane : output.channels)
            std::fill_n(plane, frames, 0.4f);
        return {0, true};
    }

  private:
    const uint32_t count_;
};
RouteDocument initial() {
    const std::array<edit::Command, 3> commands{
        edit::CreateDevice{"mix", "Mix", 2, 48000, false},
        edit::AddSource{"mix", "input", SourceKind::InputDevice, "fixture-input", 1},
        edit::CreateDevice{"other", "Unrelated"}};
    return edit::applyCommands({}, commands);
}
bool peak(EngineRuntime& runtime, float expected) {
    auto scene = runtime.observe();
    AudioMeterSnapshot meter;
    return scene && scene->deviceMeters(0, meter) && meter.channels == 2 && meter.frames &&
           std::fabs(meter.peak[0] - expected) < 0.0001f;
}

void hardwareBufferRoundTrip(const std::string& requirement) {
    const auto owner = std::this_thread::get_id();
    unsigned writes = 0;
    EditorOperations operations{[] {
                                    return "{}";
                                },
                                [](std::string_view) {
                                    return "{}";
                                },
                                [] {}};
    operations.hardwareBuffer = [&](std::string_view request) {
        assert(std::this_thread::get_id() == owner);
        assert(request == "{\"version\":1}");
        ++writes;
        return "{\"ok\":true}";
    };
    auto service = EditorService::anonymous(policy(requirement), operations);
    EditorServiceClient client(endpoint(*service, requirement));
    perform(*service, nullptr, [&] {
        client.connect();
    });
    const auto reply = perform(*service, nullptr, [&] {
        return client.hardwareBuffer("{\"version\":1}");
    });
    assert(reply == "{\"ok\":true}" && writes == 1);
    client.disconnect();
    service->stop();
    operations.hardwareBuffer = {};
    service = EditorService::anonymous(policy(requirement), operations);
    EditorServiceClient older(endpoint(*service, requirement));
    perform(*service, nullptr, [&] {
        older.connect();
    });
    perform(*service, nullptr, [&] {
        bool unavailable = false;
        try {
            older.hardwareBuffer("{\"version\":1}");
        } catch (const EditorServiceError& error) {
            unavailable = error.code == EditorFailure::Unavailable;
        }
        assert(unavailable && writes == 1);
    });
    older.disconnect();
    service->stop();
}

void previewConnectionLifetime(const std::string& requirement) {
    std::vector<bool> transitions;
    auto service =
        EditorService::anonymous(policy(requirement), {[] {
                                                           return "{}";
                                                       },
                                                       [](std::string_view) {
                                                           return "{}";
                                                       },
                                                       [] {},
                                                       {},
                                                       [&](bool enabled) {
                                                           transitions.push_back(enabled);
                                                       }});
    EditorServiceClient observer(endpoint(*service, requirement));
    observer.connect();
    perform(*service, nullptr, [&] {
        observer.snapshot();
    });
    assert(transitions.empty());
    EditorServiceClient first(endpoint(*service, requirement));
    EditorServiceClient second(endpoint(*service, requirement));
    first.connect();
    second.connect();
    perform(*service, nullptr, [&] {
        first.preview();
    });
    perform(*service, nullptr, [&] {
        second.preview();
    });
    assert(transitions == std::vector<bool>{true});
    first.disconnect();
    until([&] {
        service->poll();
        return service->status().peers == 2;
    });
    assert(transitions == std::vector<bool>{true});
    second.disconnect();
    until([&] {
        service->poll();
        return transitions.size() == 2;
    });
    assert((transitions == std::vector<bool>{true, false}));
    perform(*service, nullptr, [&] {
        observer.snapshot();
    });
    assert(transitions.size() == 2);
    first.connect();
    perform(*service, nullptr, [&] {
        first.preview();
    });
    service->stop();
    assert((transitions == std::vector<bool>{true, false, true, false}));
}
void runtimeRoundTrip(const std::string& requirement, const std::filesystem::path& root) {
    const auto path = root / "routing.json";
    saveRouteDocumentFile(path, initial());
    Link link;
    EngineRuntimeOptions options;
    options.documentPath = path;
    EngineRuntime runtime(link, options, [](SourceKind, const std::string&, uint32_t channels) {
        return std::make_shared<Input>(channels);
    });
    runtime.start();
    auto service = EditorService::anonymous(policy(requirement), runtimeEditorOperations(runtime));
    EditorServiceClient client(endpoint(*service, requirement));
    client.connect();
    until([&] {
        runtime.poll();
        return runtime.status().restored && peak(runtime, 0.4f);
    });
    const auto oldEpoch = runtime.editorSnapshot().revision.epoch;
    const auto unrelated = runtime.devices()[1].region;
    auto& transportHeader = runtime.devices()[0].region->view().header();
    constexpr uint64_t largeCount = (uint64_t{1} << 54) + 7;
    transportHeader.captureReadFrames.store(largeCount);
    transportHeader.captureMissingFrames.store(29);
    id snapshot = parsed(perform(*service, &runtime, [&] {
        return client.snapshot();
    }));
    assert([snapshot[@"runtime"][@"rendering"] boolValue]);
    assert([snapshot[@"runtime"][@"metersFresh"] boolValue]);
    assert([snapshot[@"runtime"][@"devices"] count] == 2);
    NSDictionary* transport = snapshot[@"runtime"][@"devices"][0][@"transport"];
    assert([transport[@"available"] boolValue]);
    assert([transport[@"captureReadFrames"] isEqual:@"18014398509481991"]);
    assert([transport[@"captureMissingFrames"] isEqual:@"29"]);
    NSString* transportGeneration =
        [NSString stringWithFormat:@"%llu", (unsigned long long)transportHeader.generation];
    assert([transport[@"generation"] isEqual:transportGeneration]);
    transportHeader.captureReadFrames.store(0);
    transportHeader.captureMissingFrames.store(0);
    const auto edit = request(snapshot, @[
        @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @"Stream Mix"},
        @{@"type" : @"setDeviceControls",
          @"device" : @"mix",
          @"controls" : @{@"gain" : @0.5}},
        @{
            @"type" : @"setSourceControls",
            @"device" : @"mix",
            @"source" : @"input",
            @"controls" : @{@"gain" : @0.5}
        },
        @{
            @"type" : @"createDevice",
            @"id" : @"bus",
            @"name" : @"Wide Bus",
            @"channels" : @8,
            @"passThrough" : @NO
        },
        @{
            @"type" : @"addSource",
            @"device" : @"bus",
            @"id" : @"mix",
            @"kind" : @"virtual-device",
            @"selector" : @"mix"
        },
        @{
            @"type" : @"addMonitor",
            @"device" : @"mix",
            @"id" : @"saved",
            @"selector" : @"offline-output"
        },
        @{
            @"type" : @"setMonitorControls",
            @"device" : @"mix",
            @"monitor" : @"saved",
            @"controls" : @{@"enabled" : @NO}
        }
    ]);
    id commit = parsed(perform(*service, &runtime, [&] {
        return client.edit(edit);
    }));
    assert([commit[@"changed"] boolValue]);
    until([&] {
        runtime.poll();
        return runtime.status().restored && peak(runtime, 0.1f);
    });
    assert(runtime.devices().size() == 3 && runtime.devices()[1].region == unrelated);
    const auto saved = loadRouteDocumentFile(path).document;
    assert(saved.devices[0].name == "Stream Mix" && saved.devices[0].routing.gain == 0.5f);
    assert(saved.devices[0].monitors.size() == 1 && !saved.devices[0].monitors[0].enabled);
    assert(saved.devices[2].routing.outputChannels == 8 &&
           saved.devices[2].bindings[0].selector == "mix");
    snapshot = parsed(perform(*service, &runtime, [&] {
        return client.snapshot();
    }));
    id meter = snapshot[@"runtime"][@"devices"][0][@"meter"];
    assert([meter[@"valid"] boolValue] && [meter[@"peak"] count] == 2);
    assert(std::fabs([meter[@"peak"][0] doubleValue] - 0.1) < 0.0001);
    assert(
        [snapshot[@"restoration"][@"activeGeneration"] isEqual:commit[@"revision"][@"generation"]]);

    bool conflict = false;
    try {
        perform(*service, &runtime, [&] {
            return client.edit(edit);
        });
    } catch (const EditorServiceError& error) {
        conflict = error.code == EditorFailure::Conflict;
    }
    assert(conflict && client.connected());
    assert([snapshot[@"history"][@"canUndo"] boolValue]);
    const auto historyRequest = [&](NSString* direction) {
        return json(
            @{@"version" : @1,
              @"expected" : snapshot[@"revision"],
              @"history" : direction});
    };
    perform(*service, &runtime, [&] {
        return client.edit(historyRequest(@"undo"));
    });
    until([&] {
        runtime.poll();
        return runtime.status().restored && peak(runtime, 0.4f);
    });
    assert(runtime.devices().size() == 2 && runtime.devices()[1].region == unrelated);
    snapshot = parsed(perform(*service, &runtime, [&] {
        return client.snapshot();
    }));
    assert([snapshot[@"history"][@"canRedo"] boolValue]);
    perform(*service, &runtime, [&] {
        return client.edit(historyRequest(@"redo"));
    });
    until([&] {
        runtime.poll();
        return runtime.status().restored && peak(runtime, 0.1f);
    });
    assert(runtime.devices().size() == 3 && runtime.devices()[1].region == unrelated);
    snapshot = parsed(perform(*service, &runtime, [&] {
        return client.snapshot();
    }));
    assert([snapshot[@"history"][@"canUndo"] boolValue]);
    const auto generation = runtime.editorSnapshot().revision.generation;
    auto external = saved;
    external.devices[0].name = "Externally changed";
    saveRouteDocumentFile(path, external);
    const auto stale = request(
        snapshot, @[ @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @"Stale"} ]);
    conflict = false;
    try {
        perform(*service, &runtime, [&] {
            return client.edit(stale);
        });
    } catch (const EditorServiceError& error) {
        conflict = error.code == EditorFailure::Conflict;
    }
    assert(conflict && runtime.editorSnapshot().revision.generation == generation + 1);
    assert(loadRouteDocumentFile(path).document.devices[0].name == "Externally changed");

    link.identity = 0;
    for (const auto& mapping : link.mappings)
        mapping.region->retire();
    runtime.poll();
    snapshot = parsed(perform(*service, &runtime, [&] {
        return client.snapshot();
    }));
    const auto offlineEdit = request(snapshot, @[
        @{@"type" : @"setDeviceControls",
          @"device" : @"mix",
          @"controls" : @{@"muted" : @YES}}
    ]);
    perform(*service, &runtime, [&] {
        return client.edit(offlineEdit);
    });
    assert(loadRouteDocumentFile(path).document.devices[0].routing.muted);
    assert(runtime.status().configuration.activeGeneration == 0);
    link.identity = 2;
    runtime.retryNow();
    until([&] {
        runtime.poll();
        return runtime.status().restored && peak(runtime, 0);
    });
    assert(runtime.observe()->document().devices.size() == 3);
    assert(runtime.observe()->document().devices[0].routing.muted);

    client.disconnect();
    service->stop();
    const auto blocks = runtime.status().render.blocks;
    until([&] {
        runtime.poll();
        return runtime.status().render.blocks > blocks;
    });
    assert(runtime.status().restored);
    runtime.stop();
    const auto stopped = runtimeEditorOperations(runtime).snapshot();
    assert(![parsed(stopped)[@"runtime"][@"metersFresh"] boolValue]);
    Link nextLink;
    EngineRuntime reconstructed(nextLink, options);
    assert(reconstructed.editorSnapshot().revision.epoch != oldEpoch);
    options.installationValidation = true;
    EngineRuntime validation(nextLink, options);
    validation.reloadDesired();
    const auto beforeValidationEdit = validation.editorSnapshot();
    const std::array<edit::Command, 1> forbidden{edit::RenameDevice{"mix", "Installer edit"}};
    bool refused = false;
    try {
        validation.applyEditor(beforeValidationEdit.revision, forbidden);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    assert(refused && validation.editorSnapshot().revision == beforeValidationEdit.revision);
    assert(loadRouteDocumentFile(path).document.devices[0].name == "Externally changed");
}
void idleSnapshotAndImmediateEnable(const std::string& requirement,
                                    const std::filesystem::path& root) {
    auto document = initial();
    document.devices.resize(1);
    document.devices.front().routing.enabled = false;
    const auto path = root / "idle-routing.json";
    saveRouteDocumentFile(path, document);
    Link link;
    EngineRuntimeOptions options;
    options.documentPath = path;
    EngineRuntime runtime(link, options, [](SourceKind, const std::string&, uint32_t channels) {
        return std::make_shared<Input>(channels);
    });
    runtime.start();
    until([&] {
        runtime.poll();
        return runtime.idleStatusToken().has_value();
    });
    auto operations = runtimeEditorOperations(runtime);
    auto service = EditorService::anonymous(policy(requirement), operations);
    EditorServiceClient client(endpoint(*service, requirement));
    client.connect();
    id before = parsed(perform(*service, &runtime, [&] {
        return client.snapshot();
    }));
    assert(![before[@"runtime"][@"rendering"] boolValue]);
    const auto rename = request(
        before, @[ @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @"Idle renamed"} ]);
    perform(*service, &runtime, [&] {
        return client.edit(rename);
    });
    // Snapshot before another maintenance tick must expose the edit immediately.
    id renamed = parsed(operations.snapshot());
    assert([renamed[@"document"][@"devices"][0][@"name"] isEqual:@"Idle renamed"]);
    until([&] {
        runtime.poll();
        return runtime.idleStatusToken().has_value();
    });
    const auto cached = operations.snapshot();
    assert(cached == operations.snapshot());
    const auto enable = request(parsed(cached), @[
        @{@"type" : @"setDeviceControls",
          @"device" : @"mix",
          @"controls" : @{@"enabled" : @YES}}
    ]);
    const auto begun = now();
    perform(*service, &runtime, [&] {
        return client.edit(enable);
    });
    until(
        [&] {
            runtime.poll();
            return runtime.status().restored && runtime.status().render.running &&
                   peak(runtime, 0.4f);
        },
        500);
    assert(now() - begun < 500);
    assert(!runtime.idleStatusToken());
    assert([parsed(operations.snapshot())[@"runtime"][@"rendering"] boolValue]);
    client.disconnect();
    service->stop();
}

struct Reply {
    std::atomic<bool> done{false};
    xpc_object_t value{nullptr};
};
struct RawPeer {
    xpc_connection_t peer;
    dispatch_queue_t queue;
    explicit RawPeer(const EditorServiceEndpoint& endpoint, bool authenticate = true)
        : peer(xpc_connection_create_from_endpoint(endpoint.endpoint)),
          queue(dispatch_queue_create("loopz.editor.raw-test", DISPATCH_QUEUE_SERIAL)) {
        xpc_connection_set_target_queue(peer, queue);
        assert(xpc_connection_set_peer_code_signing_requirement(
                   peer, endpoint.serverRequirement.c_str()) == 0);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t){
                                         });
        xpc_connection_activate(peer);
        if (authenticate) {
            auto reply = send("hello");
            until([&] {
                return reply->done.load();
            });
            assert(xpc_get_type(reply->value) == XPC_TYPE_DICTIONARY &&
                   xpc_dictionary_get_bool(reply->value, "ok"));
        }
    }
    ~RawPeer() {
        xpc_connection_cancel(peer);
        dispatch_sync(queue, ^{
                      });
    }
    std::shared_ptr<Reply> send(const char* operation, xpc_object_t request = nullptr) {
        if (!request) {
            request = xpc_dictionary_create(nullptr, nullptr, 0);
            xpc_dictionary_set_uint64(request, "protocol", kEditorProtocolVersion);
            xpc_dictionary_set_string(request, "op", operation);
        }
        auto reply = std::make_shared<Reply>();
        xpc_connection_send_message_with_reply(peer, request, queue, ^(xpc_object_t value) {
            reply->value = value;
            reply->done.store(true);
        });
        return reply;
    }
};

void authenticationAndMalformedRequests(const std::string& requirement) {
    std::atomic<unsigned> calls{0};
    EditorOperations operations{[&] {
                                    ++calls;
                                    return "{}";
                                },
                                [&](std::string_view) {
                                    ++calls;
                                    return "{}";
                                },
                                [&] {
                                    ++calls;
                                }};
    auto service = EditorService::anonymous(policy(requirement), operations);
    auto rejected = [&](EditorServiceEndpoint target) {
        EditorServiceClient client(std::move(target));
        bool failed = false;
        try {
            client.connect();
        } catch (const EditorServiceError&) {
            failed = true;
        }
        assert(failed && !client.connected() && !calls && !service->status().outstanding);
    };
    auto wrong = endpoint(*service, requirement);
    wrong.serverRequirement = "identifier \"not.the.engine\"";
    rejected(wrong);
    wrong = endpoint(*service, requirement);
    ++wrong.serverUID;
    rejected(wrong);
    {
        auto bad = policy(requirement);
        bad.clientRequirement = "identifier \"not.the.editor\"";
        auto other = EditorService::anonymous(bad, operations);
        rejected(endpoint(*other, requirement));
    }
    {
        auto bad = policy(requirement);
        ++bad.clientUID;
        auto other = EditorService::anonymous(bad, operations);
        rejected(endpoint(*other, requirement));
    }
    assert(productionEditorPolicy().clientRequirement.find("com.lexasoft.loopz.editor") !=
           std::string::npos);
    assert(productionEditorEndpoint().serverRequirement.find("com.lexasoft.loopz.engine") !=
           std::string::npos);
    assert(productionEditorEndpoint().serverUID == geteuid());
    RawPeer peer(endpoint(*service, requirement));
    for (unsigned index = 0; index < 5; ++index) {
        auto request = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_uint64(request, "protocol", kEditorProtocolVersion);
        xpc_dictionary_set_string(request, "op", index >= 3 ? "edit" : "snapshot");
        if (index == 0)
            xpc_dictionary_set_int64(request, "protocol", 1);
        if (index == 1)
            xpc_dictionary_set_string(request, "extra", "forbidden");
        if (index == 2)
            xpc_dictionary_set_string(request, "op", "unknown");
        if (index == 3)
            xpc_dictionary_set_string(request, "payload", "not data");
        if (index == 4) {
            const std::string oversized(kMaximumEditorRequestBytes + 1, 'x');
            xpc_dictionary_set_data(request, "payload", oversized.data(), oversized.size());
        }
        const auto reply = peer.send("unused", request);
        until([&] {
            return reply->done.load();
        });
        assert(xpc_get_type(reply->value) == XPC_TYPE_DICTIONARY);
        assert(!xpc_dictionary_get_bool(reply->value, "ok"));
        assert(std::string(xpc_dictionary_get_string(reply->value, "code")) == "invalid");
        assert(!calls && !service->status().outstanding);
    }
}

void boundedQueuesAndCancellation(const std::string& requirement) {
    const auto owner = std::this_thread::get_id();
    unsigned calls = 0;
    auto operation = [&] {
        assert(std::this_thread::get_id() == owner);
        ++calls;
        return "{}";
    };
    auto service = EditorService::anonymous(policy(requirement), {operation,
                                                                  [&](std::string_view) {
                                                                      return operation();
                                                                  },
                                                                  [&] {
                                                                      (void)operation();
                                                                  }});
    const auto target = endpoint(*service, requirement);
    RawPeer idle(target, false);
    auto unauthenticated = idle.send("snapshot");
    until([&] {
        return unauthenticated->done.load();
    });
    assert(std::string(xpc_dictionary_get_string(unauthenticated->value, "code")) == "invalid");
    std::vector<std::unique_ptr<RawPeer>> peers;
    peers.push_back(std::make_unique<RawPeer>(target));
    for (unsigned i = 0; i < 4; ++i)
        peers[0]->send("snapshot");
    until([&] {
        return service->status().outstanding == 4;
    });
    auto busy = peers[0]->send("snapshot");
    until([&] {
        return busy->done.load();
    });
    assert(std::string(xpc_dictionary_get_string(busy->value, "code")) == "busy");
    for (unsigned i = 1; i < 4; ++i) {
        peers.push_back(std::make_unique<RawPeer>(target));
        for (unsigned j = 0; j < 4; ++j)
            peers.back()->send("snapshot");
    }
    until([&] {
        return service->status().outstanding == kMaximumEditorOutstanding;
    });
    peers.push_back(std::make_unique<RawPeer>(target));
    busy = peers.back()->send("snapshot");
    until([&] {
        return busy->done.load();
    });
    assert(std::string(xpc_dictionary_get_string(busy->value, "code")) == "busy");
    assert(calls == 0 && service->status().inFlight == 0);
    peers.erase(peers.begin());
    until([&] {
        return service->status().outstanding == 12;
    });
    for (unsigned i = 0; i < 4; ++i)
        peers.back()->send("snapshot");
    until([&] {
        return service->status().outstanding == 16;
    });
    std::this_thread::sleep_for(2100ms);
    for (unsigned i = 0; i < 16; ++i)
        service->poll();
    assert(!service->status().outstanding && service->status().expired == 16 && calls == 0);
    std::vector<std::unique_ptr<EditorServiceClient>> clients;
    until([&] {
        return service->status().peers == 4;
    });
    for (unsigned i = 0; i < 4; ++i) {
        clients.push_back(std::make_unique<EditorServiceClient>(target));
        clients.back()->connect();
    }
    assert(service->status().peers == kMaximumEditorPeers);
    EditorServiceClient excess(target);
    bool rejected = false;
    try {
        excess.connect();
    } catch (const EditorServiceError&) {
        rejected = true;
    }
    assert(rejected && service->status().peers <= kMaximumEditorPeers);
    for (unsigned i = 0; i < 4; ++i)
        peers.back()->send("snapshot");
    until([&] {
        return service->status().outstanding == 4;
    });
    service->stop();
    assert(!service->status().listening && !service->status().outstanding &&
           !service->status().peers);
}

void lateReplyDoesNotReplay(const std::string& requirement) {
    unsigned edits = 0;
    EditorService* observed = nullptr;
    auto service = EditorService::anonymous(
        policy(requirement), {[&] {
                                  return json(@{@"edits" : @(edits)});
                              },
                              [&](std::string_view) {
                                  assert(observed && observed->status().outstanding == 1 &&
                                         observed->status().inFlight == 1);
                                  ++edits;
                                  std::this_thread::sleep_for(3500ms);
                                  return "{}";
                              },
                              [] {}});
    const auto target = endpoint(*service, requirement);
    observed = service.get();
    EditorConnection client(target);
    auto refresh = [&] {
        return perform(*service, nullptr, [&] {
            while (!client.ready()) {
                client.poll();
                std::this_thread::sleep_for(1ms);
            }
            return client.snapshot();
        });
    };
    auto refused = [&] {
        bool unavailable = false;
        try {
            client.edit("{}");
        } catch (const EditorServiceError& error) {
            unavailable = error.code == EditorFailure::Unavailable;
        }
        assert(unavailable);
    };
    refused();
    assert(!edits);
    refresh();
    bool uncertain = false;
    try {
        perform(*service, nullptr, [&] {
            return client.edit("{}");
        });
    } catch (const UncertainEditorOutcome&) {
        uncertain = true;
    }
    assert(uncertain && edits == 1 && !client.connected() && !client.ready() &&
           !service->status().outstanding);
    refused();
    id snapshot = parsed(refresh());
    assert([snapshot[@"edits"] intValue] == 1 && edits == 1);
}

void stopDuringConnection(const std::string& requirement) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::atomic<unsigned> calls{0};
        auto service = EditorService::anonymous(policy(requirement), {[&] {
                                                                          ++calls;
                                                                          return "{}";
                                                                      },
                                                                      [&](std::string_view) {
                                                                          ++calls;
                                                                          return "{}";
                                                                      },
                                                                      [] {}});
        const auto target = endpoint(*service, requirement);
        auto connecting = std::async(std::launch::async, [target] {
            EditorServiceClient client(target);
            try {
                client.connect();
            } catch (const EditorServiceError&) {
            }
        });
        if (attempt % 2)
            std::this_thread::sleep_for(1ms);
        service->stop();
        connecting.get();
        assert(!calls && !service->status().peers && !service->status().outstanding);
    }
}

volatile sig_atomic_t stopping = 0;
void stop(int) {
    stopping = 1;
}

void serverFixture(const std::string& name, const std::filesystem::path& path,
                   const std::string& requirement) {
    if (!std::filesystem::exists(path))
        saveRouteDocumentFile(path, initial());
    Link link;
    EngineRuntimeOptions options;
    options.documentPath = path;
    EngineRuntime runtime(link, options, [](SourceKind, const std::string&, uint32_t channels) {
        return std::make_shared<Input>(channels);
    });
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    runtime.start();
    runtime.poll();
    auto service =
        EditorService::machService(policy(requirement), runtimeEditorOperations(runtime), name);
    const auto deadline = now() + 90000;
    while (!stopping && now() < deadline) {
        @autoreleasepool {
            runtime.poll();
            service->poll();
            std::this_thread::sleep_for(2ms);
        }
    }
    service->stop();
    runtime.stop();
}

void clientFixture(const std::string& name, const std::string& phase,
                   const std::filesystem::path& previousPath, const std::string& requirement) {
    EditorConnection client({name, requirement, static_cast<uint32_t>(geteuid()), nullptr});
    id snapshot = nil;
    until(
        [&] {
            client.poll();
            if (!client.ready())
                return false;
            snapshot = parsed(client.snapshot());
            return [snapshot[@"runtime"][@"restored"] boolValue] != NO;
        },
        25000);
    NSString* epoch = [snapshot[@"revision"][@"epoch"] copy];
    if (phase == "first") {
        assert([snapshot[@"document"][@"devices"] count] == 2);
        client.edit(request(snapshot, @[
            @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @"Persistent edit"},
            @{@"type" : @"setDeviceControls",
              @"device" : @"mix",
              @"controls" : @{@"gain" : @0.25}},
            @{
                @"type" : @"setSourceControls",
                @"device" : @"mix",
                @"source" : @"input",
                @"controls" : @{@"gain" : @0.5}
            },
            @{
                @"type" : @"createDevice",
                @"id" : @"bus",
                @"name" : @"Bus",
                @"channels" : @8,
                @"passThrough" : @NO
            },
            @{
                @"type" : @"addSource",
                @"device" : @"bus",
                @"id" : @"mix",
                @"kind" : @"virtual-device",
                @"selector" : @"mix"
            }
        ]));
    } else {
        assert(phase == "replacement");
        std::ifstream previous(previousPath);
        assert(previous.good());
        const std::string data{std::istreambuf_iterator<char>(previous), {}};
        id old = parsed(data);
        assert(![epoch isEqual:old[@"revision"][@"epoch"]]);
        assert([snapshot[@"document"][@"devices"] count] == 3);
        assert([snapshot[@"document"][@"devices"][0][@"name"] isEqual:@"Persistent edit"]);
        bool conflict = false;
        try {
            client.edit(request(
                old, @[ @{@"type" : @"renameDevice", @"device" : @"mix", @"name" : @"Replay"} ]));
        } catch (const EditorServiceError& error) {
            conflict = error.code == EditorFailure::Conflict;
        }
        assert(conflict);
        until([&] {
            client.poll();
            return client.ready();
        });
        snapshot = parsed(client.snapshot());
        client.edit(request(snapshot, @[
            @{@"type" : @"resizeDevice",
              @"device" : @"bus",
              @"channels" : @64},
            @{@"type" : @"setDeviceControls",
              @"device" : @"mix",
              @"controls" : @{@"gain" : @0.5}}
        ]));
    }
    const double expected = phase == "first" ? 0.05 : 0.1;
    until([&] {
        client.poll();
        if (!client.ready())
            return false;
        snapshot = parsed(client.snapshot());
        id meter = snapshot[@"runtime"][@"devices"][0][@"meter"];
        return [snapshot[@"runtime"][@"restored"] boolValue] && [meter[@"valid"] boolValue] &&
               std::fabs([meter[@"peak"][0] doubleValue] - expected) < 0.0001;
    });
    const auto report = json(@{
        @"passed" : @YES,
        @"pid" : @(getpid()),
        @"revision" : snapshot[@"revision"],
        @"devices" : @([snapshot[@"document"][@"devices"] count]),
        @"busChannels" : snapshot[@"document"][@"devices"][2][@"routing"][@"outputChannels"],
        @"peak" : @(expected)
    });
    std::puts(report.c_str());
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        const auto requirement = selfCodeRequirement();
        if (argc == 4 && std::string_view(argv[1]) == "--server-fixture") {
            serverFixture(argv[2], argv[3], requirement);
            return 0;
        }
        if (argc == 6 && std::string_view(argv[1]) == "--client-fixture") {
            clientFixture(argv[2], argv[3], argv[4], requirement);
            assert(std::string_view(argv[5]) == "--capture-free");
            return 0;
        }
        assert(argc == 1);
        previewConnectionLifetime(requirement);
        hardwareBufferRoundTrip(requirement);
        const auto root = std::filesystem::path(NSTemporaryDirectory().UTF8String) /
                          [NSUUID UUID].UUIDString.UTF8String;
        std::filesystem::create_directories(root);
        runtimeRoundTrip(requirement, root);
        idleSnapshotAndImmediateEnable(requirement, root);
        authenticationAndMalformedRequests(requirement);
        boundedQueuesAndCancellation(requirement);
        lateReplyDoesNotReplay(requirement);
        stopDuringConnection(requirement);
        std::filesystem::remove_all(root);
        std::puts("Authenticated editor control, real runtime meters, bounds and uncertain replies "
                  "passed");
    }
}
