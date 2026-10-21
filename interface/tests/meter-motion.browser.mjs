import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";
const root = new URL("../../", import.meta.url).pathname;
const data = engineFixture();
let polls = 0;
const browser = await chromium.launch({
    executablePath:
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage({
        viewport: { width: 1600, height: 1100 },
    });
    await page.exposeFunction("fixtureTransport", async (message) => {
        if (message.operation === "menu-context") return {};
        assert.equal(message.operation, "state");
        const value = structuredClone(data);
        value.snapshot.runtime.devices[0].sources[0].meter.peak[0] =
            ++polls % 2 ? 0.1 : 0.8;
        return value;
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
    await page
        .locator('[aria-label="Player channel 1 pre-gain meter"]')
        .waitFor();
    const result = await page.evaluate(async () => {
        const meter = document.querySelector(
            '[aria-label="Player channel 1 pre-gain meter"]',
        );
        const fill = meter.querySelector(".lz-level-fill");
        const samples = [];
        const start = performance.now();
        while (performance.now() - start < 1400) {
            await new Promise(requestAnimationFrame);
            samples.push({
                transform: getComputedStyle(fill).transform,
                width: getComputedStyle(fill).width,
            });
        }
        return {
            animationFrames: samples.length,
            distinctTransforms: new Set(samples.map((s) => s.transform)).size,
            layoutWidths: [...new Set(samples.map((s) => s.width))],
        };
    });
    result.statePolls = polls;
    await writeFile(
        root + "artifacts/smooth-meter-motion-proof.json",
        JSON.stringify(result, null, 2),
    );
    assert(result.animationFrames >= 40);
    assert(
        result.distinctTransforms > result.statePolls,
        "Meters must interpolate between incoming readings",
    );
    assert.equal(
        result.layoutWidths.length,
        1,
        "Smoothing must not animate layout width",
    );
    console.log(JSON.stringify(result));
} finally {
    await browser.close();
}
