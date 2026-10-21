import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";

const fixture = engineFixture();
fixture.installation = {
    checked: true,
    maintenanceAvailable: true,
    components: Object.fromEntries(
        ["engine", "driver", "service"].map((name) => [
            name,
            { state: "missing" },
        ]),
    ),
};
let outcome = {
    ok: false,
    code: "approval-required",
    detail: "Allow the LoopZ helper.",
};
const calls = [];
const browser = await chromium.launch({
    executablePath:
        process.env.LOOPZ_CHROME ||
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage();
    page.setDefaultTimeout(15000);
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
        if (request.operation === "audio-setup")
            return structuredClone(outcome);
        if (
            [
                "menu-context",
                "retry-session",
                "audio-maintenance-settings",
            ].includes(request.operation)
        )
            return { ok: true };
        throw Error("Unexpected operation: " + request.operation);
    });
    await page.addInitScript(() => {
        window.webkit = {
            messageHandlers: {
                loopz: {
                    postMessage: (request) => window.fixtureTransport(request),
                },
            },
        };
    });
    const url = pathToFileURL(
        new URL("../dist/index.html", import.meta.url).pathname,
    ).href;
    const settings = async () => {
        await page.goto(url);
        await page
            .getByRole("button", { name: "Settings", exact: true })
            .click();
    };
    await settings();
    await page
        .getByRole("button", { name: "Install audio components", exact: true })
        .click();
    await page.getByText("Allow the LoopZ helper.", { exact: true }).waitFor();
    await page
        .getByRole("button", {
            name: "Open audio approval settings",
            exact: true,
        })
        .click();
    assert(
        calls.some(
            (request) => request.operation === "audio-maintenance-settings",
        ),
    );
    for (const component of Object.values(fixture.installation.components))
        component.state = "installed";
    fixture.installation.updateAvailable = true;
    outcome = { ok: true };
    await settings();
    await page
        .getByRole("button", { name: "Update audio components", exact: true })
        .click();
    await page
        .getByText(
            "Audio components verified. Reconnecting your saved mixes…",
            { exact: true },
        )
        .waitFor();
    assert(
        calls.some(
            (request) =>
                request.operation === "audio-setup" &&
                request.action === "update",
        ),
    );
    await page
        .getByRole("button", { name: "Repair audio", exact: true })
        .click();
    assert(
        calls.some(
            (request) =>
                request.operation === "audio-setup" &&
                request.action === "repair",
        ),
    );
    fixture.installation.updateAvailable = false;
    outcome = { ok: true, activationRequired: true };
    await settings();
    await page
        .getByRole("button", { name: "Repair audio", exact: true })
        .click();
    await page
        .getByText(
            "Audio components installed. Choose Finish audio setup to activate the driver.",
            { exact: true },
        )
        .waitFor();
    assert(
        !calls.some(
            (request) =>
                request.operation === "audio-setup" &&
                request.action === "activate",
        ),
    );
    outcome = { ok: true };
    await page
        .getByRole("button", { name: "Finish audio setup", exact: true })
        .click();
    await page
        .getByText(
            "Audio components verified. Reconnecting your saved mixes…",
            { exact: true },
        )
        .waitFor();
    assert(
        calls.some(
            (request) =>
                request.operation === "audio-setup" &&
                request.action === "activate",
        ),
    );
    assert(calls.some((request) => request.operation === "retry-session"));
    console.log(
        "PASS: native maintenance actions, approval, update, repair and explicit audio activation UI",
    );
} finally {
    await browser.close();
}
