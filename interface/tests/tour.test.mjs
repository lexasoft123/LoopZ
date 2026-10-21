import { createTourDemoState } from "../src/tour-demo.js";
import {
    selected,
    outputAmplitude,
    sourceAmplitude,
    monitorAmplitude,
} from "../src/model.js";
import test from "node:test";
import assert from "node:assert/strict";
import {
    tourPlacement,
    tourSeen,
    rememberTour,
    tourSteps,
} from "../src/tour.js";

test("the demo tour covers the whole app even with no user project", () => {
    const steps = tourSteps();
    assert.equal(steps.length, 8);
    for (const target of ["outputs", "monitors", "sources", "pass-through"])
        assert(steps.some((step) => step.target?.includes(target)));
    assert(steps.some((step) => step.body.includes("Zoom")));
    assert(steps[0].body.includes("No audio is captured or played"));
});

test("the demo has independent wired sources, output and monitor with example levels", () => {
    const state = createTourDemoState("night-studio");
    const device = selected(state);
    assert.equal(device.name, "Demo Mix");
    assert.equal(device.channels, 2);
    assert.equal(device.sources.length, 2);
    assert.deepEqual(device.sources[0].routes, [[1, 2]]);
    assert.deepEqual(device.sources[1].routes, [[1], [2]]);
    assert.equal(device.monitors[0].name, "Demo headphones");
    assert.deepEqual(device.monitors[0].routes, [[1], [2]]);
    assert.equal(outputAmplitude(state, device.id, 1), 0.58);
    assert.equal(sourceAmplitude(state, device.sources[0], 1), 0.45);
    assert.equal(monitorAmplitude(state, device.monitors[0], 1), 0.38);
    device.name = "Changed";
    assert.equal(selected(createTourDemoState()).name, "Demo Mix");
});

test("dismissal persists and unavailable browser storage does not prevent using the app", () => {
    const values = new Map();
    const storage = {
        getItem: (key) => values.get(key),
        setItem: (key, value) => values.set(key, value),
    };
    assert.equal(tourSeen(storage), false);
    rememberTour(storage);
    assert.equal(tourSeen(storage), true);
    const blocked = {
        getItem() {
            throw Error("blocked");
        },
        setItem() {
            throw Error("blocked");
        },
    };
    assert.doesNotThrow(() => rememberTour(blocked));
    assert.equal(tourSeen(blocked), false);
});

test("callouts fit the viewport after resizing and stay clear of targets when space permits", () => {
    for (const viewport of [
        { width: 1440, height: 900 },
        { width: 360, height: 420 },
    ]) {
        for (const rect of [
            null,
            { left: 20, top: 20, right: 80, bottom: 60 },
            { left: 80, top: 300, right: 300, bottom: 400 },
        ]) {
            const placement = tourPlacement(rect, viewport, { height: 260 });
            assert(placement.left >= 16 && placement.top >= 16);
            assert(placement.left + placement.width <= viewport.width - 16);
            assert(placement.top + 260 <= viewport.height - 16);
        }
    }
    const target = { left: 30, top: 50, right: 180, bottom: 90 };
    assert(
        tourPlacement(target, { width: 1440, height: 900 }, { height: 240 })
            .top > target.bottom,
    );
});
