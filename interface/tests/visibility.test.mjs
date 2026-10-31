import test from "node:test";
import assert from "node:assert/strict";
import { setImmediate as flush } from "node:timers/promises";
import { observeUIVisibility, startVisiblePolling } from "../src/visibility.js";
import { EngineStore } from "../src/store.js";
import { engineFixture } from "./fixtures.mjs";

function visibility(initial = true) {
    let change;
    return {
        observe(callback) {
            change = callback;
            callback(initial);
            return () => {
                change = null;
            };
        },
        set(value) {
            change?.(value);
        },
    };
}

test("native occlusion and document visibility both gate updates, not focus", () => {
    const page = new EventTarget();
    const document = new EventTarget();
    document.hidden = false;
    document.documentElement = { dataset: {} };
    const states = [];
    const stop = observeUIVisibility((visible) => states.push(visible), {
        page,
        document,
    });
    page.dispatchEvent(new Event("blur"));
    assert.deepEqual(states, [true]);
    page.__loopzNativeVisible = false;
    page.dispatchEvent(new Event("loopz-visibilitychange"));
    page.dispatchEvent(new Event("loopz-visibilitychange"));
    document.hidden = true;
    document.dispatchEvent(new Event("visibilitychange"));
    page.__loopzNativeVisible = true;
    page.dispatchEvent(new Event("loopz-visibilitychange"));
    assert.deepEqual(states, [true, false]);
    document.hidden = false;
    document.dispatchEvent(new Event("visibilitychange"));
    assert.deepEqual(states, [true, false, true]);
    assert.equal(document.documentElement.dataset.loopzUiVisible, "true");
    stop();
    page.__loopzNativeVisible = false;
    page.dispatchEvent(new Event("loopz-visibilitychange"));
    assert.equal(states.length, 3);
});

test("hidden polling and meter expiry stop; showing reads immediately", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const view = visibility(false);
    let reads = 0,
        publishes = 0;
    const store = new EngineStore(async () => {
        ++reads;
        return engineFixture();
    });
    store.subscribe(() => ++publishes);
    const stop = store.start(view.observe);
    t.mock.timers.tick(10000);
    await flush();
    assert.equal(reads, 0);
    view.set(true);
    assert.equal(reads, 1, "show starts a read without advancing the clock");
    await flush();
    assert.equal(store.state.meterExpired, false);
    view.set(false);
    const frozen = publishes;
    t.mock.timers.tick(10000);
    await flush();
    assert.equal(reads, 1);
    assert.equal(
        publishes,
        frozen,
        "no meter-expiry or projection publish while hidden",
    );
    view.set(true);
    assert.equal(reads, 2);
    await flush();
    t.mock.timers.tick(100);
    await flush();
    assert.equal(reads, 3, "one polling loop resumes");
    stop();
    t.mock.timers.tick(10000);
    await flush();
    assert.equal(reads, 3);
});

test("an old in-flight response is discarded and show queues one immediate fresh read", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const view = visibility();
    const pending = [];
    const store = new EngineStore(
        () => new Promise((resolve) => pending.push(resolve)),
    );
    const stop = store.start(view.observe);
    assert.equal(pending.length, 1);
    view.set(false);
    view.set(true);
    assert.equal(pending.length, 1, "do not overlap native reads");
    pending[0](engineFixture());
    await flush();
    assert.equal(store.envelope, null, "do not publish the pre-hide response");
    assert.equal(
        pending.length,
        2,
        "refresh immediately after the old read drains",
    );
    pending[1](engineFixture());
    await flush();
    assert.equal(store.state.connected, true);
    stop();
});

test("stop during an in-flight read cannot publish or restart timers", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const view = visibility();
    let complete,
        reads = 0;
    const store = new EngineStore(() => {
        ++reads;
        return new Promise((r) => {
            complete = r;
        });
    });
    const stop = store.start(view.observe);
    stop();
    complete(engineFixture());
    await flush();
    t.mock.timers.tick(10000);
    await flush();
    assert.equal(store.envelope, null);
    assert.equal(reads, 1);
});

test("a rejected read retries at the normal interval, not in a busy loop", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    let reads = 0;
    const stop = startVisiblePolling(
        async () => {
            ++reads;
            throw Error("offline");
        },
        {
            interval: 5000,
            observe: visibility().observe,
        },
    );
    await flush();
    assert.equal(reads, 1);
    t.mock.timers.tick(4999);
    await flush();
    assert.equal(reads, 1);
    t.mock.timers.tick(1);
    await flush();
    assert.equal(reads, 2);
    stop();
});

test("restarting a store while its old read drains still requests fresh state immediately", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const view = visibility();
    const pending = [];
    const store = new EngineStore(
        () => new Promise((resolve) => pending.push(resolve)),
    );
    store.start(view.observe)();
    const stop = store.start(view.observe);
    assert.equal(pending.length, 1);
    pending[0](engineFixture());
    await flush();
    assert.equal(pending.length, 2);
    assert.equal(store.envelope, null);
    pending[1](engineFixture());
    await flush();
    assert.equal(store.state.connected, true);
    stop();
});

for (const transition of ["hide/show", "stop/restart"]) {
    test(`direct refresh cannot publish a pre-${transition} response`, async (t) => {
        t.mock.timers.enable({ apis: ["setTimeout"] });
        const view = visibility();
        const pending = [];
        const store = new EngineStore(
            () => new Promise((r) => pending.push(r)),
        );
        let stop = store.start(view.observe);
        pending[0](engineFixture());
        await flush();
        const reading = store.refresh();
        assert.equal(pending.length, 2);
        if (transition === "hide/show") {
            view.set(false);
            view.set(true);
        } else {
            stop();
            stop = store.start(view.observe);
        }
        const stale = engineFixture();
        stale.snapshot.document.devices[0].name = "Pre-hide stale snapshot";
        pending[1](stale);
        assert.equal(await reading, false);
        await flush();
        assert.equal(
            pending.length,
            3,
            "a new read starts without advancing time",
        );
        assert.notEqual(
            store.envelope.snapshot.document.devices[0].name,
            "Pre-hide stale snapshot",
        );
        pending[2](engineFixture());
        await flush();
        stop();
    });
}
