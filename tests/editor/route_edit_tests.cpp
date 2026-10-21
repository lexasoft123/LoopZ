#include <loopz/route_edits.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace loopz;
using namespace loopz::edit;

RouteDocument edited(const RouteDocument& document, std::initializer_list<Command> commands) {
    return applyCommands(document, {commands.begin(), commands.size()});
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

RouteDocument initial() {
    return edited({}, {CreateDevice{"mix", "Stream Mix", 4},
                       AddSource{"mix", "mic", SourceKind::InputDevice, "mic-uid", 1},
                       AddSource{"mix", "app", SourceKind::Application, "com.example.music"},
                       AddMonitor{"mix", "headphones", "headphone-uid", 2},
                       CreateDevice{"call", "Call Mix", 2, 44100, false},
                       AddSource{"call", "stream", SourceKind::VirtualDevice, "mix"}});
}

void arbitraryOutputPairs() {
    auto original = edited(
        {}, {
                CreateDevice{"mix", "Eight channels", 8},
                AddSource{"mix", "physical", SourceKind::InputDevice, "hardware", 8, false, false},
                SetSourceWiring{"mix",
                                "physical",
                                {{0, 0, 0.25f},
                                 {2, 2, 0.5f},
                                 {6, 6, 0.75f},
                                 {7, 1, 0.125f},
                                 {5, 4, 0.25f},
                                 {2, 7, 0.5f}}},
                AddMonitor{"mix", "phones", "physical-output", 8, false},
                SetMonitorWiring{"mix", "phones", {{0, 0, 0.25f}, {2, 5, 0.5f}, {6, 7, 0.75f}}},
                CreateDevice{"dependent", "Dependent", 8, 48000, false},
                AddSource{"dependent", "virtual", SourceKind::VirtualDevice, "mix", 2, true, false},
                SetSourceWiring{"dependent",
                                "virtual",
                                {{0, 7, 0.5f}, {2, 0, 0.25f}, {6, 1, 0.75f}, {7, 2, 0.125f}}},
                SetSourceControls{
                    "dependent", "virtual", {.gain = 0.5f, .enabled = false, .muted = true}, {}},
                CreateDevice{"unrelated", "Unrelated", 4},
            });
    const auto changed = edited(original, {RemoveOutputPair{"mix", 2}});
    const auto& mix = changed.devices[0];
    const auto& dependent = changed.devices[1];
    assert(original.devices[0].routing.outputChannels == 8 && mix.routing.outputChannels == 6);
    assert(mix.routing.sources[0].channels == 6 && mix.routing.sources[0].connections.size() == 6);
    for (uint32_t channel = 0; channel < 6; ++channel) {
        const auto& wire = mix.routing.sources[0].connections[channel];
        assert(wire.sourceChannel == channel && wire.outputChannel == channel && wire.gain == 1);
    }
    const auto check = [](const auto& actual, std::initializer_list<ChannelConnection> expected) {
        assert(actual.size() == expected.size());
        size_t index = 0;
        for (const auto& wire : expected) {
            assert(actual[index].sourceChannel == wire.sourceChannel &&
                   actual[index].outputChannel == wire.outputChannel &&
                   actual[index].gain == wire.gain);
            ++index;
        }
    };
    assert(mix.routing.sources[1].channels == 8);
    check(mix.routing.sources[1].connections,
          {{0, 0, 0.25f}, {6, 4, 0.75f}, {7, 1, 0.125f}, {5, 2, 0.25f}, {2, 5, 0.5f}});
    assert(mix.monitors[0].channels == 8);
    check(mix.monitors[0].connections, {{0, 0, 0.25f}, {4, 7, 0.75f}});
    assert(dependent.routing.outputChannels == 8 && dependent.routing.sources[0].channels == 6);
    check(dependent.routing.sources[0].connections, {{0, 7, 0.5f}, {4, 1, 0.75f}, {5, 2, 0.125f}});
    assert(!dependent.routing.sources[0].enabled && dependent.routing.sources[0].muted &&
           dependent.routing.sources[0].gain == 0.5f);
    assert(changed.devices[2].routing.outputChannels == 4);
    const auto cross =
        edited(original,
               {
                   SetSourceWiring{
                       "mix",
                       "pass-through",
                       {{0, 7, 0.125f}, {2, 0, 0.5f}, {6, 1, 0.75f}, {1, 3, 0.25f}, {6, 6, 0.5f}}},
                   RemoveOutputPair{"mix", 2},
               });
    check(cross.devices[0].routing.sources[0].connections,
          {{0, 5, 0.125f}, {4, 1, 0.75f}, {4, 4, 0.5f}});
    check(changed.devices[2].routing.sources[0].connections,
          {{0, 0, 1}, {1, 1, 1}, {2, 2, 1}, {3, 3, 1}});
    for (const auto first : {1U, 8U, UINT32_MAX})
        invalid([&] {
            (void)edited(original, {RemoveOutputPair{"mix", first}});
        });
    invalid([&] {
        (void)edited(original, {RemoveOutputPair{"missing", 0}});
    });
    invalid([&] {
        (void)edited(original, {RemoveOutputPair{"mix", 2}, RemoveOutputPair{"mix", 9}});
    });
    assert(original.devices[0].routing.sources[0].connections.size() == 8);

    // Actual zdsp output proves physical input channels did not shift with the
    // virtual output pair. Channel 3 still routes to the former output 8.
    std::array<std::array<float, 64>, 8> input{};
    std::array<std::array<float, 64>, 6> zero{}, output{};
    std::array<const float*, 8> physical{};
    std::array<const float*, 6> pass{};
    std::array<float*, 6> planes{};
    for (uint32_t channel = 0; channel < 8; ++channel) {
        input[channel].fill((channel + 1) * 0.01f);
        physical[channel] = input[channel].data();
    }
    for (uint32_t channel = 0; channel < 6; ++channel) {
        pass[channel] = zero[channel].data();
        planes[channel] = output[channel].data();
    }
    for (const auto rate : {44100U, 48000U, 96000U, 192000U}) {
        RoutingGraph graph(rate, mix.routing);
        const std::array<RoutingInput, 2> inputs{{{pass, 64}, {physical, 64}}};
        assert(graph.render(inputs, {planes, 64}, 64, 0));
        const std::array<float, 6> expected{0.0025f, 0.01f, 0.015f, 0, 0.0525f, 0.015f};
        for (size_t channel = 0; channel < output.size(); ++channel)
            for (const auto sample : output[channel])
                assert(std::abs(sample - expected[channel]) < 1e-6f);
    }
    for (uint32_t channels = 4; channels <= 64; channels += 2)
        for (uint32_t first = 0; first < channels; first += 2) {
            const auto single = edited({}, {CreateDevice{"pair", "Pair", channels}});
            const auto smaller = edited(single, {RemoveOutputPair{"pair", first}});
            const auto& route = smaller.devices[0].routing;
            assert(route.outputChannels == channels - 2 &&
                   route.sources[0].channels == channels - 2);
            assert(route.sources[0].connections.size() == channels - 2);
            for (uint32_t channel = 0; channel < channels - 2; ++channel)
                assert(route.sources[0].connections[channel].sourceChannel == channel &&
                       route.sources[0].connections[channel].outputChannel == channel);
        }
    const auto stereo = edited({}, {CreateDevice{"stereo", "Stereo", 2}});
    invalid([&] {
        (void)edited(stereo, {RemoveOutputPair{"stereo", 0}});
    });
}

void creationAndControls() {
    auto document = initial();
    const auto& mix = document.devices[0];
    assert(mix.id == "mix" && mix.routing.sources.size() == 3);
    assert(mix.routing.sources[0].channels == 4);
    assert(mix.routing.sources[1].connections.size() == 2);
    assert(mix.routing.sources[1].connections[0].sourceChannel == 0);
    assert(mix.routing.sources[1].connections[1].outputChannel == 1);
    assert(!mix.bindings[1].muteOriginal && mix.bindings[2].muteOriginal);
    assert(document.devices[1].routing.sources[0].channels == 4);

    document =
        edited(document,
               {
                   SetDeviceControls{"mix", {.gain = 0.5f, .enabled = false, .muted = true}, false},
                   SetSourceControls{
                       "mix", "app", {.gain = 0.25f, .enabled = false, .muted = true}, false},
                   SetMonitorControls{
                       "mix", "headphones", {.gain = 0.75f, .enabled = false, .muted = true}},
               });
    const auto& changed = document.devices[0];
    assert(changed.routing.gain == 0.5f && !changed.routing.enabled && changed.routing.muted);
    assert(!changed.routing.safetyLimiter);
    assert(changed.routing.sources[2].gain == 0.25f && !changed.routing.sources[2].enabled &&
           changed.routing.sources[2].muted && !changed.bindings[2].muteOriginal);
    assert(changed.monitors[0].gain == 0.75f && !changed.monitors[0].enabled &&
           changed.monitors[0].muted);
    document = edited(document, {SetDeviceControls{"mix", {.enabled = true, .muted = false}, true},
                                 SetSourceControls{"mix", "app", {.muted = false}, true}});
    assert(document.devices[0].routing.enabled && !document.devices[0].routing.muted);
    assert(document.devices[0].routing.gain == 0.5f);
    assert(document.devices[0].bindings[2].muteOriginal);
}

void wiringAndResizing() {
    auto document =
        edited(initial(), {
                              SetSourceWiring{"mix", "app", {{0, 3, 0.5f}, {1, 0, 0.25f}, {0, 1}}},
                              SetMonitorEndpoint{"mix", "headphones", "wide-output", 8},
                              SetMonitorWiring{"mix", "headphones", {{3, 7, 0.5f}, {0, 6, 0.25f}}},
                              SetSourceWiring{"call", "stream", {{3, 1}, {0, 0, 0.5f}}},
                          });
    document = edited(document, {ResizeDevice{"mix", 2}});
    const auto& mix = document.devices[0];
    assert(mix.routing.outputChannels == 2 && mix.routing.sources[0].channels == 2);
    assert(mix.routing.sources[2].connections.size() == 2);
    assert(mix.routing.sources[2].connections[0].gain == 0.25f);
    assert(mix.monitors[0].channels == 8 && mix.monitors[0].connections.size() == 1);
    assert(mix.monitors[0].connections[0].sourceChannel == 0 &&
           mix.monitors[0].connections[0].outputChannel == 6);
    assert(document.devices[1].routing.sources[0].channels == 2);
    assert(document.devices[1].routing.sources[0].connections.size() == 1);
    document = edited(document, {ResizeDevice{"mix", 64}});
    const auto& pass = document.devices[0].routing.sources[0];
    assert(pass.channels == 64 && pass.connections.size() == 64);
    assert(pass.connections.back().sourceChannel == 63 &&
           pass.connections.back().outputChannel == 63);
    assert(document.devices[1].routing.sources[0].channels == 64);
    // Growing outputs doesn't silently reconnect intentionally removed wires.
    assert(document.devices[0].routing.sources[2].connections.size() == 2);
    assert(document.devices[0].monitors[0].connections.size() == 1);
    document = edited(document, {SetInputChannels{"mix", "mic", 64},
                                 SetSourceWiring{"mix", "mic", {{63, 63}, {0, 0}}},
                                 SetInputChannels{"mix", "mic", 1}});
    assert(document.devices[0].routing.sources[1].connections.size() == 1);
    assert(document.devices[0].routing.sources[1].connections[0].sourceChannel == 0);
}

void referencesAndAtomicFailures() {
    const auto document = initial();
    invalid([&] {
        edited(document, {DeleteDevice{"mix"}});
    });
    invalid([&] {
        edited(document, {SetDeviceControls{"mix", {.enabled = false}, {}},
                          AddSource{"mix", "cycle", SourceKind::VirtualDevice, "call"}});
    });
    invalid([&] {
        edited(document,
               {RenameDevice{"mix", "Should not commit"}, RemoveSource{"mix", "missing"}});
    });
    assert(document.devices[0].name == "Stream Mix" && document.devices[0].routing.enabled);
    assert(document.devices.size() == 2 && document.devices[1].bindings[0].selector == "mix");
    auto removed = edited(document, {RemoveSource{"call", "stream"}, DeleteDevice{"mix"}});
    assert(removed.devices.size() == 1 && removed.devices[0].id == "call");
    assert(removed.devices[0].routing.sources.empty() && removed.devices[0].bindings.empty());

    auto disabled = edited(document, {SetDeviceControls{"call", {.enabled = false}, {}},
                                      SetSourceControls{"call", "stream", {.enabled = false}, {}}});
    invalid([&] {
        edited(disabled, {DeleteDevice{"mix"}});
    });
    invalid([&] {
        edited(disabled, {SetSourceSelector{"call", "stream", "call"}});
    });
}

void identityReceiptsAndReordering() {
    auto document = initial();
    document.devices[0].formatRequestReceipt = "native-origin:17";
    document = edited(document, {DuplicateDevice{"mix", "copy", "Copied Mix"},
                                 RenameDevice{"mix", "Renamed Mix"}, SetDeviceRate{"mix", 44100},
                                 ReorderDevices{{"copy", "call", "mix"}},
                                 ReorderSources{"mix", {"app", "pass-through", "mic"}},
                                 AddMonitor{"mix", "speakers", "speaker-uid"},
                                 ReorderMonitors{"mix", {"speakers", "headphones"}}});
    const auto& copy = document.devices[0];
    const auto& original = document.devices[2];
    assert(copy.id == "copy" && copy.formatRequestReceipt.empty());
    assert(copy.sampleRate == 48000 && copy.routing.sources[0].id == "pass-through");
    assert(original.id == "mix" && original.name == "Renamed Mix" && original.sampleRate == 44100);
    assert(original.formatRequestReceipt == "native-origin:17");
    assert(document.devices[1].bindings[0].selector == "mix");
    for (size_t index = 0; index < original.bindings.size(); ++index)
        assert(original.bindings[index].id == original.routing.sources[index].id);
    assert(original.monitors[0].id == "speakers");
    invalid([&] {
        edited(document, {ReorderDevices{{"mix", "mix", "copy"}}});
    });
    invalid([&] {
        edited(document, {ReorderDevices{{"mix", "call"}}});
    });
    invalid([&] {
        edited(document, {ReorderDevices{{"mix", "call", "unknown"}}});
    });
    invalid([&] {
        edited(document, {DuplicateDevice{"mix", "mix", "Duplicate ID"}});
    });
}

void endpointChangesAndRemoval() {
    auto document = edited(initial(), {CreateDevice{"stereo", "Stereo", 2, 48000, false},
                                       SetSourceWiring{"call", "stream", {{3, 1}, {1, 0, 0.5f}}},
                                       SetSourceSelector{"call", "stream", "stereo"},
                                       SetSourceSelector{"mix", "mic", "unplugged-mic"},
                                       SetSourceSelector{"mix", "app", "com.example.other"},
                                       SetMonitorWiring{"mix", "headphones", {{0, 0}, {3, 1}}},
                                       SetMonitorEndpoint{"mix", "headphones", "mono-output", 1}});
    assert(document.devices[1].routing.sources[0].channels == 2);
    assert(document.devices[1].routing.sources[0].connections.size() == 1);
    assert(document.devices[1].routing.sources[0].connections[0].gain == 0.5f);
    assert(document.devices[0].bindings[1].selector == "unplugged-mic");
    assert(document.devices[0].bindings[2].selector == "com.example.other");
    assert(document.devices[0].monitors[0].channels == 1);
    assert(document.devices[0].monitors[0].connections.size() == 1);
    document =
        edited(document, {RemoveSource{"mix", "pass-through"}, RemoveMonitor{"mix", "headphones"}});
    assert(!hasPlaybackEndpoint(document.devices[0]));
    assert(document.devices[0].bindings.size() == 2 && document.devices[0].monitors.empty());
    document = edited(document, {AddSource{"mix", "new-pass", SourceKind::PassThrough, ""}});
    assert(hasPlaybackEndpoint(document.devices[0]));
    invalid([&] {
        edited(document, {SetSourceSelector{"mix", "new-pass", "uid"}});
    });
    invalid([&] {
        edited(document, {SetInputChannels{"mix", "app", 1}});
    });
    invalid([&] {
        edited(document, {SetSourceControls{"mix", "mic", {}, false}});
    });
}

void boundsAndValidation() {
    const auto document = initial();
    invalid([&] {
        edited(document, {ResizeDevice{"mix", 3}});
    });
    invalid([&] {
        edited(document, {SetInputChannels{"mix", "mic", 65}});
    });
    invalid([&] {
        edited(document, {AddMonitor{"mix", "bad", "uid", 0, false}});
    });
    invalid([&] {
        edited(document, {AddMonitor{"mix", "app", "uid"}});
    });
    invalid([&] {
        edited(document, {AddSource{"mix", "other-pass", SourceKind::PassThrough, ""}});
    });
    invalid([&] {
        edited(document, {AddSource{"mix", "pass2", SourceKind::PassThrough, "uid"}});
    });
    invalid([&] {
        edited(document, {AddSource{"mix", "app", SourceKind::InputDevice, "uid"}});
    });
    invalid([&] {
        edited(document, {AddSource{"mix", "none", SourceKind::VirtualDevice, "missing"}});
    });
    invalid([&] {
        edited(document, {SetSourceWiring{"mix", "mic", {{0, 0}, {0, 0}}}});
    });
    invalid([&] {
        edited(document, {SetMonitorWiring{"mix", "headphones", {{4, 0}}}});
    });
    invalid([&] {
        edited(document, {SetSourceWiring{"mix", "app", {{2, 0}}}});
    });
    invalid([&] {
        edited(document, {SetDeviceControls{"mix", {.gain = -0.1f}, {}}});
    });
    invalid([&] {
        edited(document, {SetSourceControls{
                             "mix", "app", {.gain = std::numeric_limits<float>::quiet_NaN()}, {}}});
    });
    invalid([&] {
        edited(document, {SetDeviceRate{"mix", std::numeric_limits<double>::infinity()}});
    });
    invalid([&] {
        edited(document, {RenameDevice{"mix", std::string("nul\0name", 8)}});
    });
    invalid([&] {
        edited(document, {});
    });
    std::vector<Command> tooMany(kMaximumCommands + 1, RenameDevice{"mix", "Mix"});
    invalid([&] {
        applyCommands(document, tooMany);
    });
    auto all = edited({}, {CreateDevice{"base", "Base", 64}});
    for (uint32_t index = 1; index < kMaximumVirtualDevices; ++index)
        all = edited(all, {DuplicateDevice{"base", "device-" + std::to_string(index), "Device"}});
    invalid([&] {
        edited(all, {CreateDevice{"excess", "Excess"}});
    });
    auto legacy = initial();
    legacy.version = 1;
    const auto upgraded = edited(legacy, {RenameDevice{"mix", "Upgraded"}});
    assert(upgraded.version == kRouteDocumentVersion && legacy.version == 1);
}

void editsReachRealDSP(double rate) {
    auto document = edited({}, {CreateDevice{"mix", "Mix", 2, rate, false},
                                AddSource{"mix", "mono", SourceKind::InputDevice, "uid", 1},
                                SetDeviceControls{"mix", {.gain = 0.5f}, false},
                                SetSourceControls{"mix", "mono", {.gain = 0.5f}, {}}});
    std::array<float, 64> input{}, left{}, right{};
    input.fill(0.8f);
    const float* channels[]{input.data()};
    float* outputs[]{left.data(), right.data()};
    const std::array<RoutingInput, 1> sources{{{channels, 64}}};
    auto render = [&](float expectedLeft, float expectedRight) {
        RoutingGraph graph(rate, document.devices[0].routing);
        assert(graph.render(sources, {outputs, 64}, 64, 0));
        for (uint32_t index = 0; index < 64; ++index) {
            assert(std::fabs(left[index] - expectedLeft) < 0.00001f);
            assert(std::fabs(right[index] - expectedRight) < 0.00001f);
        }
    };
    render(0.2f, 0.2f);
    document = edited(document, {SetSourceWiring{"mix", "mono", {{0, 1, 0.25f}}}});
    render(0, 0.05f);
    document = edited(document, {SetDeviceControls{"mix", {.muted = true}, {}}});
    render(0, 0);
    document = edited(document, {SetDeviceControls{"mix", {.muted = false}, {}},
                                 SetSourceControls{"mix", "mono", {.enabled = false}, {}}});
    render(0, 0);
    document = edited(document, {SetSourceControls{"mix", "mono", {.enabled = true}, {}}});
    render(0, 0.05f);
}
} // namespace

int main() {
    arbitraryOutputPairs();
    creationAndControls();
    wiringAndResizing();
    referencesAndAtomicFailures();
    identityReceiptsAndReordering();
    endpointChangesAndRemoval();
    boundsAndValidation();
    editsReachRealDSP(44100);
    editsReachRealDSP(48000);
    std::puts("Portable routing commands, atomic failures and DSP behavior passed");
}
