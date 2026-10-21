import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { pathToFileURL } from "node:url";
import { mkdir } from "node:fs/promises";
import { engineFixture } from "./fixtures.mjs";

const fixture = engineFixture();
const device = fixture.snapshot.document.devices[0];
device.routing.sources.push({
    id: "pass",
    channels: 2,
    gain: 1,
    enabled: true,
    muted: false,
    connections: [],
});
device.bindings.push({
    id: "pass",
    kind: "pass-through",
    selector: "",
    muteOriginal: false,
});
fixture.snapshot.runtime.devices[0].sources.push({
    id: "pass",
    online: false,
    meter: { valid: true, peak: [0, 0] },
});
const calls = [];
const browser = await chromium.launch({
    executablePath:
        process.env.LOOPZ_CHROME ||
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
const url = pathToFileURL(
    new URL("../dist/index.html", import.meta.url).pathname,
).href;
const output = new URL("../../artifacts/startup-tour/", import.meta.url);
await mkdir(output, { recursive: true });
try {
    const page = await browser.newPage({
        viewport: { width: 1440, height: 1000 },
    });
    page.setDefaultTimeout(10000);
    await page.exposeFunction("fixtureTransport", async (request) => {
        calls.push(request);
        if (request.operation === "state") return structuredClone(fixture);
        if (request.operation === "updates")
            return {
                ok: true,
                state: "current",
                currentVersion: "0.2.0",
                automatic: true,
            };
        if (request.operation === "menu-context") return { ok: true };
        throw Error(
            "Tour unexpectedly requested a native operation: " +
                request.operation,
        );
    });
    await page.addInitScript(() => {
        localStorage.setItem("loopz-language", "en");
        localStorage.setItem(
            "loopz-appearance",
            JSON.stringify({ theme: "night-studio" }),
        );
        window.webkit = {
            messageHandlers: {
                loopz: {
                    postMessage: (request) => window.fixtureTransport(request),
                },
            },
        };
    });
    await page.goto(url);
    await page
        .getByRole("button", { name: "Take a tour", exact: true })
        .click();
    const tour = page.locator("dialog.lz-tour");
    await tour
        .getByRole("heading", { name: "Your sound, in one place" })
        .waitFor();
    await page.screenshot({ path: new URL("welcome.png", output).pathname });
    await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour
        .getByRole("heading", { name: "Create a virtual device" })
        .waitFor();
    await tour.locator("svg > rect[stroke]").waitFor();
    await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour
        .getByRole("heading", { name: "1. Choose what goes in" })
        .waitFor();
    await page.screenshot({ path: new URL("sources.png", output).pathname });
    await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour
        .getByRole("heading", { name: "When to use Pass-through" })
        .waitFor();
    await tour.getByRole("button", { name: "Back", exact: true }).click();
    await tour
        .getByRole("heading", { name: "1. Choose what goes in" })
        .waitFor();
    await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour
        .getByRole("heading", { name: "2. Connect your channels" })
        .waitFor();
    await page.screenshot({
        path: new URL("connections.png", output).pathname,
    });
    await page.setViewportSize({ width: 760, height: 580 });
    await page.waitForFunction(() => {
        const rect = document
            .querySelector(".lz-tour-card")
            .getBoundingClientRect();
        return (
            rect.left >= 0 &&
            rect.top >= 0 &&
            rect.right <= innerWidth &&
            rect.bottom <= innerHeight
        );
    });
    await page.screenshot({ path: new URL("compact.png", output).pathname });
    for (let i = 0; i < 8; ++i) {
        await page.keyboard.press("Tab");
        assert(
            await page.evaluate(
                () => document.activeElement.closest("dialog.lz-tour") !== null,
            ),
        );
    }
    await page.keyboard.press("Escape");
    await tour.waitFor({ state: "detached" });
    await page.reload();
    await page.getByRole("button", { name: "Settings", exact: true }).waitFor();
    assert.equal(
        await page
            .getByRole("button", { name: "Take a tour", exact: true })
            .count(),
        0,
    );
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page
        .getByRole("button", { name: "Connections", exact: true })
        .click();
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    await page
        .getByRole("button", { name: "Take a tour", exact: true })
        .click();
    await tour
        .getByRole("heading", { name: "Your sound, in one place" })
        .waitFor();
    assert.equal(await page.locator('[data-od-id="sources"]').count(), 1);
    for (let i = 0; i < 7; ++i)
        await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour.getByRole("heading", { name: "Help is always here" }).waitFor();
    await tour.getByRole("button", { name: "Got it", exact: true }).click();
    await tour.waitFor({ state: "detached" });
    // Opening contextual help must not alter the graph, even while meters poll.
    await page.getByText("What is Pass-through?", { exact: true }).click();
    await page
        .getByText(
            "Only using a microphone or instrument? You don’t need Pass-through and can remove it.",
            { exact: true },
        )
        .waitFor();
    await page.screenshot({
        path: new URL("pass-through-help.png", output).pathname,
    });
    fixture.snapshot.document.devices = [];
    fixture.snapshot.runtime.devices = [];
    fixture.snapshot.canCreateDocument = true;
    await page.reload();
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    await page
        .getByRole("button", { name: "Take a tour", exact: true })
        .click();
    await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour.getByRole("button", { name: "Next", exact: true }).click();
    await tour.getByRole("heading", { name: "Help is always here" }).waitFor();
    await tour.getByRole("button", { name: "Got it", exact: true }).click();
    assert(
        calls.every((request) =>
            ["state", "updates", "menu-context"].includes(request.operation),
        ),
    );
    console.log(
        "PASS: whole-app tour, highlighted controls, resize, keyboard, persistence, replay, empty workspace and read-only Pass-through help",
    );
} finally {
    await browser.close();
}
