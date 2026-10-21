import test from "node:test";
import assert from "node:assert/strict";
import { EngineStore } from "../src/store.js";
import { engineFixture } from "./fixtures.mjs";

test("undo and redo send only revision-fenced history actions", async (t) => {
    const envelope = engineFixture();
    envelope.snapshot.history = { canUndo: true, canRedo: false };
    const messages = [];
    const store = new EngineStore(async (message) => {
        messages.push(message);
        if (message.operation === "state") return envelope;
        envelope.snapshot.revision.generation = (
            BigInt(envelope.snapshot.revision.generation) + 1n
        ).toString();
        envelope.snapshot.history = {
            canUndo: message.request.history === "redo",
            canRedo: message.request.history === "undo",
        };
        return { ok: true };
    });
    t.after(() => clearTimeout(store.expiry));
    store.accept(envelope);
    assert.equal(await store.send({ type: "undo" }), true);
    assert.equal(await store.send({ type: "redo" }), true);
    const requests = messages.filter((message) => message.operation === "edit");
    assert.equal(requests.length, 2);
    assert.deepEqual(requests[0].request, {
        version: 1,
        expected: { epoch: "engine-one", generation: "9007199254740993" },
        history: "undo",
    });
    assert.equal(requests[1].request.expected.generation, "9007199254740994");
    assert.equal(requests[1].request.history, "redo");
    assert.equal("commands" in requests[1].request, false);
});

test("unsupported, empty and disconnected history cannot send an edit", async (t) => {
    for (const history of [undefined, { canUndo: false, canRedo: false }]) {
        const envelope = engineFixture();
        envelope.snapshot.history = history;
        const messages = [];
        const store = new EngineStore(async (message) => {
            messages.push(message);
            return envelope;
        });
        t.after(() => clearTimeout(store.expiry));
        store.accept(envelope);
        assert.equal(await store.send({ type: "undo" }), false);
        assert(!messages.some((message) => message.operation === "edit"));
        envelope.snapshot.history = { canUndo: true, canRedo: true };
        envelope.connection.ready = false;
        store.accept(envelope);
        assert.equal(await store.send({ type: "redo" }), false);
        assert(!messages.some((message) => message.operation === "edit"));
    }
});

test("a lost undo reply requires review and is never replayed", async (t) => {
    const envelope = engineFixture();
    envelope.snapshot.history = { canUndo: true, canRedo: false };
    const requests = [];
    const store = new EngineStore(async (message) => {
        if (message.operation === "state") return envelope;
        requests.push(message);
        throw new Error("Reply lost");
    });
    t.after(() => clearTimeout(store.expiry));
    store.accept(envelope);
    assert.equal(await store.send({ type: "undo" }), false);
    await store.refresh();
    assert.equal(store.state.uncertain, true);
    assert.equal(await store.send({ type: "undo" }), false);
    assert.equal(requests.length, 1);
    assert.equal(requests[0].request.history, "undo");
});
