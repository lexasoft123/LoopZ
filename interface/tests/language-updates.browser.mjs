import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";
const root = new URL("../../", import.meta.url).pathname;
const out = root + "artifacts/release-interface";
await mkdir(out, { recursive: true });
const fixture = engineFixture();
let updateState = {
    ok: true,
    state: "current",
    currentVersion: "0.1.0",
    version: "0.1.0",
    automatic: true,
};
const calls = [];
const browser = await chromium.launch({
    executablePath:
        process.env.LOOPZ_CHROME ||
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage({
        viewport: { width: 1440, height: 960 },
    });
    await page.exposeFunction("fixtureTransport", async (message) => {
        calls.push(message);
        if (message.operation === "state") return structuredClone(fixture);
        if (message.operation === "menu-context") return { ok: true };
        if (message.operation === "automatic-updates")
            updateState.automatic = message.enabled;
        if (message.operation === "check-updates")
            updateState.state = "available";
        if (
            [
                "updates",
                "check-updates",
                "automatic-updates",
                "open-update",
            ].includes(message.operation)
        )
            return structuredClone(updateState);
        throw Error("Unexpected mutation: " + message.operation);
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
    await page.getByRole("button", { name: "Settings", exact: true }).waitFor();
    assert((await page.locator("body").innerText()).includes("Channels 1–2"));
    assert((await page.locator("body").innerText()).includes("Choose Mix"));
    await page.screenshot({ path: out + "/workspace.png" });
    const choose = async (label) => {
        await page
            .getByRole("button", { name: /Language|Язык|语言/ })
            .first()
            .click();
        await page.getByRole("option", { name: new RegExp(label) }).click();
    };
    await choose("Русский");
    await page
        .getByRole("button", { name: "Настройки", exact: true })
        .waitFor();
    assert.equal(await page.locator("html").getAttribute("lang"), "ru");
    await page.reload();
    await page
        .getByRole("button", { name: "Настройки", exact: true })
        .waitFor();
    await choose("简体中文");
    await page.getByRole("button", { name: "设置", exact: true }).waitFor();
    assert.equal(await page.locator("html").getAttribute("lang"), "zh-CN");
    await page.screenshot({ path: out + "/chinese.png" });
    await choose("English");
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    await page
        .getByRole("button", { name: "Check for updates", exact: true })
        .click();
    await page
        .getByRole("button", { name: "Download update", exact: true })
        .click();
    const automatic = page.getByRole("switch", {
        name: "Automatically check for updates",
        exact: true,
    });
    await automatic.click();
    assert.equal(updateState.automatic, false);
    assert(calls.some((call) => call.operation === "open-update"));
    assert(
        !calls.some((call) =>
            ["edit", "initialize", "audio-setup"].includes(call.operation),
        ),
    );
    await page.screenshot({ path: out + "/settings.png" });
    await writeFile(
        out + "/result.json",
        JSON.stringify(
            {
                status: "passed",
                languages: ["en", "ru", "zh-CN"],
                persistence: true,
                updateControls: true,
                graphUntouched: true,
            },
            null,
            2,
        ),
    );
    console.log(
        "PASS: language switching, persistence and native update controls",
    );
} finally {
    await browser.close();
}
