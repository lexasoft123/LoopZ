import test from "node:test";
import assert from "node:assert/strict";
import {
    commandsFor,
    project,
    outputAmplitude,
    sourceAmplitude,
    monitorAmplitude,
    dependencies,
} from "../src/model.js";
import { EngineStore } from "../src/store.js";
import { newIdentity } from "../src/identity.js";
import { engineFixture as fixture } from "./fixtures.mjs";

test("UI meters come only from active telemetry and clear when unavailable", () => {
    const data = fixture();
    const state = project(data);
    assert.equal(outputAmplitude(state, "mix", 1), 0.2);
    assert.equal(sourceAmplitude(state, state.devices[0].sources[0], 1), 0.7);
    assert.equal(
        monitorAmplitude(state, state.devices[0].monitors[0], 1),
        0.12,
    );
    data.snapshot.document.devices[0].routing.gain = 0;
    data.snapshot.document.devices[0].routing.muted = true;
    assert.equal(outputAmplitude(project(data), "mix", 1), 0.2);
    assert.equal(
        outputAmplitude({ ...state, meterExpired: true }, "mix", 1),
        0,
    );
    data.connection.ready = false;
    assert.equal(outputAmplitude(project(data), "mix", 1), 0);
});
test("adding a source leaves channel wiring explicit", () => {
    const state = project(fixture());
    for (const kind of ["hardware", "app", "virtual", "pass"]) {
        const [command] = commandsFor(state, {
            type: "add-source",
            source: {
                id: "new-source",
                kind,
                selector: "selected-source",
                channels: 8,
            },
        });
        assert.equal(command.type, "addSource");
        assert.equal(command.connectByDefault, false);
    }
});
test("one wire edit preserves every unrelated fractional wire gain", () => {
    const data = fixture();
    const original = structuredClone(data);
    const state = project(data);
    const commands = commandsFor(state, {
        type: "route",
        kind: "source",
        id: "app",
        channel: 1,
        output: 2,
    });
    assert.deepEqual(
        commands[0].connections.slice(0, 2),
        original.snapshot.document.devices[0].routing.sources[0].connections,
    );
    assert.deepEqual(commands[0].connections[2], {
        sourceChannel: 0,
        outputChannel: 1,
        gain: 1,
    });
    const monitor = commandsFor(state, {
        type: "route",
        kind: "monitor",
        id: "monitor",
        channel: 2,
        output: 1,
    })[0];
    assert.deepEqual(monitor.connections, [
        { sourceChannel: 1, outputChannel: 0, gain: 0.2 },
        { sourceChannel: 0, outputChannel: 1, gain: 1 },
    ]);
    assert.deepEqual(data, original);
    const gain = commandsFor(state, {
        type: "source",
        id: "app",
        patch: { gain: -6 },
    })[0];
    assert.ok(Math.abs(gain.controls.gain - 10 ** (-6 / 20)) < 1e-9);
    assert.equal("formatRequestReceipt" in gain, false);
});
test("device deletion explicitly removes disabled dependent virtual sources", () => {
    const data = fixture();
    const clone = structuredClone(data.snapshot.document.devices[0]);
    clone.id = "call";
    clone.name = "Call";
    clone.routing.enabled = false;
    clone.bindings[0] = {
        id: "app",
        kind: "virtual-device",
        selector: "mix",
        muteOriginal: false,
    };
    data.snapshot.document.devices.push(clone);
    const state = project(data, { selected: "mix" });
    assert.equal(dependencies(state, "mix").length, 1);
    assert.throws(() => commandsFor(state, { type: "delete-device" }));
    assert.deepEqual(
        commandsFor(state, { type: "delete-device", removeDependencies: true }),
        [
            { type: "removeSource", device: "call", source: "app" },
            { type: "deleteDevice", device: "mix" },
        ],
    );
});
test("first-run creation uses the explicit initializer and a lossless revision", async (t) => {
    const data = fixture();
    data.snapshot.document = null;
    data.snapshot.canCreateDocument = true;
    data.snapshot.revision.generation = "0";
    const calls = [];
    const preferences = [];
    const store = new EngineStore(
        async (message) => {
            calls.push(message);
            return message.operation === "state" ? data : { ok: true };
        },
        {},
        (value) => preferences.push(structuredClone(value)),
    );
    t.after(() => clearTimeout(store.expiry));
    store.accept(data);
    assert.equal(store.state.canCreate, true);
    assert.equal(
        await store.send({
            type: "create",
            id: "first",
            name: "First",
            channels: 8,
        }),
        true,
    );
    const change = calls.find((c) => c.operation === "initialize");
    assert.equal(change.request.expected.generation, "0");
    assert.equal(change.request.commands[0].id, "first");
    assert.equal(change.request.commands[0].passThrough, false);
    assert.equal(preferences.at(-1).selected, "first");
    data.snapshot.canCreateDocument = false;
    store.accept(data);
    assert.equal(
        await store.send({
            type: "create",
            id: "second",
            name: "Second",
            channels: 2,
        }),
        false,
    );
    assert.equal(calls.filter((c) => c.operation === "initialize").length, 1);
});
test("an uncertain save refreshes state without replay and requires explicit review", async (t) => {
    const data = fixture();
    const calls = [];
    const store = new EngineStore(async (message) => {
        calls.push(message);
        if (message.operation === "state") return data;
        return { ok: false, code: "uncertain" };
    });
    t.after(() => clearTimeout(store.expiry));
    store.accept(data);
    assert.equal(await store.send({ type: "rename", name: "Renamed" }), false);
    assert.equal(store.state.uncertain, true);
    assert.equal(store.state.canEdit, false);
    await store.refresh();
    await store.send({ type: "clear-error" });
    assert.equal(store.state.canEdit, false);
    assert.ok(store.state.error);
    assert.equal(await store.send({ type: "rename", name: "Replay" }), false);
    assert.equal(calls.filter((c) => c.operation === "edit").length, 1);
    assert.equal(calls[0].request.expected.generation, "9007199254740993");
    assert.equal(await store.send({ type: "acknowledge" }), true);
    assert.equal(store.state.canEdit, true);
});
test("a lost WebKit reply is uncertain even if no structured error arrived", async (t) => {
    const data = fixture();
    let edits = 0;
    const store = new EngineStore(async (message) => {
        if (message.operation === "state") return data;
        ++edits;
        throw new Error("bridge lost");
    });
    t.after(() => clearTimeout(store.expiry));
    store.accept(data);
    assert.equal(await store.send({ type: "rename", name: "Saved?" }), false);
    assert.equal(store.state.uncertain, true);
    assert.equal(edits, 1);
    await store.refresh();
    assert.equal(edits, 1);
});
test("state expiry disables editing, meters and inherited numeric revisions", async (t) => {
    const data = fixture();
    const store = new EngineStore(async () => data);
    t.after(() => clearTimeout(store.expiry));
    store.accept(data);
    await new Promise((resolve) => setTimeout(resolve, 280));
    assert.equal(store.state.canEdit, false);
    assert.equal(outputAmplitude(store.state, "mix", 1), 0);
    const bad = structuredClone(data);
    bad.snapshot.revision.generation = 42;
    assert.throws(() => store.accept(bad));
    assert.equal(store.state.snapshot.revision.generation, "9007199254740993");
});
test("friendly names survive app exit and projection identities stay stable for meters", (t) => {
    const data = fixture();
    let saved;
    const store = new EngineStore(
        async () => data,
        { devices: ["must not be persisted"] },
        (preferences) => {
            saved = structuredClone(preferences);
        },
    );
    t.after(() => clearTimeout(store.expiry));
    store.accept(data);
    const device = store.state.devices[0];
    const next = structuredClone(data);
    next.snapshot.runtime.devices[0].meter.peak = [0.9, 0.9];
    store.accept(next);
    assert.equal(store.state.devices[0], device);
    assert.equal(outputAmplitude(store.state, "mix", 1), 0.9);
    next.inventory.applications = [];
    store.accept(next);
    assert.equal(store.state.devices[0].sources[0].name, "Player");
    assert.equal(saved.appNames["example.player"], "Player");
    assert.equal(saved.devices, undefined);
});
test("new identities use platform randomness and remain stable opaque UUIDs", () => {
    const a = newIdentity(),
        b = newIdentity();
    assert.match(
        a,
        /^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/,
    );
    assert.notEqual(a, b);
});
test("background and login state remains available while routing is disconnected", (t) => {
    const data = {
        version: 1,
        connection: { connected: false, ready: false },
        snapshot: null,
        background: {
            managed: true,
            detail: "Restoring routing",
            login: {
                available: true,
                registered: true,
                requiresApproval: true,
            },
        },
    };
    let preferences;
    const store = new EngineStore(
        async () => data,
        {},
        (value) => {
            preferences = value;
        },
    );
    t.after(() => clearTimeout(store.expiry));
    store.accept(data);
    assert.equal(store.state.background.managed, true);
    assert.equal(store.state.background.login.requiresApproval, true);
    assert.equal(store.state.canEdit, false);
    store.send({ type: "theme", value: "atelier" });
    assert.equal(preferences.background, undefined);
    assert.equal(preferences.login, undefined);
});
