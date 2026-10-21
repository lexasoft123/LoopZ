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
        await new Promise((resolve) => setTimeout(resolve, 800));
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

    await page.locator(".lz-wire-hit").first().waitFor();
    await page.locator(".lz-wire-hit").first().focus();
    await page.keyboard.press("Enter");
    await page.getByRole("button", { name: "Done", exact: true }).click();
    const focusAfterDone = await page.evaluate(() => ({
        tag: document.activeElement.tagName,
        label: document.activeElement.getAttribute("aria-label"),
    }));
    await page.locator(".lz-wire-hit").first().focus();
    await page.keyboard.press("Enter");
    await page
        .getByRole("button", { name: "Remove connection", exact: true })
        .click();
    await page
        .getByRole("button", { name: "Connections", exact: true })
        .click();
    await page.waitForTimeout(1400);
    assert.equal(focusAfterDone.tag, "BUTTON");
    assert.equal(focusAfterDone.label, "Player source channel 1");
    assert.deepEqual(errors, []);
    assert.equal(edits.length, 1);
    console.log(
        "PASS: Done restores socket focus; switching view during removal has no errors.",
    );
} finally {
    await browser.close();
}
