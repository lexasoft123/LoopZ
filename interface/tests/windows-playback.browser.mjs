import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";
const output = process.env.LOOPZ_UI_ARTIFACT_DIR;
if (!output) throw Error("Set a fresh LOOPZ_UI_ARTIFACT_DIR");
await mkdir(output, { recursive: false });
const data = engineFixture();
data.platform = "windows";
data.snapshot.runtime.editCapabilities = { appOutputRouting: true };
data.snapshot.document.devices[0].bindings[0].muteOriginal = false;
data.microphoneSetup = {
    enabled: true,
    authoritative: true,
    accepted: true,
    generation: data.snapshot.revision.generation,
    devices: { mix: { phase: "ready", removing: false } },
};
data.snapshot.runtime.devices[0].microphone = { connected: true };
const edits = [];
const errors = [];
const browser = await chromium.launch({
    executablePath:
        process.env.LOOPZ_CHROME ||
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage({
        viewport: { width: 1280, height: 900 },
    });
    page.setDefaultTimeout(15000);
    page.on("pageerror", (error) => errors.push(error.message));
    await page.exposeFunction("fixtureTransport", async (request) => {
        if (request.operation === "state") return structuredClone(data);
        if (request.operation === "menu-context") return { ok: true };
        if (request.operation === "updates")
            return {
                ok: true,
                state: "current",
                currentVersion: "0.2.0",
                automatic: false,
            };
        assert.equal(request.operation, "edit");
        assert.deepEqual(request.request.expected, data.snapshot.revision);
        for (const command of request.request.commands) {
            assert.equal(command.type, "setSourceControls");
            assert.equal(command.device, "mix");
            assert.equal(command.source, "app");
            data.snapshot.document.devices[0].bindings[0].muteOriginal =
                command.muteOriginal;
        }
        edits.push(structuredClone(request));
        data.snapshot.revision.generation = String(
            BigInt(data.snapshot.revision.generation) + 1n,
        );
        return { ok: true };
    });
    await page.addInitScript(() => {
        globalThis.webkit = {
            messageHandlers: {
                loopz: {
                    postMessage: (message) =>
                        globalThis.fixtureTransport(message),
                },
            },
        };
    });
    await page.goto(
        process.env.LOOPZ_INTERFACE_URL ||
            pathToFileURL(
                new URL("../dist/index.html", import.meta.url).pathname,
            ).href,
    );
    await page
        .getByRole("button", { name: "Player options", exact: true })
        .click();
    const toggle = page.getByRole("switch", {
        name: "Mute original app playback",
        exact: true,
    });
    assert.equal(await toggle.getAttribute("aria-checked"), "false");
    await toggle.click();
    await page.waitForFunction(
        () =>
            document
                .querySelector('[aria-label="Mute original app playback"]')
                ?.getAttribute("aria-checked") === "true",
    );
    assert.equal(edits[0].request.commands[0].muteOriginal, true);
    await page.screenshot({
        path: `${output}/source-options.png`,
        fullPage: false,
        animations: "disabled",
    });
    await page.setViewportSize({ width: 1100, height: 720 });
    assert.equal(await toggle.isVisible(), true);
    const dialogBounds = await page.getByRole("dialog").boundingBox();
    assert.ok(
        dialogBounds.y >= 0 && dialogBounds.y + dialogBounds.height <= 720,
        "Source dialog must fit inside the compact viewport",
    );
    await page.screenshot({
        path: `${output}/source-options-1100.png`,
        fullPage: false,
        animations: "disabled",
    });
    await page.keyboard.press("Escape");
    await page
        .getByText("Routing original playback…", { exact: true })
        .waitFor();
    await page.screenshot({
        path: `${output}/pending-routing-1100.png`,
        animations: "disabled",
    });
    data.snapshot.runtime.sources.find(
        (source) => source.kind === "application",
    ).playbackRouting = { requested: true, verified: true };
    await page.getByText("Original playback routed", { exact: true }).waitFor();
    await page.screenshot({
        path: `${output}/confirmed-routing.png`,
        fullPage: false,
        animations: "disabled",
    });
    data.snapshot.runtime.editCapabilities.appOutputRouting = false;
    await page.waitForTimeout(350);
    await page
        .getByRole("button", { name: "Player options", exact: true })
        .click();
    await page.getByRole("dialog").waitFor();
    assert.equal(
        await toggle.count(),
        0,
        "Legacy backend must not expose new control",
    );
    assert.deepEqual(errors, []);
    await writeFile(
        `${output}/report.json`,
        JSON.stringify(
            {
                ok: true,
                scope: "Browser fixture only; no Windows audio proof",
                checks: [
                    "ordinary playback edit command",
                    "pending versus verified labels",
                    "legacy backend capability gating",
                    "visible source dialog",
                ],
                edits,
                errors,
            },
            null,
            2,
        ),
    );
    console.log(
        "PASS: Windows playback controls, confirmation labels and legacy gating",
    );
} finally {
    await browser.close();
}
