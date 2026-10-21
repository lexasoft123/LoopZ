import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import {
    mkdir,
    mkdtemp,
    readFile,
    writeFile,
    lstat,
    copyFile,
} from "node:fs/promises";
import { dirname, resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import { homedir } from "node:os";
import { engineFixture } from "../tests/fixtures.mjs";
import { persistenceScenario } from "../tests/webkit-persistence-scenarios.mjs";

assert.equal(process.platform, "darwin");
const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const output = resolve(
    process.argv[2] || `${root}/artifacts/webkit-persistence`,
);
assert.equal(process.argv.length, process.argv[2] ? 3 : 2);
await mkdir(output, { recursive: true });
const directory = await mkdtemp(join(output, "run-"));
const records = [];
const run = (command, args, timeout = 60000, cwd = root) =>
    new Promise((resolve, reject) => {
        const child = spawn(command, args, {
            cwd,
            stdio: ["ignore", "pipe", "pipe"],
        });
        let stdout = "",
            stderr = "",
            timedOut = false;
        let forced;
        const timer = setTimeout(() => {
            timedOut = true;
            child.kill("SIGTERM");
            forced = setTimeout(() => child.kill("SIGKILL"), 3000);
        }, timeout);
        child.stdout.on("data", (chunk) => {
            stdout += chunk;
        });
        child.stderr.on("data", (chunk) => {
            stderr += chunk;
        });
        child.on("error", (error) => {
            clearTimeout(timer);
            clearTimeout(forced);
            reject(error);
        });
        child.on("close", (code, signal) => {
            clearTimeout(timer);
            clearTimeout(forced);
            const result = {
                command,
                args,
                pid: child.pid,
                code,
                signal,
                timedOut,
                stdout,
                stderr,
            };
            records.push(result);
            if (code !== 0 || timedOut)
                reject(
                    Object.assign(
                        new Error(`Private fixture command failed: ${command}`),
                        { result },
                    ),
                );
            else resolve(result);
        });
    });
const bundles = [];
const cleanup = [];
const phases = [];
const updates = [];
const hash = async (path) =>
    createHash("sha256")
        .update(await readFile(path))
        .digest("hex");
let failure;
try {
    await run(
        process.execPath,
        [`${root}/interface/scripts/build.mjs`, `${directory}/interface`],
        60000,
        `${root}/interface`,
    );
    await run("/usr/bin/clang++", [
        "-std=c++20",
        "-fobjc-arc",
        "-I",
        `${root}/native/include`,
        "-I",
        `${root}/native/platform/macos/include`,
        `${root}/tests/editor/webkit_persistence_fixture.mm`,
        `${root}/native/platform/macos/desktop/resources.mm`,
        "-framework",
        "Cocoa",
        "-framework",
        "WebKit",
        "-framework",
        "Security",
        "-o",
        `${directory}/fixture`,
    ]);
    const envelope = engineFixture();
    const second = structuredClone(envelope.snapshot.document.devices[0]);
    Object.assign(second, {
        id: "second",
        name: "Second Mix",
        bindings: [],
        monitors: [],
    });
    second.routing.sources = [];
    envelope.snapshot.document.devices.push(second);
    await writeFile(`${directory}/envelope.json`, JSON.stringify(envelope));
    for (const name of ["main", "isolated"]) {
        const identifier = `com.lexasoft.loopz.fixture.preferences.${randomBytes(16).toString("hex")}`;
        const runtimePaths = [
            join(homedir(), "Library/WebKit", identifier),
            join(homedir(), "Library/Caches", identifier),
            join(homedir(), "Library/Preferences", identifier + ".plist"),
        ];
        for (const path of runtimePaths) {
            try {
                await lstat(path);
                assert.fail("Private application namespace already exists");
            } catch (error) {
                if (error.code !== "ENOENT") throw error;
            }
        }
        const app = `${directory}/${name}/LoopZ Preference Fixture.app`;
        const executable = `${app}/Contents/MacOS/LoopZ Preference Fixture`;
        await mkdir(dirname(executable), { recursive: true });
        await copyFile(`${directory}/fixture`, executable);
        await writeFile(
            `${app}/Contents/Info.plist`,
            `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>${identifier}</string>
<key>CFBundleExecutable</key><string>LoopZ Preference Fixture</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string><key>LSMinimumSystemVersion</key><string>14.2</string>
<key>NSPrincipalClass</key><string>NSApplication</string></dict></plist>\n`,
        );
        await run("/usr/bin/codesign", [
            "--force",
            "--sign",
            "-",
            "--options",
            "runtime",
            "--timestamp=none",
            app,
        ]);
        await run("/usr/bin/codesign", ["--verify", "--strict", app]);
        bundles.push({
            name,
            identifier,
            app,
            executable,
            runtimePaths,
            version: "1",
        });
    }
    const plan = [
        {
            name: "fresh-dark-save-night",
            fresh: true,
            expectedTheme: "system",
            native: "dark",
            writeTheme: "night-studio",
        },
        {
            name: "restart-light-save-atelier",
            expectedTheme: "night-studio",
            native: "light",
            writeTheme: "atelier",
        },
        {
            name: "restart-dark-save-system",
            expectedTheme: "atelier",
            native: "dark",
            writeTheme: "system",
        },
        {
            name: "restart-light-read-system",
            update: true,
            expectedTheme: "system",
            native: "light",
        },
        {
            name: "restart-dark-read-system",
            expectedTheme: "system",
            native: "dark",
        },
        {
            name: "separate-identity-is-empty",
            fresh: true,
            expectedTheme: "system",
            native: "light",
            isolated: true,
        },
        {
            name: "original-identity-still-retains-settings",
            expectedTheme: "system",
            native: "dark",
        },
    ];
    for (const phase of plan) {
        const bundle = bundles[phase.isolated ? 1 : 0];
        if (phase.update) {
            const info = `${bundle.app}/Contents/Info.plist`;
            const original = await readFile(info, "utf8");
            const replacement = original.replace(
                "<key>CFBundleVersion</key><string>1</string>",
                "<key>CFBundleVersion</key><string>2</string>",
            );
            assert.notEqual(replacement, original);
            const before = await hash(bundle.executable);
            await writeFile(info, replacement);
            await run("/usr/bin/codesign", [
                "--force",
                "--sign",
                "-",
                "--options",
                "runtime",
                "--timestamp=none",
                bundle.app,
            ]);
            await run("/usr/bin/codesign", [
                "--verify",
                "--strict",
                bundle.app,
            ]);
            const after = await hash(bundle.executable);
            assert.notEqual(
                before,
                after,
                "The updated app must have a new signed executable image",
            );
            updates.push({
                bundleIdentifier: bundle.identifier,
                versionBefore: "1",
                versionAfter: "2",
                executableBeforeSHA256: before,
                executableAfterSHA256: after,
            });
            bundle.version = "2";
        }
        const script = `${directory}/${phase.name}.js`;
        await writeFile(script, persistenceScenario(phase));
        const process = await run(bundle.executable, [
            `${directory}/interface`,
            `${directory}/envelope.json`,
            script,
            phase.native,
        ]);
        const report = JSON.parse(process.stdout);
        assert.equal(report.status, "passed");
        assert.equal(report.bundleIdentifier, bundle.identifier);
        assert.equal(report.bundleVersion, bundle.version);
        assert.equal(report.pid, process.pid);
        assert(
            report.persistent &&
                report.defaultDataStore &&
                report.rejectedCommands === 0,
        );
        phases.push({ ...phase, report });
        await writeFile(
            `${directory}/${phase.name}.json`,
            JSON.stringify(report, null, 2),
        );
    }
    assert.equal(
        new Set(phases.map((phase) => phase.report.pid)).size,
        plan.length,
    );
} catch (error) {
    failure = {
        message: error.message,
        result: error.result,
        stack: error.stack,
    };
} finally {
    // Public WebKit API clears only these freshly generated application IDs.
    for (const bundle of bundles) {
        try {
            const result = await run(bundle.executable, [
                "--clear-private-store",
            ]);
            const report = JSON.parse(result.stdout);
            assert.equal(report.bundleIdentifier, bundle.identifier);
            assert.equal(report.recordsRemaining, 0);
            cleanup.push(report);
        } catch (error) {
            cleanup.push({
                status: "failed",
                bundleIdentifier: bundle.identifier,
                message: error.message,
            });
            failure ||= {
                message: "Private WebKit data cleanup remains incomplete",
            };
        }
    }
}
const sources = {};
for (const name of [
    "tests/editor/webkit_persistence_fixture.mm",
    "native/platform/macos/desktop/resources.mm",
    "interface/scripts/test-webkit-persistence.mjs",
    "interface/tests/webkit-persistence-scenarios.mjs",
    "interface/src/appearance.js",
    "interface/src/useAppearance.js",
    "interface/src/main.jsx",
    "interface/src/store.js",
    "interface/src/App.jsx",
    "interface/src/Dialogs.jsx",
])
    sources[name] = await hash(`${root}/${name}`);
const report = {
    status: failure ? "failed" : "passed",
    observedAt: new Date().toISOString(),
    directory,
    bundles,
    phases,
    updates,
    cleanup,
    failure,
    sourceSHA256: sources,
    limits: [
        "Private ad-hoc-signed bundle identities and synthetic bridge; no canonical production Engine, permissions or audio",
        "The production default persistent WebKit API and resource handler are exercised across complete process exits",
        "Public WebKit cleanup clears private data records; per-identity cache metadata directories may remain",
        "This does not verify the installed canonical desktop, reboot/logout, or power-loss durability",
    ],
};
await writeFile(
    `${directory}/processes.json`,
    JSON.stringify(records, null, 2),
);
await writeFile(`${directory}/result.json`, JSON.stringify(report, null, 2));
await writeFile(`${output}/result.json`, JSON.stringify(report, null, 2));
console.log(JSON.stringify(report, null, 2));
if (failure) process.exitCode = 1;
