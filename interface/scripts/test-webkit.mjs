import {
    beforeTourReload,
    afterTourReload,
} from "../tests/webkit-tour-scenarios.mjs";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
import { engineFixture } from "../tests/fixtures.mjs";
import { beforeReload, afterReload } from "../tests/webkit-scenarios.mjs";
import {
    beforeRecoveryReload,
    afterRecoveryReload,
} from "../tests/webkit-recovery-scenarios.mjs";
import {
    beforeUpstreamReload,
    afterUpstreamReload,
} from "../tests/webkit-upstream-recovery-scenarios.mjs";
import {
    beforeInstallationReload,
    afterInstallationReload,
} from "../tests/webkit-installation-scenarios.mjs";
import {
    beforeDocumentRecoveryReload,
    afterDocumentRecoveryReload,
} from "../tests/webkit-document-recovery-scenarios.mjs";
import {
    beforeKeyboardReload,
    afterKeyboardReload,
} from "../tests/webkit-keyboard-scenarios.mjs";
import {
    beforeSystemAppearanceReload,
    afterSystemAppearanceReload,
} from "../tests/webkit-system-appearance-scenarios.mjs";
import {
    beforeGainReload,
    afterGainReload,
} from "../tests/webkit-gain-rejection-scenarios.mjs";
import {
    beforeRateReload,
    afterRateReload,
} from "../tests/webkit-rate-scenarios.mjs";
import {
    beforePairReload,
    afterPairReload,
} from "../tests/webkit-output-pair-scenarios.mjs";
import {
    beforeBufferReload,
    afterBufferReload,
} from "../tests/webkit-buffer-scenarios.mjs";
import {
    beforeStatusReload,
    afterStatusReload,
} from "../tests/webkit-status-scenarios.mjs";

assert.equal(
    process.platform,
    "darwin",
    "This acceptance test requires macOS WebKit",
);
const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const output = resolve(process.argv[2] || `${root}/artifacts/webkit-interface`);
const keyboard = process.argv[3] === "keyboard";
const sanitized = process.argv[4] === "sanitized";
const profiles = new Map([
    [
        "status",
        {
            before: beforeStatusReload,
            after: afterStatusReload,
            label: "status-layout",
        },
    ],
    [
        "tour",
        {
            before: beforeTourReload,
            after: afterTourReload,
            label: "startup-tour",
        },
    ],
    [
        "document-recovery",
        {
            before: beforeDocumentRecoveryReload,
            after: afterDocumentRecoveryReload,
            label: "document-recovery",
        },
    ],
    [
        "installation",
        {
            before: beforeInstallationReload,
            after: afterInstallationReload,
            label: "installation",
        },
    ],
    [
        "upstream-recovery",
        {
            before: beforeUpstreamReload,
            after: afterUpstreamReload,
            label: "upstream-recovery",
        },
    ],
    [
        "buffers",
        {
            before: beforeBufferReload,
            after: afterBufferReload,
            label: "hardware-buffers",
        },
    ],
    [
        "output-pairs",
        {
            before: beforePairReload,
            after: afterPairReload,
            label: "output-pairs",
        },
    ],
    [
        "rates",
        {
            before: beforeRateReload,
            after: afterRateReload,
            label: "sample-rates",
        },
    ],
    [
        "appearance",
        { before: beforeReload, after: afterReload, label: "appearance" },
    ],
    [
        "recovery",
        {
            before: beforeRecoveryReload,
            after: afterRecoveryReload,
            label: "recovery",
        },
    ],
    [
        "keyboard",
        {
            before: beforeKeyboardReload,
            after: afterKeyboardReload,
            label: "native-keyboard",
        },
    ],
    [
        "system-appearance",
        {
            before: beforeSystemAppearanceReload,
            after: afterSystemAppearanceReload,
            label: "system-appearance",
        },
    ],
    [
        "gain",
        {
            before: beforeGainReload,
            after: afterGainReload,
            label: "gain-rejection",
        },
    ],
]);
const profile = profiles.get(process.argv[3] ?? "appearance");
assert(profile, "Unknown WebKit validation profile");
assert([undefined, "sanitized"].includes(process.argv[4]));
await mkdir(output, { recursive: true });

const run = (command, args, timeout = 60000, cwd = root) =>
    new Promise((success, failure) => {
        const child = spawn(command, args, {
            cwd,
            stdio: ["ignore", "pipe", "pipe"],
        });
        let stdout = "";
        let stderr = "";
        const timer = setTimeout(() => child.kill("SIGKILL"), timeout);
        child.stdout.on("data", (chunk) => (stdout += chunk));
        child.stderr.on("data", (chunk) => (stderr += chunk));
        child.on("error", (error) => {
            clearTimeout(timer);
            failure(error);
        });
        child.on("close", (code, signal) => {
            clearTimeout(timer);
            if (code !== 0)
                failure(
                    Object.assign(
                        new Error(`${command}: ${code ?? signal}\n${stderr}`),
                        {
                            stdout,
                            stderr,
                        },
                    ),
                );
            else success({ stdout, stderr });
        });
    });

