import assert from "node:assert/strict";
import test from "node:test";
import { linuxTransport } from "../src/linux-transport.js";

test("Linux bridge preserves protocol objects and asynchronous replies", async () => {
    const request = { version: 1, operation: "state" };
    const response = {
        version: 1,
        snapshot: { revision: { epoch: "native", generation: "9" } },
    };
    const scope = {
        webkit: {
            messageHandlers: {
                loopz: {
                    async postMessage(value) {
                        assert.equal(value, request);
                        return response;
                    },
                },
            },
        },
    };
    assert.equal(await linuxTransport(request, scope), response);
});
test("Linux bridge reports an unavailable host", async () => {
    await assert.rejects(linuxTransport({}, {}), /desktop application/);
});
test("Linux bridge retains uncertain native failures as rejected requests", async () => {
    const error = new Error(
        "Audio service disconnected before acknowledgement",
    );
    const scope = {
        webkit: {
            messageHandlers: {
                loopz: {
                    postMessage() {
                        return Promise.reject(error);
                    },
                },
            },
        },
    };
    await assert.rejects(
        linuxTransport({ operation: "edit" }, scope),
        (actual) => actual === error,
    );
});
