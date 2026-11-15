import test from "node:test";
import assert from "node:assert/strict";
import { build } from "esbuild";
import { installationCondition } from "../src/installation.js";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const temporary = await mkdtemp(join(tmpdir(), "loopz-audio-setup-"));
try {
    const output = join(temporary, "render.mjs");
    await build({
        stdin: {
            contents: `import React from 'react'; import {renderToStaticMarkup} from 'react-dom/server'; import {AudioSetup} from './src/AudioSetup.jsx'; export function render(state) { return renderToStaticMarkup(React.createElement(AudioSetup,{state})); }`,
            resolveDir: new URL("..", import.meta.url).pathname,
            loader: "jsx",
        },
        bundle: true,
        platform: "node",
        format: "esm",
        outfile: output,
        banner: {
            js: "import {createRequire} from 'node:module'; const require = createRequire(import.meta.url);",
        },
    });
    const { render } = await import(pathToFileURL(output));
    const state = (extra = {}, status = "installed") => ({
        installation: {
            checked: true,
            maintenanceAvailable: true,
            components: Object.fromEntries(
                ["engine", "driver", "service"].map((key) => [
                    key,
                    { state: status },
                ]),
            ),
            ...extra,
        },
    });
    test("AudioSetup renders each unavailable reason with a disabled maintenance action", () => {
        for (const [reason, text] of [
            ["wrong-location", "Move LoopZ to Applications"],
            ["missing-helper", "missing its setup helper"],
            ["missing-payload", "incomplete"],
            ["separate-driver", "Contact support"],
        ]) {
            const html = render(
                state({
                    maintenanceAvailable: false,
                    maintenanceUnavailableReason: reason,
                }),
            );
            assert.ok(html.includes(text), html);
            assert.match(html, /disabled=""/);
            if (reason.startsWith("missing"))
                assert.match(html, /Download the full LoopZ release/);
        }
    });
    test("AudioSetup labels install, update, repair and activation and keeps available actions enabled", () => {
        for (const [extra, status, label] of [
            [{}, "missing", "Install audio components"],
            [{ updateAvailable: true }, "installed", "Update audio components"],
            [
                {
                    components: {
                        engine: { state: "missing" },
                        driver: { state: "installed" },
                        service: { state: "installed" },
                    },
                },
                "installed",
                "Repair audio",
            ],
            [{ activationRequired: true }, "installed", "Finish audio setup"],
        ]) {
            const html = render(state(extra, status));
            assert.ok(html.includes(label), html);
            assert.doesNotMatch(html, /disabled=""/);
        }
        for (const [extra, status] of [
            [{}, "missing"],
            [{ updateAvailable: true }, "installed"],
            [{ activationRequired: true }, "installed"],
        ]) {
            assert.match(
                render(
                    state(
                        {
                            ...extra,
                            maintenanceAvailable: false,
                            maintenanceUnavailableReason: "missing-helper",
                        },
                        status,
                    ),
                ),
                /disabled=""/,
            );
        }
        assert.equal(render({ installation: { checked: false } }), "");
    });
    test("healthy separate routing layout shows maintenance guidance without an error banner", () => {
        const healthy = state({
            maintenanceAvailable: false,
            maintenanceUnavailableReason: "separate-driver",
            updateAvailable: false,
        });
        assert.equal(installationCondition(healthy), null);
        const html = render(healthy);
        assert.match(
            html,
            /separate routing driver layout cannot be maintained in-app/,
        );
        assert.match(html, /disabled=""/);
        assert.doesNotMatch(html, /must be removed/);
    });
    test("AudioSetup exposes retry for a waiting driver", () => {
        assert.match(
            render({
                ...state(),
                snapshot: { restoration: { phase: "waiting-for-driver" } },
            }),
            /Retry audio connection/,
        );
    });
} finally {
    await rm(temporary, { recursive: true, force: true });
}
