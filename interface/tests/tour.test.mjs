import test from "node:test";
import assert from "node:assert/strict";
import {
    tourPlacement,
    tourSeen,
    rememberTour,
    tourSteps,
} from "../src/tour.js";

test("the tour covers the full routing workflow and adapts to an empty workspace", () => {
    const populated = tourSteps(true, true);
    assert(populated.some((step) => step.target?.includes("outputs")));
    assert(populated.some((step) => step.target?.includes("monitors")));
    assert(populated.some((step) => step.target?.includes("sources")));
    assert(populated.some((step) => step.target?.includes("pass-through")));
    assert(populated.some((step) => step.body.includes("Zoom")));
    const empty = tourSteps(false, false);
    assert(
        empty.every(
            (step) => !step.target || /new-device|settings/.test(step.target),
        ),
    );
    assert(empty.at(-1).body.includes("creating a device"));
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
