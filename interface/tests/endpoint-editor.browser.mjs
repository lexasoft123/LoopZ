import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { engineFixture } from "./fixtures.mjs";

const [bundle, reducer, output] = process.argv
    .slice(2)
    .map((path) => resolve(path));
assert(
    bundle && reducer && output,
    "Provide interface bundle, native reducer fixture and evidence directory",
);
await mkdir(output, { recursive: true });
const data = engineFixture();
data.inventory.applications.push({
    bundle: "example.other",
    name: "Other Player",
});
Object.assign(data.inventory.devices[0], {
    name: "Old Interface",
    inputChannels: 8,
    defaultInput: true,
});
data.inventory.devices.push(
    {
        uid: "replacement-input",
        name: "Replacement Input",
        inputChannels: 4,
        outputChannels: 0,
    },
    {
        uid: "mono-output",
        name: "Mono Output",
        inputChannels: 0,
        outputChannels: 1,
    },
    {
        uid: "second-output",
        name: "Second Monitor",
        inputChannels: 0,
        outputChannels: 2,
    },
);
const mix = data.snapshot.document.devices[0];
mix.routing.sources.push(
    {
        id: "hardware",
        channels: 8,
        gain: 0.25,
        enabled: false,
        muted: true,
        connections: [
            { sourceChannel: 0, outputChannel: 0, gain: 0.35 },
            { sourceChannel: 7, outputChannel: 1, gain: 0.15 },
        ],
    },
    {
        id: "virtual",
        channels: 8,
        gain: 0.5,
        enabled: true,
        muted: false,
        connections: [
            { sourceChannel: 1, outputChannel: 0, gain: 0.6 },
            { sourceChannel: 7, outputChannel: 1, gain: 0.4 },
        ],
    },
);
mix.bindings.push(
    {
        id: "hardware",
        kind: "input-device",
        selector: "input",
        muteOriginal: false,
    },
    {
        id: "virtual",
        kind: "virtual-device",
        selector: "wide",
        muteOriginal: false,
    },
);
mix.monitors[0].muted = true;
mix.monitors[0].connections.push({
    sourceChannel: 0,
    outputChannel: 1,
    gain: 0.7,
});
mix.monitors.push({
    id: "monitor-two",
    deviceSelector: "second-output",
    channels: 2,
    gain: 0.25,
    enabled: false,
    muted: true,
    connections: [{ sourceChannel: 0, outputChannel: 1, gain: 0.5 }],
});
const empty = (id, name, channels) => ({
    id,
    name,
    sampleRate: 48000,
    formatRequestReceipt: "",
    routing: {
        outputChannels: channels,
        gain: 1,
        enabled: true,
        muted: false,
        safetyLimiter: true,
        sources: [],
    },
    bindings: [],
    monitors: [],
});
data.snapshot.document.devices.push(
    empty("wide", "Wide", 8),
    empty("stereo", "Stereo", 2),
);
const dependent = empty("dependent", "Dependent", 2);
dependent.routing.sources.push({
    id: "from-mix",
    channels: 2,
    gain: 1,
    enabled: true,
    muted: false,
    connections: [],
});
dependent.bindings.push({
    id: "from-mix",
    kind: "virtual-device",
    selector: "mix",
    muteOriginal: false,
});
data.snapshot.document.devices.push(dependent);
const original = structuredClone(mix);
const edits = [],
    errors = [],
    checks = [],
    screenshots = [];
const current = () =>
    data.snapshot.document.devices.find((item) => item.id === "mix");
const revision = () => {
    const next = (BigInt(data.snapshot.revision.generation) + 1n).toString();
    data.snapshot.revision.generation = next;
    data.snapshot.restoration.desiredGeneration =
        data.snapshot.restoration.activeGeneration = next;
};
const apply = (document, request) =>
    new Promise((success, failure) => {
        const child = execFile(
            reducer,
            [],
            { timeout: 10000, maxBuffer: 8 * 1024 * 1024 },
            (error, stdout, stderr) => {
                if (error) failure(new Error(stderr || error.message));
                else {
                    try {
                        success(JSON.parse(stdout));
                    } catch (error) {
                        failure(error);
                    }
                }
            },
        );
        child.stdin.on("error", failure);
        child.stdin.end(JSON.stringify({ document, request }));
    });
