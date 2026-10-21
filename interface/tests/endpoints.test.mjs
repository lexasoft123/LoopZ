import test from "node:test";
import assert from "node:assert/strict";
import { engineFixture } from "./fixtures.mjs";
import {
    commandsFor,
    project,
    endpointChoices,
    endpointWiresRemoved,
    endpointLayout,
} from "../src/model.js";

test("endpoint edits preserve stable identities and unrelated controls and wiring", () => {
    const state = project(engineFixture());
    const original = structuredClone(state.devices);
    const source = state.devices[0].sources[0];
    assert.deepEqual(
        commandsFor(state, {
            type: "source-endpoint",
            id: "app",
            selector: "other.player",
            expectedLayout: endpointLayout(source),
        }),
        [
            {
                type: "setSourceSelector",
                device: "mix",
                source: "app",
                selector: "other.player",
            },
        ],
    );
    source.kind = "hardware";
    assert.deepEqual(
        commandsFor(state, {
            type: "source-endpoint",
            id: "app",
            selector: "usb-new",
            channels: 8,
        }),
        [
            {
                type: "setSourceSelector",
                device: "mix",
                source: "app",
                selector: "usb-new",
            },
            {
                type: "setInputChannels",
                device: "mix",
                source: "app",
                channels: 8,
            },
        ],
    );
    source.kind = "app";
    assert.deepEqual(
        commandsFor(state, {
            type: "monitor-endpoint",
            id: "monitor",
            selector: "new-phones",
            channels: 1,
        }),
        [
            {
                type: "setMonitorEndpoint",
                device: "mix",
                monitor: "monitor",
                selector: "new-phones",
                channels: 1,
            },
        ],
    );
    assert.deepEqual(state.devices, original);
});

test("saved unavailable endpoints remain selectable and virtual feedback is blocked", () => {
    const state = project(engineFixture());
    const device = state.devices[0],
        node = device.sources[0];
    node.kind = "hardware";
    node.selector = "unplugged";
    const choices = endpointChoices(state, device, node, "source");
    assert(choices.some((item) => item.selector === "default"));
    assert(choices.find((item) => item.selector === "unplugged").unavailable);
    node.kind = "virtual";
    node.selector = "upstream";
    state.devices.push({
        id: "upstream",
        name: "Upstream",
        channels: 8,
        sources: [],
    });
    state.devices.push({
        id: "dependent",
        name: "Dependent",
        channels: 2,
        sources: [{ kind: "virtual", deviceId: "mix" }],
    });
    const virtual = endpointChoices(state, device, node, "source");
    assert(virtual.find((item) => item.selector === "mix").blocked);
    assert(virtual.find((item) => item.selector === "dependent").blocked);
    assert(!virtual.find((item) => item.selector === "upstream").blocked);
    assert.throws(
        () =>
            commandsFor(state, {
                type: "source-endpoint",
                id: node.id,
                selector: "dependent",
            }),
        /routing loop/,
    );
    assert.throws(
        () =>
            commandsFor(state, {
                type: "source-endpoint",
                id: node.id,
                selector: "gone",
            }),
        /routing loop/,
    );
});

test("channel reductions count the correct wire dimension and recheck changed layouts", () => {
    const state = project(engineFixture());
    const source = state.devices[0].sources[0],
        monitor = state.devices[0].monitors[0];
    source.connections = [
        { sourceChannel: 7, outputChannel: 0, gain: 0.25 },
        { sourceChannel: 0, outputChannel: 7, gain: 0.5 },
    ];
    assert.equal(endpointWiresRemoved(source, "source", 2), 1);
    assert.equal(endpointWiresRemoved(source, "monitor", 2), 1);
    const expected = endpointLayout(source);
    source.connections[0].gain = 0.75;
    assert.throws(
        () =>
            commandsFor(state, {
                type: "source-endpoint",
                id: source.id,
                selector: "other",
                expectedLayout: expected,
            }),
        /Connections changed/,
    );
    const savedMonitor = endpointLayout(monitor);
    monitor.selector = "external-edit";
    assert.throws(
        () =>
            commandsFor(state, {
                type: "monitor-endpoint",
                id: monitor.id,
                selector: "other",
                channels: 2,
                expectedLayout: savedMonitor,
            }),
        /Connections changed/,
    );
    for (const channels of [0, 65, NaN, 1.5, "2"]) {
        assert.throws(() =>
            commandsFor(state, {
                type: "monitor-endpoint",
                id: monitor.id,
                selector: "other",
                channels,
            }),
        );
        source.kind = "hardware";
        assert.throws(() =>
            commandsFor(state, {
                type: "source-endpoint",
                id: source.id,
                selector: "other",
                channels,
            }),
        );
    }
    source.kind = "pass";
    assert.throws(
        () =>
            commandsFor(state, {
                type: "source-endpoint",
                id: source.id,
                selector: "other",
            }),
        /Pass-through/,
    );
});
