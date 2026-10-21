import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";

const root = new URL("../../", import.meta.url).pathname;
const output = process.argv[2] || root + "artifacts/drag-refresh-browser";
await mkdir(output, { recursive: true });
const data = engineFixture();
const edits = [],
    errors = [];
let polls = 0;
const reducer = root + "build/editor-release/loopz-editor-reducer-fixture";
const browser = await chromium.launch({
    executablePath:
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage({
        viewport: { width: 1600, height: 1100 },
    });
    page.on("pageerror", (error) => errors.push(error.message));
    await page.exposeFunction("fixtureTransport", async (message) => {
        if (message.operation === "menu-context") return {};
        if (message.operation === "state") {
            data.inventory.refreshSequence = ++polls;
            return structuredClone(data);
        }
        assert.equal(message.operation, "edit");
        const document = await new Promise((resolve, reject) => {
            const child = execFile(
                reducer,
                [],
                { maxBuffer: 8e6 },
                (error, stdout, stderr) =>
                    error ? reject(Error(stderr)) : resolve(JSON.parse(stdout)),
            );
            child.stdin.end(
                JSON.stringify({
                    document: data.snapshot.document,
                    request: message.request,
                }),
            );
        });
        data.snapshot.document = document;
        data.snapshot.revision.generation = String(
            BigInt(data.snapshot.revision.generation) + 1n,
        );
        edits.push(message.request);
        return { ok: true };
    });
    await page.addInitScript(() => {
        window.webkit = {
            messageHandlers: {
                loopz: {
                    postMessage: (message) => window.fixtureTransport(message),
                },
            },
        };
    });
    await page.goto(pathToFileURL(root + "interface/dist/index.html").href);
    const drag = async (from, to) => {
        const a = await page.locator(`[data-socket="${from}"]`).boundingBox();
        const b = await page.locator(`[data-socket="${to}"]`).boundingBox();
        assert(a && b);
        await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2);
        await page.mouse.down();
        for (let step = 1; step <= 20; ++step) {
            await page.mouse.move(
                a.x + a.width / 2 + ((b.x - a.x) * step) / 20,
                a.y + a.height / 2 + ((b.y - a.y) * step) / 20,
            );
            await page.waitForTimeout(30);
        }
        await page.mouse.up();
        await page.waitForTimeout(200);
    };
    await page.locator('[data-socket="source:app:1"]').waitFor();
    const before = polls;
    await drag("source:app:1", "output-input:bus:2");
    await drag("output-send:bus:1", "monitor-input:monitor:1");
    const result = { pollsDuringGestures: polls - before, edits, errors };
    await writeFile(output + "/result.json", JSON.stringify(result, null, 2));
    assert(polls - before >= 4, "Both gestures must cross inventory refreshes");
    assert.deepEqual(errors, []);
    assert.equal(
        edits.length,
        2,
        "Source and monitor drags must each save one edit",
    );
    assert(
        data.snapshot.document.devices[0].routing.sources[0].connections.some(
            (wire) => wire.sourceChannel === 0 && wire.outputChannel === 1,
        ),
    );
    assert(
        data.snapshot.document.devices[0].monitors[0].connections.some(
            (wire) => wire.sourceChannel === 0 && wire.outputChannel === 0,
        ),
    );
    console.log(
        "PASS: source and monitor drags survive live inventory refreshes and save native edits",
    );
} finally {
    await browser.close();
}
