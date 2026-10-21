import assert from "node:assert/strict";
import test from "node:test";
import { project } from "../src/model.js";
import {
    installationComponents,
    installationCondition,
} from "../src/installation.js";
import { engineFixture } from "./fixtures.mjs";

const ready = () => ({
    checked: true,
    components: Object.fromEntries(
        ["engine", "driver", "service"].map((id) => [
            id,
            { state: "installed", detail: "" },
        ]),
    ),
});

test("older transports and an in-progress check do not invent a setup failure", () => {
    const data = engineFixture();
    assert.deepEqual(installationComponents(project(data)), []);
    assert.equal(installationCondition(project(data)), null);
    data.installation = { checked: false };
    assert(
        installationComponents(project(data)).every(
            (c) => c.label === "Checking",
        ),
    );
    assert.equal(installationCondition(project(data)), null);
});

test("missing engine, driver and service have separate installation guidance", () => {
    const data = engineFixture();
    for (const id of ["engine", "driver", "service"]) {
        data.installation = ready();
        data.installation.components[id] = {
            state: "missing",
            detail: `Install the ${id}`,
        };
        const issue = installationCondition(project(data));
        assert.equal(issue.heading, "Finish LoopZ setup");
        assert.equal(issue.detail, `Install the ${id}`);
    }
});

test("an invalid component takes precedence over missing components", () => {
    const data = engineFixture();
    data.installation = ready();
    data.installation.components.engine = {
        state: "missing",
        detail: "Install engine",
    };
    data.installation.components.driver = {
        state: "invalid",
        detail: "Repair driver",
    };
    const state = project(data);
    assert.equal(installationCondition(state).detail, "Repair driver");
    assert.equal(installationCondition(state).kind, "invalid");
});

test("incomplete and unknown inspection never implies successful installation", () => {
    const data = engineFixture();
    data.installation = ready();
    delete data.installation.components.driver;
    assert.equal(installationCondition(project(data)).kind, "unknown");
    assert.equal(
        installationComponents(project(data))[1].label,
        "Could not check",
    );
    data.installation.components.driver = {
        state: "unknown",
        detail: "Retry check",
    };
    assert.equal(installationCondition(project(data)).detail, "Retry check");
});

test("driver installation and live connection are distinct, and an empty document is healthy", () => {
    const data = engineFixture();
    data.installation = ready();
    assert.equal(installationCondition(project(data)), null);
    data.snapshot.restoration.phase = "no-document";
    assert.equal(installationCondition(project(data)), null);
    data.snapshot.restoration.phase = "waiting-for-driver";
    assert.equal(
        installationCondition(project(data)).kind,
        "driver-connection",
    );
    data.snapshot.restoration.phase = "restored";
    assert.equal(installationCondition(project(data)), null);
});

test("setup telemetry does not replace saved routing or edit permissions", () => {
    const data = engineFixture();
    const saved = structuredClone(data.snapshot.document);
    const previous = project(data);
    data.installation = ready();
    data.installation.components.service = {
        state: "invalid",
        detail: "Repair service",
    };
    const state = project(data);
    assert.deepEqual(data.snapshot.document, saved);
    assert.deepEqual(state.devices, previous.devices);
    assert.equal(state.canEdit, previous.canEdit);
    assert.equal(state.canCreate, previous.canCreate);
});
