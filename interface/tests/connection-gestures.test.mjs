import test from "node:test";
import assert from "node:assert/strict";
import { commandsFor, project } from "../src/model.js";
import {
    parseSocket,
    connectionAction,
} from "../src/components/connection-gestures.js";
import { engineFixture } from "./fixtures.mjs";
test("stable socket identifiers and compatible pairs in either drag direction", () => {
    const s = parseSocket("source:id:with:colons:2");
    assert.equal(s.id, "id:with:colons");
    assert.equal(s.channel, 2);
    const t = parseSocket("output-input:bus:1");
    assert.deepEqual(connectionAction(s, t), connectionAction(t, s));
    assert.equal(connectionAction(s, s), null);
});
test("duplicate drops retain wire weights; atomic branch moves preserve fan-out", () => {
    const state = project(engineFixture());
    const a = {
        type: "route",
        kind: "source",
        id: "app",
        channel: 1,
        output: 1,
        mode: "add",
    };
    const before = state.devices[0].sources[0].connections;
    assert.deepEqual(commandsFor(state, a)[0].connections, before);
    const moved = commandsFor(state, { ...a, output: 2, previous: a });
    assert.equal(moved.length, 1);
    assert.deepEqual(moved[0].connections, [
        before[1],
        { sourceChannel: 0, outputChannel: 1, gain: 0.25 },
    ]);
    assert.deepEqual(state.devices[0].sources[0].connections, before);
    assert.deepEqual(
        commandsFor(state, { ...a, mode: "remove" })[0].connections,
        [before[1]],
    );
});
test("cross-monitor move uses a single batch and preserves branch gain", () => {
    const data = engineFixture(),
        d = data.snapshot.document.devices[0];
    d.monitors.push({
        ...structuredClone(d.monitors[0]),
        id: "other",
        connections: [],
    });
    const state = project(data);
    const actions = commandsFor(state, {
        type: "route",
        kind: "monitor",
        id: "other",
        channel: 2,
        output: 1,
        mode: "add",
        previous: { kind: "monitor", id: "monitor", channel: 1, output: 2 },
    });
    assert.equal(actions.length, 2);
    assert.equal(actions[0].monitor, "monitor");
    assert.equal(actions[1].monitor, "other");
    assert.equal(
        actions[1].connections[0].gain,
        d.monitors[0].connections[0].gain,
    );
});