// Validate the initial fixture with the same native decoder/reducer before UI use.
data.snapshot.document = await apply(data.snapshot.document, {
    version: 1,
    expected: data.snapshot.revision,
    commands: [{ type: "renameDevice", device: "mix", name: "Mix" }],
});
const browser = await chromium.launch({
    executablePath:
        process.env.LOOPZ_CHROME ||
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    headless: true,
});
try {
    const page = await browser.newPage({
        viewport: { width: 1440, height: 900 },
        reducedMotion: "reduce",
    });
    page.on("pageerror", (error) => errors.push(error.message));
    page.on("console", (message) => {
        if (message.type() === "error") errors.push(message.text());
    });
    await page.exposeFunction("fixtureTransport", async (message) => {
        if (message.operation === "state") return structuredClone(data);
        if (message.operation === "choose-application") {
            const app = { bundle: "example.chosen", name: "Chosen Player" };
            data.inventory.applications.push(app);
            return app;
        }
        assert.equal(message.operation, "edit");
        assert.deepEqual(message.request.expected, data.snapshot.revision);
        data.snapshot.document = await apply(
            data.snapshot.document,
            message.request,
        );
        edits.push(structuredClone(message));
        revision();
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
    await page.goto(pathToFileURL(`${bundle}/index.html`).href);
    const close = async () => {
        await page.keyboard.press("Escape");
        await page.getByRole("dialog").waitFor({ state: "detached" });
    };
    const waitSelection = async (label, value) => {
        await page.waitForFunction(
            ({ label, value }) =>
                [...document.querySelectorAll("select")].some(
                    (item) =>
                        item.getAttribute("aria-label") === label &&
                        item.value === value,
                ),
            { label, value },
        );
        await page.waitForFunction(
            () => !document.querySelector('[role="dialog"] fieldset')?.disabled,
        );
    };
    const screenshot = async (name) => {
        await page.screenshot({ path: `${output}/${name}.png` });
        screenshots.push(`${name}.png`);
    };
    await page
        .getByRole("button", { name: "Player options", exact: true })
        .click();
    await page
        .getByLabel("Source application", { exact: true })
        .selectOption("example.other");
    await page
        .getByRole("button", { name: "Apply source selection", exact: true })
        .click();
    await page
        .getByRole("dialog", { name: "Other Player", exact: true })
        .waitFor();
    assert.equal(current().bindings[0].id, original.bindings[0].id);
    assert.equal(
        current().bindings[0].muteOriginal,
        original.bindings[0].muteOriginal,
    );
    assert.deepEqual(current().routing.sources[0], original.routing.sources[0]);
    await page
        .getByRole("button", {
            name: "Choose another application…",
            exact: true,
        })
        .click();
    await waitSelection("Source application", "example.chosen");
    await page
        .getByRole("button", { name: "Apply source selection", exact: true })
        .click();
    await page
        .getByRole("dialog", { name: "Chosen Player", exact: true })
        .waitFor();
    checks.push(
        "App replacement and chooser preserve source identity, fractional wiring, controls and original-mute policy through the native reducer",
    );
    await close();

    await page
        .getByRole("button", { name: "Old Interface options", exact: true })
        .click();
    await page
        .getByLabel("Source input device", { exact: true })
        .selectOption("replacement-input");
    await page
        .getByLabel("Source input channel count", { exact: true })
        .selectOption("2");
    const before = edits.length;
    await page
        .getByRole("button", { name: "Apply source selection", exact: true })
        .click();
    await page.getByText("Remove 1 connection?", { exact: true }).waitFor();
    assert.equal(edits.length, before);
    await screenshot("input-channel-warning");
    await page
        .getByRole("button", { name: "Keep current selection", exact: true })
        .click();
    assert.equal(
        await page
            .getByLabel("Source input device", { exact: true })
            .inputValue(),
        "input",
    );
    assert.equal(
        await page
            .getByLabel("Source input channel count", { exact: true })
            .inputValue(),
        "8",
    );
    assert.equal(edits.length, before);
    await page
        .getByLabel("Source input device", { exact: true })
        .selectOption("replacement-input");
    await page
        .getByLabel("Source input channel count", { exact: true })
        .selectOption("2");
    await page
        .getByRole("button", { name: "Apply source selection", exact: true })
        .click();
    await page.getByText("Remove 1 connection?", { exact: true }).waitFor();
    // Simulate another authenticated editor changing the saved layout while
    // confirmation is open; no production document or service is accessed.
    current()
        .routing.sources.find((item) => item.id === "hardware")
        .connections.push({ sourceChannel: 6, outputChannel: 0, gain: 0.2 });
    revision();
    await page
        .getByRole("button", { name: "Apply source selection", exact: true })
        .waitFor();
    await page
        .getByRole("button", { name: "Apply source selection", exact: true })
        .click();
    await page.getByText("Remove 2 connections?", { exact: true }).waitFor();
    await page
        .getByRole("button", {
            name: "Change and remove connections",
            exact: true,
        })
        .click();
    await page
        .getByRole("dialog", { name: "Replacement Input", exact: true })
        .waitFor();
    await waitSelection("Source input channel count", "2");
    const hardware = current().routing.sources.find(
        (item) => item.id === "hardware",
    );
    assert.equal(hardware.channels, 2);
    assert.equal(hardware.gain, 0.25);
    assert.equal(hardware.enabled, false);
    assert.equal(hardware.muted, true);
    assert.equal(hardware.connections.length, 1);
    assert(Math.abs(hardware.connections[0].gain - 0.35) < 0.000001);
    assert.deepEqual(
        edits.at(-1).request.commands.map((item) => item.type),
        ["setSourceSelector", "setInputChannels"],
    );
    checks.push(
        "Input replacement/channel change is atomic, warns before trimming and refreshes confirmation after concurrent layout edits",
    );
    await close();

    await page
        .getByRole("button", { name: "Headphones options", exact: true })
        .click();
    await page
        .getByLabel("Monitor output device", { exact: true })
        .selectOption("mono-output");
    await page
        .getByLabel("Monitor channel count", { exact: true })
        .selectOption("1");
    await page
        .getByRole("button", { name: "Apply monitor selection", exact: true })
        .click();
    await page.getByText("Remove 1 connection?", { exact: true }).waitFor();
    await page
        .getByRole("button", {
            name: "Change and remove connections",
            exact: true,
        })
        .click();
    await page
        .getByRole("dialog", { name: "Mono Output", exact: true })
        .waitFor();
    const monitor = current().monitors[0];
    assert.equal(monitor.id, original.monitors[0].id);
    assert.equal(monitor.deviceSelector, "mono-output");
    assert.equal(monitor.channels, 1);
    assert.equal(monitor.muted, true);
    assert.equal(monitor.gain, original.monitors[0].gain);
    assert.equal(monitor.connections.length, 1);
    assert(Math.abs(monitor.connections[0].gain - 0.2) < 0.000001);
    await page
        .getByRole("button", { name: "Edit monitor connections", exact: true })
        .click();
    await page
        .getByRole("checkbox", {
            name: "Output 1 to Mono Output channel 1",
            exact: true,
        })
        .waitFor();
    checks.push(
        "Monitor replacement preserves controls and surviving gains, and mapping labels state the actual signal direction",
    );
    await close();

    await page
        .getByRole("button", { name: "Wide options", exact: true })
        .click();
    const endpoints = page.getByLabel("Source virtual device", { exact: true });
    assert(
        await endpoints
            .locator('option[value="mix"]')
            .evaluate((item) => item.disabled),
    );
    assert(
        await endpoints
            .locator('option[value="dependent"]')
            .evaluate((item) => item.disabled),
    );
    await endpoints.selectOption("stereo");
    await page
        .getByRole("button", { name: "Apply source selection", exact: true })
        .click();
    await page
        .getByRole("button", {
            name: "Change and remove connections",
            exact: true,
        })
        .click();
    await page.getByRole("dialog", { name: "Stereo", exact: true }).waitFor();
    const virtual = current().routing.sources.find(
        (item) => item.id === "virtual",
    );
    assert.equal(virtual.channels, 2);
    assert.equal(virtual.connections.length, 1);
    assert(Math.abs(virtual.connections[0].gain - 0.6) < 0.000001);
    checks.push(
        "Virtual retarget follows the target channel count, trims only invalid wires and blocks direct/indirect feedback",
    );
    await close();

    await page
        .getByRole("button", { name: "Replacement Input options", exact: true })
        .click();
    data.connection.ready = false;
    await page.waitForFunction(
        () => document.querySelector('[role="dialog"] fieldset').disabled,
    );
    assert(
        await page
            .getByLabel("Source input device", { exact: true })
            .isDisabled(),
    );
    assert.equal(
        await page
            .getByLabel("Source input device", { exact: true })
            .inputValue(),
        "replacement-input",
    );
    data.connection.ready = true;
    await waitSelection("Source input device", "replacement-input");
    checks.push(
        "Disconnect disables endpoint edits and preserves the saved selection; fresh state restores editing",
    );
    await close();
    const beforeOrdering = structuredClone(data.snapshot.document);
    await page
        .getByRole("button", { name: "Device options", exact: true })
        .click();
    const deviceOrder = page.getByRole("group", {
        name: "Device order",
        exact: true,
    });
    assert(
        await deviceOrder
            .getByRole("button", { name: "Move Mix up", exact: true })
            .isDisabled(),
    );
    await deviceOrder
        .getByRole("button", { name: "Move Mix down", exact: true })
        .click();
    await deviceOrder.getByText("Position 2 of 4", { exact: true }).waitFor();
    await close();
    assert(
        await page
            .getByRole("button", { name: "Select Mix", exact: true })
            .evaluate((item) => item.getAttribute("aria-current") === "true"),
    );
    assert.deepEqual(
        data.snapshot.document.devices.map((item) => item.id),
        ["wide", "mix", "stereo", "dependent"],
    );
    await page.reload();
    await page.getByRole("heading", { name: "Mix", exact: true }).waitFor();
    assert(
        await page
            .getByRole("button", { name: "Select Mix", exact: true })
            .evaluate((item) => item.getAttribute("aria-current") === "true"),
    );
    checks.push(
        "Device ordering keeps the selected identity, sends a complete native permutation and persists selection across interface reload",
    );

    await page
        .getByRole("button", { name: "Chosen Player options", exact: true })
        .click();
    const sourceOrder = page.getByRole("group", {
        name: "Source order",
        exact: true,
    });
    await sourceOrder
        .getByRole("button", { name: "Move Chosen Player down", exact: true })
        .focus();
    await page.keyboard.press("Enter");
    await sourceOrder.getByText("Position 2 of 3", { exact: true }).waitFor();
    await page.waitForFunction(
        () =>
            document.activeElement?.getAttribute("aria-label") ===
            "Move Chosen Player down",
    );
    assert(
        await sourceOrder
            .getByRole("button", {
                name: "Move Chosen Player down",
                exact: true,
            })
            .evaluate((item) => item === document.activeElement),
    );
    assert.deepEqual(
        current().routing.sources.map((item) => item.id),
        ["hardware", "app", "virtual"],
    );
    assert.deepEqual(
        current().bindings.map((item) => item.id),
        ["hardware", "app", "virtual"],
    );
    await close();

    await page
        .getByRole("button", { name: "Mono Output options", exact: true })
        .click();
    const monitorOrder = page.getByRole("group", {
        name: "Monitor order",
        exact: true,
    });
    await monitorOrder
        .getByRole("button", { name: "Move Mono Output down", exact: true })
        .click();
    await monitorOrder.getByText("Position 2 of 2", { exact: true }).waitFor();
    await page.waitForFunction(
        () =>
            document.activeElement?.getAttribute("aria-label") ===
            "Move Mono Output up",
    );
    assert(
        await monitorOrder
            .getByRole("button", { name: "Move Mono Output down", exact: true })
            .isDisabled(),
    );
    data.connection.ready = false;
    await page.waitForFunction(() =>
        [
            ...document.querySelectorAll(
                '[role="group"][aria-label="Monitor order"] button',
            ),
        ].every((item) => item.disabled),
    );
    data.connection.ready = true;
    await page.waitForFunction(
        () =>
            !document.querySelector(
                '[role="group"][aria-label="Monitor order"] button',
            ).disabled,
    );
    const unchanged = (document) =>
        document.devices
            .map((item) => ({
                ...item,
                routing: {
                    ...item.routing,
                    sources: [...item.routing.sources].sort((a, b) =>
                        a.id.localeCompare(b.id),
                    ),
                },
                bindings: [...item.bindings].sort((a, b) =>
                    a.id.localeCompare(b.id),
                ),
                monitors: [...item.monitors].sort((a, b) =>
                    a.id.localeCompare(b.id),
                ),
            }))
            .sort((a, b) => a.id.localeCompare(b.id));
    assert.deepEqual(
        unchanged(data.snapshot.document),
        unchanged(beforeOrdering),
    );
    checks.push(
        "Keyboard source moves keep bindings aligned; monitor moves respect boundaries and disconnect; native reordering preserves all document settings and wires",
    );
    await screenshot("monitor-order");
    await close();
    await page
        .getByRole("button", { name: "Replacement Input options", exact: true })
        .click();
    for (const [width, height] of [
        [1440, 900],
        [768, 900],
    ]) {
        await page.setViewportSize({ width, height });
        for (const palette of ["night-studio", "atelier"]) {
            if (
                (await page.evaluate(
                    () => document.documentElement.dataset.szPalette,
                )) !== palette
            ) {
                await close();
                await page
                    .getByRole("button", { name: "Appearance", exact: true })
                    .click();
                await page.waitForFunction(
                    (palette) =>
                        document.documentElement.dataset.szPalette === palette,
                    palette,
                );
                await page
                    .getByRole("button", {
                        name: "Replacement Input options",
                        exact: true,
                    })
                    .click();
            }
            const metrics = await page
                .getByRole("dialog")
                .evaluate((dialog) => ({
                    width: dialog.getBoundingClientRect().width,
                    viewport: innerWidth,
                    overflow: dialog.scrollWidth > dialog.clientWidth + 1,
                }));
            assert(
                !metrics.overflow && metrics.width <= metrics.viewport,
                JSON.stringify(metrics),
            );
            await screenshot(`endpoint-${width}-${palette}`);
        }
    }
    for (let i = 0; i < 12; ++i) {
        await page.keyboard.press(i % 3 === 0 ? "Shift+Tab" : "Tab");
        assert(
            await page
                .getByRole("dialog")
                .evaluate((dialog) => dialog.contains(document.activeElement)),
        );
    }
    checks.push(
        "Actual Appearance control switches both SingZ palettes, layouts fit two viewports and keyboard focus stays in the modal then returns to its invoker",
    );
    await close();
    assert(
        await page
            .getByRole("button", {
                name: "Replacement Input options",
                exact: true,
            })
            .evaluate((item) => document.activeElement === item),
    );
    assert.deepEqual(errors, []);
    await writeFile(
        `${output}/result.json`,
        JSON.stringify(
            {
                status: "passed",
                nativeReducer: reducer,
                scope: "Production UI and native command decoder/reducer; transport, runtime telemetry and inventory are fixtures; no real routing or audio IO",
                checks,
                edits,
                screenshots,
                consoleErrors: errors,
            },
            null,
            2,
        ) + "\n",
    );
    console.log(
        JSON.stringify({
            status: "passed",
            checks: checks.length,
            edits: edits.length,
            screenshots: screenshots.length,
        }),
    );
} finally {
    await browser.close();
}
