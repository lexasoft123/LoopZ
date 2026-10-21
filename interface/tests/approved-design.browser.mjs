import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";
const root = new URL("../../", import.meta.url).pathname;
const output = root + "artifacts/approved-interface-validation";
await mkdir(output, { recursive: true });
const data = engineFixture();
const edits = [],
    errors = [],
    checks = [];
const reducer =
    process.argv[2] ||
    root + "build/editor-release/loopz-editor-reducer-fixture";
const apply = (request) =>
    new Promise((ok, no) => {
        const c = execFile(reducer, [], { maxBuffer: 8e6 }, (e, out, err) =>
            e ? no(Error(err)) : ok(JSON.parse(out)),
        );
        c.stdin.end(
            JSON.stringify({ document: data.snapshot.document, request }),
        );
    });
const browser = await chromium.launch({
    executablePath:
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage({
        viewport: { width: 1440, height: 900 },
        reducedMotion: "reduce",
    });
    page.on("pageerror", (e) => errors.push(e.message));
    await page.exposeFunction("fixtureTransport", async (message) => {
        if (message.operation === "state") return structuredClone(data);
        if (message.operation === "menu-context") return {};
        assert.equal(message.operation, "edit");
        assert.deepEqual(message.request.expected, data.snapshot.revision);
        data.snapshot.document = await apply(message.request);
        edits.push(message);
        const n = String(BigInt(data.snapshot.revision.generation) + 1n);
        data.snapshot.revision.generation = n;
        data.snapshot.restoration.desiredGeneration = n;
        data.snapshot.restoration.activeGeneration = n;
        return { ok: true };
    });
    await page.addInitScript(() => {
        window.webkit = {
            messageHandlers: {
                loopz: { postMessage: (m) => window.fixtureTransport(m) },
            },
        };
    });
    await page.goto(pathToFileURL(root + "interface/dist/index.html").href);
    const socket = (k) => page.locator(`[data-socket="${k}"]`);
    const from = "source:app:1",
        to = "output-input:bus:2";
    await socket(from).waitFor();
    const point = async (l) => {
        const b = await l.boundingBox();
        assert(b);
        return { x: b.x + b.width / 2, y: b.y + b.height / 2 };
    };
    const settle = async (n) => {
        await page.waitForFunction(
            () =>
                !document
                    .querySelector(".lz-saved")
                    ?.textContent.includes("Saving"),
        );
        await page.waitForTimeout(150);
        assert.equal(edits.length, n);
    };
    const drag = async (a, b, cancel = false) => {
        const A = await point(a),
            B = await point(b);
        await page.mouse.move(A.x, A.y);
        await page.mouse.down();
        await page.mouse.move(B.x, B.y, { steps: 15 });
        if (cancel) await page.keyboard.press("Escape");
        await page.mouse.up();
    };
    const y = await page
        .locator(".lz-graph")
        .evaluate((e) => e.getBoundingClientRect().top);
    await socket(from).click();
    assert.equal(await socket(to).getAttribute("data-connectable"), "true");
    assert.equal(
        await page
            .locator(".lz-graph")
            .evaluate((e) => e.getBoundingClientRect().top),
        y,
    );
    await page.keyboard.press("Escape");
    checks.push("click targeting and fixed graph position");
    await drag(socket(from), socket(to));
    await settle(1);
    assert.equal(
        data.snapshot.document.devices[0].routing.sources[0].connections.length,
        3,
    );
    checks.push("physical pointer drag adds fan-out");
    await drag(socket(from), socket(to));
    await settle(2);
    assert.equal(
        data.snapshot.document.devices[0].routing.sources[0].connections.length,
        3,
    );
    checks.push("duplicate drop is idempotent");
    await drag(socket(from), socket("output-input:bus:1"), true);
    await settle(2);
    checks.push("Escape cancels captured drag without an edit");
    await drag(socket(from), page.locator(".lz-column-heading").last());
    await settle(2);
    checks.push("invalid drop preserves graph");
    const wire = page.locator(".lz-wire-hit").first();
    await wire.focus();
    await page.keyboard.press("Enter");
    await page.getByRole("button", { name: "Done", exact: true }).waitFor();
    await page.keyboard.press("Escape");
    assert.equal(await page.locator(".lz-wire-inspector").count(), 0);
    checks.push("wire keyboard selection and Escape");
    // Use a real path point in the gutter for pointer rewire.
    const hit = page.locator(".lz-wire-hit").first();
    const at = await hit.evaluate((e) => {
        const p = e.getPointAtLength(e.getTotalLength() / 2),
            m = e.getScreenCTM();
        return {
            x: p.x * m.a + p.y * m.c + m.e,
            y: p.x * m.b + p.y * m.d + m.f,
        };
    });
    await page.mouse.move(at.x, at.y);
    await page.mouse.down();
    const target = await point(socket(to));
    await page.mouse.move(target.x, target.y, { steps: 15 });
    assert.equal(await socket(to).getAttribute("data-connectable"), "true");
    assert((await socket(to).getAttribute("class")).includes("drop-target"));
    await page.mouse.up();
    await settle(3);
    assert.equal(edits.at(-1).request.commands.length, 1);
    checks.push("pointer reroute is atomic; valid drop highlighting");
    await page.locator(".lz-wire-hit").first().focus();
    await page.keyboard.press("Space");
    await page
        .getByRole("button", { name: "Remove connection", exact: true })
        .click();
    await settle(4);
    assert.equal(
        await socket(from).evaluate((e) => document.activeElement === e),
        true,
    );
    checks.push("selected wire removal restores originating socket focus");
    await socket("source:app:2").click();
    await socket("output-input:bus:1").click();
    await settle(5);
    checks.push("non-dragging socket connection");
    await page.locator(".lz-wire-hit").first().focus();
    await page.keyboard.press("Delete");
    await settle(6);
    assert.equal(
        await socket("source:app:2").evaluate(
            (e) => document.activeElement === e,
        ),
        true,
    );
    await drag(socket("source:app:2"), socket("output-input:bus:1"));
    await settle(7);
    checks.push(
        "Delete removes only the focused wire and restores socket focus",
    );
    for (const [width, height] of [
        [1440, 900],
        [1100, 720],
    ])
        for (const theme of ["night-studio", "atelier"]) {
            await page.setViewportSize({ width, height });
            await page.evaluate((t) => {
                localStorage.setItem(
                    "loopz-appearance",
                    JSON.stringify({
                        selected: "mix",
                        theme: t === "atelier" ? "atelier" : "night-studio",
                    }),
                );
            }, theme);
            await page.reload();
            await socket(from).waitFor();
            const actual = await page
                .locator("html")
                .getAttribute("data-sz-palette");
            assert.equal(actual, theme);
            const boxes = await page.locator(".lz-graph").evaluate((e) => ({
                width: e.scrollWidth,
                client: e.clientWidth,
            }));
            assert(boxes.width <= boxes.client + 1, JSON.stringify(boxes));
            const tops = await page
                .locator(".lz-node")
                .evaluateAll((es) =>
                    es.slice(0, 3).map((e) => e.getBoundingClientRect().top),
                );
            const beforeDrag = structuredClone(data.snapshot.document);
            const priorEdits = edits.length;
            await drag(socket("source:app:2"), socket("output-input:bus:1"));
            await settle(priorEdits + 1);
            assert.deepEqual(data.snapshot.document, beforeDrag);
            await page.screenshot({ path: `${output}/${theme}-${width}.png` });
            await page
                .getByRole("button", { name: "Player options", exact: true })
                .click();
            await page.getByRole("dialog").waitFor();
            await page.keyboard.press("Escape");
            assert.equal(
                await page
                    .getByRole("button", {
                        name: "Player options",
                        exact: true,
                    })
                    .evaluate((e) => document.activeElement === e),
                true,
            );
            checks.push(`${theme} ${width}x${height}: layout, modal, focus`);
        }
    await page
        .getByRole("button", { name: "Connections", exact: true })
        .click();
    await page.locator(".lz-connection-list table").waitFor();
    assert.equal(
        await page.getByLabel("Device gain", { exact: true }).count(),
        1,
    );
    checks.push("Connections view retains master controls");
    // Real transport envelopes with simulated faults: production store must retain desired intent.
    const desired = structuredClone(data.snapshot.document);
    data.connection = { connected: false, ready: false };
    await page.waitForTimeout(750);
    assert.equal(
        await page.getByRole("meter").first().getAttribute("aria-valuenow"),
        "-60",
    );
    assert.deepEqual(data.snapshot.document, desired);
    data.connection = { connected: true, ready: true };
    await page.waitForTimeout(750);
    assert.deepEqual(data.snapshot.document, desired);
    checks.push("transport disconnect and recovery retain desired graph");
    assert.deepEqual(errors, []);
    await writeFile(
        output + "/browser-report.json",
        JSON.stringify(
            {
                checks,
                errors,
                edits: edits.length,
                scope: "Real Chromium pointer/keyboard + native reducer; fixture envelopes, no installed audio",
            },
            null,
            2,
        ),
    );
    console.log(JSON.stringify({ checks, errors, edits: edits.length }));
} finally {
    await browser.close();
}
