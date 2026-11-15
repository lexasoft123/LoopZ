import { mkdir } from "node:fs/promises";
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
let registrationStatus = 2;
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
        if (request.operation === "audio-setup" && request.action === "status")
            return { ok: true, registrationStatus };
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
    await mkdir("../artifacts/audio-maintenance", { recursive: true });
    await page.screenshot({
        path: "../artifacts/audio-maintenance/approval-required.png",
    });
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
    // Approving the helper in Login Items continues the install by itself.
    const installs = () =>
        calls.filter(
            (request) =>
                request.operation === "audio-setup" &&
                request.action === "install",
        ).length;
    assert.equal(installs(), 1);
    outcome = { ok: true };
    registrationStatus = 1;
    for (let i = 0; i < 40 && installs() < 2; i++)
        await new Promise((resolve) => setTimeout(resolve, 250));
    assert.equal(installs(), 2, "install did not continue after approval");
    for (const component of Object.values(fixture.installation.components))
        component.state = "installed";
    fixture.installation.updateAvailable = true;
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
    fixture.installation.maintenanceAvailable = false;
    fixture.installation.maintenanceUnavailableReason = "missing-payload";
    fixture.installation.components.engine = { state: "missing" };
    await settings();
    await page
        .getByText(
            "This copy of LoopZ is incomplete. Download the full LoopZ release and replace this copy.",
            { exact: true },
        )
        .waitFor();
    assert(
        await page
            .getByRole("button", { name: "Repair audio", exact: true })
            .isDisabled(),
    );
    await mkdir("../artifacts/audio-maintenance", { recursive: true });
    await page.waitForTimeout(350);
    await page.screenshot({
        path: "../artifacts/audio-maintenance/missing-payload.png",
        animations: "disabled",
    });
    fixture.installation.maintenanceAvailable = true;
    fixture.installation.components.engine = { state: "installed" };
    fixture.snapshot.restoration = { phase: "waiting-for-driver" };
    await settings();
    const beforeRetry = calls.filter(
        (request) => request.operation === "retry-session",
    ).length;
    await page
        .getByRole("button", { name: "Retry audio connection", exact: true })
        .click();
    assert(
        calls.filter((request) => request.operation === "retry-session")
            .length > beforeRetry,
    );
    fixture.installation.maintenanceAvailable = false;
    fixture.installation.maintenanceUnavailableReason = "separate-driver";
    fixture.snapshot.restoration = { phase: "restored" };
    await settings();
    await page
        .getByText(
            "This separate routing driver layout cannot be maintained in-app. Contact support before changing audio components.",
            { exact: true },
        )
        .waitFor();
    assert.equal(
        await page
            .getByText("LoopZ setup needs attention", { exact: true })
            .count(),
        0,
    );
    assert(
        await page
            .getByRole("button", { name: "Repair audio", exact: true })
            .isDisabled(),
    );
    console.log(
        "PASS: native maintenance actions, approval, update, repair and explicit audio activation UI",
    );
} finally {
    await browser.close();
}
