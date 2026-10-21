import assert from "node:assert/strict";
import test from "node:test";
import { bufferLatencyLabel } from "../src/hardware-buffer-model.js";

test("buffer targets use the actual hardware rates and distinguish CPU profiles", () => {
    assert.equal(
        bufferLatencyLabel(64, [{ rate: 48000 }]),
        "64 frames · 1.33 ms target/buffer · Balanced",
    );
    assert.equal(
        bufferLatencyLabel(128, [{ rate: 48000 }]),
        "128 frames · 2.67 ms target/buffer · Lower CPU",
    );
    assert.equal(
        bufferLatencyLabel(32, [{ rate: 48000 }, { rate: 44100 }]),
        "32 frames · 0.67–0.73 ms target/buffer · Low latency",
    );
    assert.equal(bufferLatencyLabel(64, [{ rate: 0 }]), "64 frames · Balanced");
});
