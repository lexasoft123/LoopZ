import test from "node:test";
import assert from "node:assert/strict";
import { engineFixture } from "./fixtures.mjs";
import { project, commandsFor } from "../src/model.js";
import { outputPairRemoval } from "../src/output-pairs.js";

function fixture() {
    const envelope = engineFixture();
    envelope.snapshot.runtime.editCapabilities = { removeOutputPair: true };
    const mix = envelope.snapshot.document.devices[0];
    mix.routing.outputChannels = 8;
    mix.routing.sources.push({
        id: "pass",
        channels: 8,
        gain: 1,
        enabled: true,
        muted: false,
        connections: Array.from({ length: 8 }, (_, channel) => ({
            sourceChannel: channel,
            outputChannel: channel,
            gain: 1,
        })),
    });
    mix.bindings.push({
        id: "pass",
        kind: "pass-through",
        selector: "",
        muteOriginal: false,
    });
    mix.routing.sources[0].connections.push(
        { sourceChannel: 0, outputChannel: 2, gain: 0.5 },
        { sourceChannel: 0, outputChannel: 6, gain: 0.125 },
    );
    mix.monitors[0].connections.push(
        { sourceChannel: 2, outputChannel: 1, gain: 0.5 },
        { sourceChannel: 6, outputChannel: 1, gain: 0.25 },
    );
    const downstream = structuredClone(mix);
    Object.assign(downstream, {
        id: "dependent",
        name: "Dependent",
        bindings: [
            {
                id: "virtual",
                kind: "virtual-device",
                selector: "mix",
                muteOriginal: false,
            },
        ],
        monitors: [],
    });
    downstream.routing.sources = [
        {
            id: "virtual",
            channels: 8,
            gain: 0.5,
            enabled: false,
            muted: true,
            connections: [
                { sourceChannel: 2, outputChannel: 0, gain: 0.25 },
                { sourceChannel: 6, outputChannel: 7, gain: 0.75 },
            ],
        },
    ];
    envelope.snapshot.document.devices.push(downstream);
    return envelope;
}

test("pair review counts source, pass-through, monitor and disabled dependent wires", () => {
    const envelope = fixture();
    const before = JSON.stringify(envelope);
    const state = project(envelope);
    const review = outputPairRemoval(state, "mix", 3);
    assert.equal(review.channels, 6);
    assert.equal(review.removedConnections, 5);
    assert.equal(review.shiftedConnections, 7);
    assert.equal(review.dependentDevices, 1);
    assert.equal(JSON.stringify(envelope), before);
    assert.deepEqual(
        commandsFor(state, {
            type: "remove-output-pair",
            device: "mix",
            start: 3,
            basis: review.basis,
        }),
        [{ type: "removeOutputPair", device: "mix", firstChannel: 2 }],
    );
});

test("a changed dependent wire invalidates a reviewed pair before transport", () => {
    const envelope = fixture();
    const review = outputPairRemoval(project(envelope), "mix", 3);
    envelope.snapshot.document.devices[1].routing.sources[0].connections[0].gain = 0.5;
    assert.throws(
        () =>
            commandsFor(project(envelope), {
                type: "remove-output-pair",
                device: "mix",
                start: 3,
                basis: review.basis,
            }),
        /Connections changed/,
    );
});

test("control changes do not unnecessarily invalidate the reviewed channel wiring", () => {
    const envelope = fixture();
    const review = outputPairRemoval(project(envelope), "mix", 3);
    envelope.snapshot.document.devices[0].routing.sources[0].gain = 0.25;
    envelope.snapshot.document.devices[1].name = "New dependent name";
    assert.equal(
        outputPairRemoval(project(envelope), "mix", 3).basis,
        review.basis,
    );
});

test("older or malformed helper capabilities never authorize pair removal", () => {
    for (const value of [undefined, false, 1, "true"]) {
        const envelope = fixture();
        envelope.snapshot.runtime.editCapabilities.removeOutputPair = value;
        const state = project(envelope);
        assert.equal(state.canRemoveOutputPair, false);
        const review = outputPairRemoval(state, "mix", 3);
        assert.throws(
            () =>
                commandsFor(state, {
                    type: "remove-output-pair",
                    device: "mix",
                    start: 3,
                    basis: review.basis,
                }),
            /Update LoopZ/,
        );
    }
});

test("invalid pairs, the final pair and unreviewed submissions are rejected", () => {
    const state = project(fixture());
    for (const start of [0, 2, 8, 1.5, null, NaN])
        assert.throws(() => outputPairRemoval(state, "mix", start));
    assert.throws(() => outputPairRemoval(state, "missing", 1));
    assert.equal(outputPairRemoval(state, "mix", 7).channels, 6);
    assert.throws(
        () =>
            commandsFor(state, {
                type: "remove-output-pair",
                device: "mix",
                start: 3,
            }),
        /Connections changed/,
    );
    const stereo = project(engineFixture());
    assert.throws(() => outputPairRemoval(stereo, "mix", 1));
});
