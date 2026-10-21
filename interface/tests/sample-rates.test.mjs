import test from "node:test";
import assert from "node:assert/strict";
import {
    supportedSampleRates,
    initialDeviceRate,
} from "../src/sample-rates.js";
import { project, commandsFor } from "../src/model.js";
import { engineFixture } from "./fixtures.mjs";

test("older or malformed capabilities retain the two-rate compatibility path", () => {
    for (const sampleRates of [
        undefined,
        [],
        [96000, 96000],
        [44100.5],
        ["96000"],
        [0],
        [384000],
    ])
        assert.deepEqual(
            supportedSampleRates({ runtime: { sampleRates } }),
            [44100, 48000],
        );
});

test("new backend rate capabilities are sorted without changing the snapshot", () => {
    const rates = [192000, 44100, 96000, 48000, 88200, 176400];
    const original = [...rates];
    assert.deepEqual(
        supportedSampleRates({ runtime: { sampleRates: rates } }),
        [44100, 48000, 88200, 96000, 176400, 192000],
    );
    assert.deepEqual(rates, original);
});

test("new devices use the supported default output rate and can explicitly override it", () => {
    const envelope = engineFixture();
    envelope.snapshot.runtime.sampleRates = [
        44100, 48000, 88200, 96000, 176400, 192000,
    ];
    Object.assign(envelope.inventory.devices[1], {
        defaultOutput: true,
        rate: 96000,
    });
    const state = project(envelope);
    assert.equal(initialDeviceRate(state), 96000);
    const action = { type: "create", id: "new", name: "New", channels: 2 };
    assert.equal(commandsFor(state, action)[0].sampleRate, 96000);
    assert.equal(
        commandsFor(state, { ...action, rate: 192000 })[0].sampleRate,
        192000,
    );
    assert.throws(
        () => commandsFor(state, { ...action, rate: 96001 }),
        /supported sample rate/,
    );
});

test("an older backend never advertises or receives a high-rate create command", () => {
    const envelope = engineFixture();
    Object.assign(envelope.inventory.devices[1], {
        defaultOutput: true,
        rate: 96000,
    });
    const state = project(envelope);
    assert.deepEqual(state.sampleRates, [44100, 48000]);
    assert.equal(initialDeviceRate(state), 48000);
    assert.throws(() =>
        commandsFor(state, {
            type: "create",
            id: "new",
            name: "New",
            channels: 2,
            rate: 96000,
        }),
    );
});

test("stale or ambiguous default-output observations do not choose the creation format", () => {
    const inventory = {
        devices: [{ defaultOutput: true, rate: 96000 }],
        devicesFresh: false,
    };
    const state = { sampleRates: [44100, 48000, 96000], inventory };
    assert.equal(initialDeviceRate(state), 48000);
    inventory.devicesFresh = true;
    inventory.devices.push({ defaultOutput: true, rate: 44100 });
    assert.equal(initialDeviceRate(state), 48000);
});
