import test from "node:test";
import assert from "node:assert/strict";
import {
    project,
    health,
    sourceStatus,
    sourceAmplitude,
    outputAmplitude,
} from "../src/model.js";
import {
    sourceCondition,
    monitorCondition,
    microphoneAccess,
} from "../src/connection-state.js";
import { engineFixture } from "./fixtures.mjs";

function hardwareFixture() {
    const data = engineFixture();
    data.snapshot.document.devices[0].bindings[0] = {
        id: "app",
        kind: "input-device",
        selector: "input",
        muteOriginal: false,
    };
    data.snapshot.runtime.permissions = { microphone: "authorized" };
    data.snapshot.runtime.sources = [
        {
            kind: "input-device",
            selector: "input",
            demanded: true,
            connected: true,
            complete: true,
            error: "",
        },
    ];
    return data;
}
const sourceOf = (state) => state.devices[0].sources[0];

test("revoked input permission overrides a previously healthy capture without changing saved wiring", () => {
    const data = hardwareFixture();
    const saved = structuredClone(data.snapshot.document);
    const initial = project(data);
    assert.equal(sourceCondition(initial, sourceOf(initial)).kind, "available");
    data.snapshot.runtime.permissions.microphone = "denied";
    const revoked = project(data);
    assert.equal(
        sourceCondition(revoked, sourceOf(revoked)).kind,
        "permission",
    );
    assert.equal(
        sourceCondition(revoked, sourceOf(revoked)).action.kind,
        "settings",
    );
    assert.match(sourceStatus(revoked, sourceOf(revoked)), /access required/);
    assert.equal(health(revoked, revoked.devices[0]), "Source waiting");
    assert.equal(sourceAmplitude(revoked, sourceOf(revoked), 1), 0);
    data.snapshot.runtime.permissions.microphone = "authorized";
    const restored = project(data);
    assert.equal(
        sourceCondition(restored, sourceOf(restored)).kind,
        "available",
    );
    assert.deepEqual(data.snapshot.document, saved);
});

test("inactive routes and offline sources cannot retain live meters from an earlier block", () => {
    const data = hardwareFixture();
    let state = project(data);
    assert(sourceAmplitude(state, sourceOf(state), 1) > 0);
    data.snapshot.runtime.devices[0].sources[0].online = false;
    state = project(data);
    assert.equal(sourceAmplitude(state, sourceOf(state), 1), 0);
    assert(outputAmplitude(state, "mix", 1) > 0);
    data.snapshot.runtime.devices[0].active = false;
    state = project(data);
    assert.equal(outputAmplitude(state, "mix", 1), 0);
    assert.equal(sourceAmplitude(state, sourceOf(state), 1), 0);
});

test("denied, restricted and unrequested access have distinct guidance; idle inputs do not claim a live capture", () => {
    const data = hardwareFixture();
    for (const permission of ["denied", "restricted", "not-determined"]) {
        data.snapshot.runtime.permissions.microphone = permission;
        let state = project(data);
        assert.equal(
            sourceCondition(state, sourceOf(state)).kind,
            "permission",
        );
        data.snapshot.runtime.sources[0].demanded = false;
        state = project(data);
        assert.equal(sourceCondition(state, sourceOf(state)).kind, "idle");
        data.snapshot.runtime.sources[0].demanded = true;
    }
    assert.match(microphoneAccess("denied"), /turned off/);
    assert.match(microphoneAccess("restricted"), /restricted/);
    assert.match(microphoneAccess("not-determined"), /not been granted/);
    assert.match(microphoneAccess(undefined), /could not be checked/);
});

test("stale or disconnected state cannot advertise healthy sources or monitors or offer a misleading retry", () => {
    const data = hardwareFixture();
    for (const [change, flags] of [
        [
            (value) => {
                value.connection.connected = false;
            },
            {},
        ],
        [
            (value) => {
                value.connection.ready = false;
            },
            {},
        ],
        [() => {}, { meterExpired: true }],
    ]) {
        const copy = structuredClone(data);
        change(copy);
        const state = project(copy, {}, flags);
        assert.equal(sourceCondition(state, sourceOf(state)).kind, "unknown");
        assert.equal(sourceCondition(state, sourceOf(state)).action, null);
        assert.equal(
            monitorCondition(state, state.devices[0].monitors[0]).kind,
            "unknown",
        );
        assert.equal(state.monitorFaults.monitor, "Status unavailable");
    }
});

test("a shared input with fewer physical channels offers source review rather than a permission request", () => {
    const data = hardwareFixture();
    data.snapshot.runtime.sources[0].complete = false;
    let state = project(data);
    const partial = sourceCondition(state, sourceOf(state));
    assert.equal(partial.kind, "partial");
    assert.equal(partial.action.kind, "source-options");
    assert.match(partial.detail, /Available channels keep working/);
    data.snapshot.runtime.sources[0].complete = true;
    state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "available");
});

test("native error wording cannot choose UI actions or misclassify application failure as microphone denial", () => {
    const data = engineFixture();
    Object.assign(data.snapshot.runtime.sources[0], {
        connected: false,
        complete: false,
        demanded: true,
        error: "Process access lookup failed (internal callback details)",
    });
    const state = project(data);
    const failure = sourceCondition(state, sourceOf(state));
    assert.equal(failure.kind, "waiting");
    assert.equal(failure.action.kind, "retry");
    assert.equal(failure.label, "Waiting for application audio");
    assert(!failure.label.includes("internal"));
});