await run(
    process.execPath,
    [`${root}/interface/scripts/build.mjs`, `${output}/interface`],
    60000,
    `${root}/interface`,
);
await run("/usr/bin/clang++", [
    "-std=c++20",
    "-fobjc-arc",
    ...(sanitized
        ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        : []),
    "-I",
    `${root}/native/include`,
    "-I",
    `${root}/native/platform/macos/include`,
    `${root}/tests/editor/webkit_fixture.mm`,
    `${root}/native/platform/macos/desktop/resources.mm`,
    "-framework",
    "Cocoa",
    "-framework",
    "WebKit",
    "-framework",
    "Security",
    "-o",
    `${output}/fixture`,
]);
const envelope = engineFixture();
const second = structuredClone(envelope.snapshot.document.devices[0]);
second.id = "second";
second.name = "Second Mix";
second.routing.sources = [];
second.bindings = [];
second.monitors = [];
envelope.snapshot.document.devices.push(second);
await writeFile(`${output}/envelope.json`, JSON.stringify(envelope));
await writeFile(`${output}/before.js`, profile.before);
await writeFile(`${output}/after.js`, profile.after);
const result = await run(
    `${output}/fixture`,
    [
        `${output}/interface`,
        `${output}/envelope.json`,
        `${output}/before.js`,
        `${output}/after.js`,
        `${output}/interface.png`,
    ],
    90000,
).catch(async (error) => {
    await writeFile(
        `${output}/failure.json`,
        JSON.stringify(
            {
                status: "failed",
                message: error.message,
                stdout: error.stdout || "",
                stderr: error.stderr || "",
            },
            null,
            2,
        ),
    );
    throw error;
});
await writeFile(`${output}/stderr.log`, result.stderr);
const report = JSON.parse(result.stdout);
assert.equal(report.engine, "WKWebView");
if (keyboard) {
    const events = [
        ...report.beforeReload.observedKeys,
        ...report.afterReload.observedKeys,
    ];
    assert(events.length > 0 && events.every((event) => event.trusted));
    assert.equal(report.nativeKeys, events.length);
}
report.validationProfile = profile.label;
report.instrumentation = sanitized ? "address,undefined" : "none";
assert(
    report.stateReads >= 2,
    "Each document must read from the native fixture bridge",
);
const build = JSON.parse(
    await readFile(`${output}/interface/build.json`, "utf8"),
);
assert.equal(build.package, "@singz/ui");
assert(
    build.modules.length > 0,
    "Actual SingZ modules must contribute to the bundle",
);
report.designSystem = { package: build.package, version: build.version };
report.observedAt = new Date().toISOString();
report.sourceSHA256 = {};
for (const path of [
    "tests/editor/webkit_fixture.mm",
    "native/platform/macos/desktop/resources.mm",
    "native/platform/macos/desktop/desktop.hpp",
    "native/platform/macos/desktop/main.mm",
    "interface/scripts/test-webkit.mjs",
    "interface/tests/webkit-scenarios.mjs",
    "interface/tests/webkit-recovery-scenarios.mjs",
    "interface/tests/webkit-upstream-recovery-scenarios.mjs",
    "interface/tests/webkit-installation-scenarios.mjs",
    "interface/tests/webkit-tour-scenarios.mjs",
    "interface/src/components/StartupTour.jsx",
    "interface/src/components/PassThroughHelp.jsx",
    "interface/src/tour.js",
    "interface/src/installation.js",
    "interface/tests/webkit-document-recovery-scenarios.mjs",
    "interface/src/document-recovery.js",
    "interface/tests/webkit-keyboard-scenarios.mjs",
    "interface/tests/webkit-system-appearance-scenarios.mjs",
    "interface/tests/webkit-gain-rejection-scenarios.mjs",
    "interface/tests/webkit-rate-scenarios.mjs",
    "interface/tests/webkit-output-pair-scenarios.mjs",
    "interface/tests/webkit-buffer-scenarios.mjs",
    "interface/src/HardwareBufferSettings.jsx",
    "interface/src/output-pairs.js",
    "interface/src/OutputPairRemoval.jsx",
    "interface/src/sample-rates.js",
    "interface/src/appearance.js",
    "interface/src/useAppearance.js",
    "interface/src/App.jsx",
    "interface/src/store.js",
    "interface/src/connection-state.js",
    "interface/src/model.js",
    "interface/src/Routing.jsx",
    "interface/src/Dialogs.jsx",
]) {
    report.sourceSHA256[path] = createHash("sha256")
        .update(await readFile(`${root}/${path}`))
        .digest("hex");
}
report.status = "passed";
report.interfaceSHA256 = {};
for (const name of ["index.html", "loopz.js", "loopz.css"]) {
    report.interfaceSHA256[name] = createHash("sha256")
        .update(await readFile(`${output}/interface/${name}`))
        .digest("hex");
}
await writeFile(
    `${output}/result.json`,
    `${JSON.stringify(report, null, 2)}\n`,
);
console.log(JSON.stringify(report, null, 2));
