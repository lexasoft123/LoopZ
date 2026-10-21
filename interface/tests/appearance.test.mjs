import test from "node:test";
import assert from "node:assert/strict";
import { normalizeAppearance, resolveAppearance } from "../src/appearance.js";
import { EngineStore } from "../src/store.js";
import { engineFixture } from "./fixtures.mjs";

test("fresh or invalid choices follow the system; existing explicit palettes survive", () => {
    for (const value of [undefined, null, "", "invalid", {}, false]) {
        assert.equal(normalizeAppearance(value), "system");
        assert.equal(resolveAppearance(value, false), "atelier");
        assert.equal(resolveAppearance(value, true), "night-studio");
    }
    for (const value of ["atelier", "night-studio"]) {
        assert.equal(resolveAppearance(value, false), value);
        assert.equal(resolveAppearance(value, true), value);
        assert.equal(
            new EngineStore(async () => {}, { theme: value }).state.theme,
            value,
        );
    }
});

test("system preference persists through store reconstruction without routing transport", async () => {
    let calls = 0;
    let saved;
    const transport = async () => {
        calls++;
        throw new Error("Appearance must not edit routing");
    };
    const store = new EngineStore(
        transport,
        { theme: "night-studio" },
        (value) => {
            saved = structuredClone(value);
        },
    );
    assert.equal(await store.send({ type: "theme", value: "system" }), true);
    assert.equal(saved.theme, "system");
    const restored = new EngineStore(transport, saved);
    assert.equal(restored.state.theme, "system");
    assert.equal(calls, 0);
});

test("appearance edits leave routing, revision and uncertain-save state untouched", async () => {
    const data = engineFixture();
    const before = structuredClone(data);
    const store = new EngineStore(async () => {
        throw new Error("No routing request");
    });
    store.accept(data);
    clearTimeout(store.expiry);
    store.flags.uncertain = true;
    store.publish();
    for (const value of ["atelier", "night-studio", "system"]) {
        assert.equal(await store.send({ type: "theme", value }), true);
        assert.equal(store.state.theme, value);
        assert.equal(store.flags.uncertain, true);
        assert.deepEqual(store.envelope, before);
    }
});

test("invalid theme commands do not write preferences or notify routing", async () => {
    let saves = 0;
    const store = new EngineStore(
        async () => {
            throw new Error("No routing request");
        },
        {},
        () => saves++,
    );
    for (const value of ["invalid", null, {}, "SYSTEM"]) {
        assert.equal(await store.send({ type: "theme", value }), false);
    }
    assert.equal(store.state.theme, "system");
    assert.equal(saves, 0);
});

test("malformed stored preference containers fall back to system appearance", () => {
    for (const value of [null, [], false, "invalid", 2]) {
        assert.equal(
            new EngineStore(async () => {}, value).state.theme,
            "system",
        );
    }
});
