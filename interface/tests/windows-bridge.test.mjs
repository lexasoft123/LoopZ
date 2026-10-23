import test from "node:test";
import assert from "node:assert/strict";
import { nativeTransport } from "../src/store.js";
import {
    commandsFor,
    project,
    endpointChoices,
    appPlaybackLabel,
} from "../src/model.js";
import { engineFixture } from "./fixtures.mjs";

test("Windows app sources preserve original playback until routing is requested", () => {
    const fixture = engineFixture();
    fixture.platform = "windows";
    fixture.previewNotice = "Windows preview";
    const state = project(fixture, {}, {});
    assert.equal(state.platform, "windows");
    assert.equal(state.previewNotice, "Windows preview");
    const action = {
        type: "add-source",
        device: "mix",
        source: { id: "new-app", kind: "app", selector: "player", channels: 2 },
    };
    assert.equal(commandsFor(state, action)[0].muteOriginal, false);
    assert.equal(
        commandsFor({ ...state, platform: "macos" }, action)[0].muteOriginal,
        true,
    );
});

test("Windows playback controls require backend capability and confirmed routing", () => {
    const fixture = engineFixture();
    fixture.platform = "windows";
    assert.equal(project(fixture).canRouteAppPlayback, false);
    fixture.snapshot.runtime.editCapabilities = { appOutputRouting: true };
    const state = project(fixture);
    assert.equal(state.canRouteAppPlayback, true);
    const source = state.devices[0].sources.find(
        (value) => value.kind === "app",
    );
    const requested = { ...source, muteOriginal: true };
    assert.equal(
        appPlaybackLabel(state, requested),
        "Routing original playback…",
    );
    fixture.snapshot.runtime.sources.find(
        (value) => value.kind === "application",
    ).playbackRouting = { requested: true, verified: true };
    assert.equal(
        appPlaybackLabel(state, requested),
        "Original playback routed",
    );
    assert.equal(
        appPlaybackLabel({ ...state, meterExpired: true }, requested),
        "Routing original playback…",
    );
    assert.equal(
        appPlaybackLabel(state, { ...source, muteOriginal: false }),
        "Original playback routing off",
    );
    assert.equal(
        commandsFor(state, {
            type: "source",
            device: state.devices[0].id,
            id: source.id,
            patch: { muteOriginal: true },
        })[0].muteOriginal,
        true,
    );
});

test("WebView2 correlates replies and propagates native errors", async () => {
    let listener;
    const sent = [];
    const old = globalThis.chrome;
    globalThis.chrome = {
        webview: {
            addEventListener(event, callback) {
                assert.equal(event, "message");
                listener = callback;
            },
            postMessage(message) {
                sent.push(message);
            },
        },
    };
    try {
        const first = nativeTransport({ version: 1, operation: "state" });
        const second = nativeTransport({ version: 1, operation: "edit" });
        listener({ data: { id: sent[1].id, result: { ok: true } } });
        listener({ data: { id: sent[0].id, result: { version: 1 } } });
        assert.deepEqual(await first, { version: 1 });
        assert.deepEqual(await second, { ok: true });
        listener({ data: { id: 999999, result: { ignored: true } } });
        const failed = nativeTransport({ version: 1, operation: "state" });
        listener({ data: { id: sent[2].id, error: "Engine unavailable" } });
        await assert.rejects(failed, /Engine unavailable/);
        globalThis.chrome.webview.postMessage = () => {
            throw Error("Disconnected");
        };
        await assert.rejects(nativeTransport({}), /Disconnected/);
    } finally {
        globalThis.chrome = old;
    }
});

test("managed LoopZ endpoints cannot be chosen as physical inputs or monitors", () => {
    const state = {
        inventory: {
            devices: [
                {
                    uid: "speaker",
                    name: "Headphones",
                    inputChannels: 0,
                    outputChannels: 2,
                },
                {
                    uid: "mic",
                    name: "Microphone",
                    inputChannels: 2,
                    outputChannels: 0,
                },
                {
                    uid: "feed",
                    name: "LoopZ Feed",
                    virtualRole: "loopz-feed",
                    inputChannels: 0,
                    outputChannels: 2,
                },
                {
                    uid: "published",
                    name: "LoopZ Microphone",
                    virtualRole: "loopz-microphone",
                    inputChannels: 2,
                    outputChannels: 0,
                },
            ],
            applications: [],
        },
        devices: [],
    };
    const monitor = endpointChoices(
        state,
        {},
        { selector: "speaker" },
        "monitor",
    );
    assert.deepEqual(
        monitor.map((x) => x.selector),
        ["speaker"],
    );
    const sources = endpointChoices(
        state,
        {},
        { kind: "hardware", selector: "mic" },
        "source",
    );
    assert.deepEqual(
        sources.map((x) => x.selector),
        ["default", "mic"],
    );
});

test("an unavailable Windows microphone does not report a stopped audio service", () => {
    const fixture = engineFixture();
    fixture.platform = "windows";
    fixture.snapshot.restoration.phase = "partial";
    fixture.snapshot.runtime.restored = false;
    const state = project(fixture, {}, {});
    assert.equal(state.service, "ready");
    assert.equal(state.connected, true);
    assert.equal(state.canEdit, true);
});
