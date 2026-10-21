import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdir, mkdtemp, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
import {
    beforeRestart,
    afterRestart,
} from "../tests/webkit-engine-scenarios.mjs";

assert.equal(process.platform, "darwin");
const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const fixture = resolve(
    process.argv[2] || `${root}/build/editor-release/loopz-webkit-engine-tests`,
);
const output = resolve(process.argv[3] || `${root}/artifacts/webkit-engine`);
await mkdir(output, { recursive: true });
// Retain each run's document and failure report independently.
const run = await mkdtemp(`${output}/run-`);
const graph = `${run}/graph`;
await mkdir(graph);
await writeFile(`${run}/before.js`, beforeRestart);
await writeFile(`${run}/after.js`, afterRestart);
const result = await new Promise((success) => {
    const child = spawn(
        fixture,
        [
            `${root}/build/editor-release/interface`,
            graph,
            `${run}/before.js`,
            `${run}/after.js`,
        ],
        { stdio: ["ignore", "pipe", "pipe"] },
    );
    let stdout = "",
        stderr = "";
    const timer = setTimeout(() => child.kill("SIGKILL"), 120000);
    child.stdout.on("data", (chunk) => (stdout += chunk));
    child.stderr.on("data", (chunk) => (stderr += chunk));
    child.on("error", (error) => {
        clearTimeout(timer);
        success({ code: null, error: error.message, stdout, stderr });
    });
    child.on("close", (code, signal) => {
        clearTimeout(timer);
        success({ code, signal, stdout, stderr });
    });
});
await writeFile(`${run}/process.json`, JSON.stringify(result, null, 2));
assert.equal(result.code, 0, `${run}\n${result.stderr}`);
const report = JSON.parse(result.stdout);
assert.equal(report.status, "passed");
assert.notEqual(
    report.phases[0].revision.epoch,
    report.phases[1].revision.epoch,
);
assert.equal(report.phases[0].device, report.phases[1].device);
report.observedAt = new Date().toISOString();
report.run = run;
report.sourceSHA256 = {};
for (const path of [
    "native/platform/macos/desktop/desktop.hpp",
    "native/platform/macos/desktop/resources.mm",
    "native/platform/macos/desktop/bridge.mm",
    "native/platform/macos/desktop/installation_setup.hpp",
    "native/platform/macos/desktop/installation_setup.mm",
    "native/platform/macos/desktop/editing_actions.hpp",
    "native/platform/macos/desktop/editing_actions.mm",
    "native/platform/macos/desktop/microphone_setup.hpp",
    "native/platform/macos/desktop/microphone_setup.mm",
    "native/platform/macos/desktop/session_host.hpp",
    "native/platform/macos/desktop/session_process.cpp",
    "interface/src/Dialogs.jsx",
    "interface/src/App.jsx",
    "interface/src/Routing.jsx",
    "tests/editor/webkit_engine_tests.mm",
    "interface/tests/webkit-engine-scenarios.mjs",
    "interface/tests/webkit-gain-scenario.mjs",
    "interface/tests/webkit-output-pair-durable-scenario.mjs",
    "interface/src/output-pairs.js",
    "interface/src/OutputPairRemoval.jsx",
    "interface/src/model.js",
    "interface/scripts/test-webkit-engine.mjs",
]) {
    report.sourceSHA256[path] = createHash("sha256")
        .update(await readFile(`${root}/${path}`))
        .digest("hex");
}
report.fixtureSHA256 = createHash("sha256")
    .update(await readFile(fixture))
    .digest("hex");
report.interfaceSHA256 = {};
for (const name of ["index.html", "loopz.js", "loopz.css"]) {
    report.interfaceSHA256[name] = createHash("sha256")
        .update(
            await readFile(`${root}/build/editor-release/interface/${name}`),
        )
        .digest("hex");
}
await writeFile(`${run}/result.json`, JSON.stringify(report, null, 2));
await writeFile(`${output}/result.json`, JSON.stringify(report, null, 2));
console.log(JSON.stringify(report, null, 2));
