import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";

const root = new URL("../../", import.meta.url).pathname;
const out = root + "artifacts/device-card-switch-browser";
await mkdir(out, { recursive: true });
const data = engineFixture();
const other = structuredClone(data.snapshot.document.devices[0]);
other.id = "other";
other.name = "Other Mix";
data.snapshot.document.devices.push(other);
const original = JSON.parse(
    JSON.stringify(data.snapshot.document, (key, value) =>
        key === "gain" ? Math.fround(value) : value,
    ),
);
const edits = [];
let rejectNext = false;
const browser = await chromium.launch({
    executablePath:
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage({
        viewport: { width: 1360, height: 860 },
    });
    await page.exposeFunction("fixtureTransport", async (message) => {
        if (message.operation === "menu-context") return {};
        if (message.operation === "state") return structuredClone(data);
        assert.equal(message.operation, "edit");
        edits.push(message.request);
        if (rejectNext) {
            rejectNext = false;
            return {
                ok: false,
                code: "rejected",
                detail: "Synthetic rejection",
            };
        }
        data.snapshot.document = await new Promise((resolve, reject) => {
            const child = execFile(
                root + "build/editor-release/loopz-editor-reducer-fixture",
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
        data.snapshot.revision.generation = String(
            BigInt(data.snapshot.revision.generation) + 1n,
        );
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
    const toggle = (name) =>
        page.getByRole("switch", {
            name: name + " device enabled",
            exact: true,
        });
    const wait = async (name, checked) => {
        await page.waitForFunction(
            ({ name, checked }) => {
                const control = [
                    ...document.querySelectorAll('[role="switch"]'),
                ].find(
                    (item) =>
                        item.getAttribute("aria-label") ===
                        name + " device enabled",
                );
                return (
                    control &&
                    !control.disabled &&
                    control.getAttribute("aria-checked") === String(checked)
                );
            },
            { name, checked },
        );
    };
    await wait("Other Mix", true);
    await toggle("Other Mix").click();
    await wait("Other Mix", false);
    assert.equal(edits.at(-1).commands[0].device, "other");
    assert.equal(edits.at(-1).commands[0].controls.enabled, false);
    assert.equal(
        await page
            .getByRole("button", { name: "Select Mix", exact: true })
            .getAttribute("aria-current"),
        "true",
    );
    await toggle("Other Mix").focus();
    await page.keyboard.press("Space");
    await wait("Other Mix", true);
    await toggle("Mix").click();
    await wait("Mix", false);
    assert.equal(
        await page
            .getByRole("switch", { name: "Device enabled", exact: true })
            .getAttribute("aria-checked"),
        "false",
    );
    await page
        .getByRole("switch", { name: "Device enabled", exact: true })
        .click();
    await wait("Mix", true);
    assert.deepEqual(data.snapshot.document, original);
    rejectNext = true;
    await toggle("Other Mix").click();
    await page
        .getByText("Synthetic rejection", { exact: true })
        .first()
        .waitFor();
    await wait("Other Mix", true);
    assert.deepEqual(data.snapshot.document, original);
    data.connection.ready = false;
    await page.waitForFunction(() =>
        [...document.querySelectorAll(".lz-device-power button")].every(
            (button) => button.disabled,
        ),
    );
    const layout = await page.evaluate(() =>
        [...document.querySelectorAll(".lz-device-entry")].map((card) => {
            const button = card.querySelector(".lz-device-select");
            const toggle = card.querySelector('[role="switch"]');
            const title = button.querySelector("strong");
            const b = card.getBoundingClientRect(),
                t = toggle.getBoundingClientRect();
            return {
                nested: button.contains(toggle),
                fits:
                    t.left >= b.left &&
                    t.right <= b.right &&
                    t.top >= b.top &&
                    t.bottom <= b.bottom,
                title: title.textContent,
            };
        }),
    );
    assert(layout.every((card) => !card.nested && card.fits));
    await page.screenshot({ path: out + "/interface.png" });
    await writeFile(
        out + "/result.json",
        JSON.stringify(
            {
                status: "passed",
                edits,
                layout,
                checks: [
                    "Nonselected card targets its own native device",
                    "Card toggle preserves selection",
                    "Keyboard toggle works",
                    "Header and card stay synchronized",
                    "Wiring and controls survive off/on",
                    "Rejected edit is not displayed as saved",
                    "Disconnected controls disable",
                    "No nested buttons or card overflow",
                ],
            },
            null,
            2,
        ),
    );
    console.log(
        "PASS: whole-device card switches target native edits, preserve wiring and selection, and synchronize with the header",
    );
} finally {
    await browser.close();
}