test("a disabled virtual source device points to its saved device instead of retrying acquisition", () => {
    const data = hardwareFixture();
    const target = structuredClone(data.snapshot.document.devices[0]);
    target.id = "upstream";
    target.name = "Upstream";
    target.routing.enabled = false;
    target.routing.sources = [];
    target.bindings = [];
    target.monitors = [];
    data.snapshot.document.devices.push(target);
    Object.assign(data.snapshot.document.devices[0].bindings[0], {
        kind: "virtual-device",
        selector: "upstream",
    });
    let state = project(data);
    assert.deepEqual(sourceCondition(state, sourceOf(state)).action, {
        kind: "select-device",
        label: "Show device",
        device: "upstream",
    });
    target.routing.enabled = true;
    state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "available");
});

test("restoration and disabled-state guidance do not claim that old endpoint telemetry is current", () => {
    const data = hardwareFixture();
    data.snapshot.restoration.phase = "waiting-for-driver";
    let state = project(data);
    assert.equal(
        sourceCondition(state, sourceOf(state)).label,
        "Waiting for audio service",
    );
    data.snapshot.restoration.phase = "restored";
    data.snapshot.restoration.activeGeneration = "0";
    state = project(data);
    assert.equal(
        sourceCondition(state, sourceOf(state)).label,
        "Applying changes",
    );
    data.snapshot.document.devices[0].routing.sources[0].enabled = false;
    data.snapshot.document.devices[0].monitors[0].enabled = false;
    state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "disabled");
    assert.equal(
        monitorCondition(state, state.devices[0].monitors[0]).kind,
        "disabled",
    );
});

function dependencyFixture() {
    const data = hardwareFixture();
    const consumer = data.snapshot.document.devices[0];
    const upstream = structuredClone(consumer);
    upstream.id = "physical";
    upstream.name = "Physical input";
    upstream.monitors = [];
    const middle = structuredClone(consumer);
    middle.id = "middle";
    middle.name = "Dependency";
    middle.monitors = [];
    Object.assign(middle.bindings[0], {
        kind: "virtual-device",
        selector: upstream.id,
    });
    Object.assign(consumer.bindings[0], {
        kind: "virtual-device",
        selector: middle.id,
    });
    data.snapshot.document.devices.push(middle, upstream);
    return data;
}

test("virtual sources report upstream loss through dependencies while live channels keep their meters", () => {
    const data = dependencyFixture();
    const saved = structuredClone(data.snapshot.document);
    let state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "available");
    Object.assign(data.snapshot.runtime.sources[0], {
        demanded: true,
        connected: false,
        complete: false,
    });
    state = project(data);
    const issue = sourceCondition(state, sourceOf(state));
    assert.equal(issue.kind, "partial");
    assert.equal(issue.label, "Upstream source unavailable");
    assert.deepEqual(issue.action, {
        kind: "select-device",
        label: "Show device",
        device: "physical",
    });
    assert(sourceAmplitude(state, sourceOf(state), 1) > 0);
    assert.equal(health(state, state.devices[0]), "Source waiting");
    Object.assign(data.snapshot.runtime.sources[0], {
        connected: true,
        complete: true,
    });
    state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "available");
    assert.deepEqual(data.snapshot.document, saved);
});

test("quiet audio, upstream monitor loss and unused failed sources do not become dependency faults", () => {
    const data = dependencyFixture();
    data.snapshot.runtime.devices[0].sources[0].meter.peak = [0, 0];
    data.snapshot.document.devices[2].monitors = structuredClone(
        data.snapshot.document.devices[0].monitors,
    );
    data.snapshot.runtime.monitors[0].device = "physical";
    data.snapshot.runtime.monitors[0].connected = false;
    data.snapshot.runtime.monitors[0].complete = false;
    let state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "available");
    assert.equal(health(state, state.devices[2]), "Monitor waiting");
    Object.assign(data.snapshot.runtime.sources[0], {
        demanded: true,
        connected: false,
        complete: false,
    });
    const upstream = data.snapshot.document.devices[2].routing.sources[0];
    const connections = upstream.connections;
    for (const change of [
        () => {
            upstream.connections = [];
        },
        () => {
            upstream.connections = connections.map((wire) => ({
                ...wire,
                gain: 0,
            }));
        },
        () => {
            upstream.connections = connections;
            upstream.enabled = false;
        },
    ]) {
        change();
        state = project(data);
        assert.equal(sourceCondition(state, sourceOf(state)).kind, "available");
    }
});

test("dependency health follows partial capture and microphone access but bounds invalid cycles", () => {
    const data = dependencyFixture();
    data.snapshot.runtime.sources[0].complete = false;
    let state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "partial");
    data.snapshot.runtime.sources[0].complete = true;
    data.snapshot.runtime.permissions.microphone = "denied";
    state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "partial");
    data.snapshot.runtime.permissions.microphone = "authorized";
    data.snapshot.document.devices[2].bindings[0] = {
        id: "app",
        kind: "virtual-device",
        selector: "mix",
        muteOriginal: false,
    };
    state = project(data);
    assert.equal(sourceCondition(state, sourceOf(state)).kind, "partial");
});
