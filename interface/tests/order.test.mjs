import test from "node:test";
import assert from "node:assert/strict";
import { engineFixture } from "./fixtures.mjs";
import { project, commandsFor } from "../src/model.js";
import { EngineStore } from "../src/store.js";

test("list moves send complete identity permutations and reject stale orders", () => {
    const data = engineFixture();
    data.snapshot.document.devices.push({
        ...structuredClone(data.snapshot.document.devices[0]),
        id: "other",
        name: "Other",
    });
    const state = project(data),
        device = state.devices[0];
    device.sources.push({ ...device.sources[0], id: "second-source" });
    device.monitors.push({ ...device.monitors[0], id: "second-monitor" });
    const original = structuredClone(state.devices);
    for (const [kind, id, expected] of [
        ["device", "mix", { type: "reorderDevices", ids: ["other", "mix"] }],
        [
            "source",
            "app",
            {
                type: "reorderSources",
                device: "mix",
                ids: ["second-source", "app"],
            },
        ],
        [
            "monitor",
            "monitor",
            {
                type: "reorderMonitors",
                device: "mix",
                ids: ["second-monitor", "monitor"],
            },
        ],
    ]) {
        assert.deepEqual(
            commandsFor(state, { type: "move", kind, id, offset: 1 }),
            [expected],
        );
        assert.throws(() =>
            commandsFor(state, { type: "move", kind, id, offset: -1 }),
        );
        assert.throws(() =>
            commandsFor(state, { type: "move", kind, id, offset: 2 }),
        );
        assert.throws(
            () =>
                commandsFor(state, {
                    type: "move",
                    kind,
                    id,
                    offset: 1,
                    expectedOrder: ["stale"],
                }),
            /list changed/,
        );
        assert.throws(() =>
            commandsFor(state, {
                type: "move",
                kind,
                id: "missing",
                offset: 1,
            }),
        );
    }
    assert.deepEqual(state.devices, original);
});

test("device selection remains pinned when state arrives before a reorder reply", async (t) => {
    const data = engineFixture();
    data.snapshot.document.devices.push({
        ...structuredClone(data.snapshot.document.devices[0]),
        id: "other",
        name: "Other",
    });
    let acknowledge, submitted;
    const reached = new Promise((resolve) => (submitted = resolve));
    const saved = [];
    const store = new EngineStore(
        async (message) => {
            if (message.operation === "state") return structuredClone(data);
            assert.deepEqual(message.request.commands, [
                { type: "reorderDevices", ids: ["other", "mix"] },
            ]);
            data.snapshot.document.devices.reverse();
            data.snapshot.revision.generation = "9007199254740994";
            submitted();
            return new Promise((resolve) => (acknowledge = resolve));
        },
        {},
        (preferences) => saved.push(structuredClone(preferences)),
    );
    t.after(() => clearTimeout(store.expiry));
    store.accept(structuredClone(data));
    assert.equal(store.state.selected, "mix");
    const pending = store.send({
        type: "move",
        kind: "device",
        id: "mix",
        offset: 1,
    });
    await reached;
    await store.refresh();
    assert.equal(store.state.devices[0].id, "other");
    assert.equal(store.state.selected, "mix");
    assert.equal(saved.at(-1).selected, "mix");
    acknowledge({ ok: true });
    assert(await pending);
    assert.equal(store.state.selected, "mix");
});
