import assert from "node:assert/strict";
import { chromium } from "playwright-core";
import { writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { createInterface } from "node:readline";
import { createServer } from "node:http";

const [profile, targetName] = process.argv.slice(2);
assert(profile && targetName?.startsWith("LoopZ Chrome Review "));
const work = resolve(profile);
const executable =
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const worklet = `
class LoopZSine extends AudioWorkletProcessor {
    constructor() { super(); this.active = false; this.port.onmessage = e => this.active = e.data === true; }
    process(_inputs, outputs) {
        for (let channel = 0; channel < outputs[0].length; channel++) {
            const output = outputs[0][channel];
            for (let i = 0; i < output.length; i++)
                output[i] = this.active ? 0.1 * Math.sin(2 * Math.PI * (channel ? 2200 : 1100) * (currentFrame + i) / sampleRate) : 0;
        }
        return true;
    }
}
registerProcessor("loopz-sine", LoopZSine);
`;
const emit = (event, data) => console.log(JSON.stringify({ event, ...data }));
let context;
let server;
let browser;
let web;
let closing;
const close = () =>
    (closing ||= (async () => {
        const killer = setTimeout(() => server?.kill().catch(() => {}), 5000);
        try {
            await context?.close().catch(() => {});
            await browser?.close().catch(() => {});
            await server?.close();
        } finally {
            clearTimeout(killer);
        }
    })());
for (const signal of ["SIGTERM", "SIGINT"])
    process.on(signal, () => close().finally(() => process.exit(1)));
const watchdog = setTimeout(
    () => close().finally(() => process.exit(1)),
    180000,
);
try {
    server = await chromium.launchServer({
        host: "127.0.0.1",
        executablePath: executable,
        headless: true,
        ignoreDefaultArgs: ["--mute-audio"],
        args: [
            "--autoplay-policy=no-user-gesture-required",
            "--disable-background-networking",
            "--disable-component-update",
            "--disable-sync",
        ],
    });
    browser = await chromium.connect(server.wsEndpoint());
    context = await browser.newContext({ permissions: ["microphone"] });
    const cdp = await browser.newBrowserCDPSession();
    const commandLine = (await cdp.send("Browser.getBrowserCommandLine"))
        .arguments;
    const profilePath = commandLine
        .find((value) => value.startsWith("--user-data-dir="))
        ?.slice(16);
    assert(profilePath && /playwright_chromiumdev_profile-/.test(profilePath));
    assert(!commandLine.includes("--mute-audio"));
    const html = resolve(work, "chrome-audio.html");
    const document =
        '<!doctype html><meta charset="utf-8"><title>LoopZ private Chrome audio fixture</title>';
    await writeFile(html, document);
    await writeFile(resolve(work, "signal.js"), worklet);
    web = createServer((request, response) => {
        if (
            request.method !== "GET" ||
            !["/", "/signal.js"].includes(request.url)
        ) {
            response.writeHead(404).end();
            return;
        }
        response.setHeader(
            "Content-Type",
            request.url === "/"
                ? "text/html; charset=utf-8"
                : "text/javascript",
        );
        response.setHeader(
            "Content-Security-Policy",
            "default-src 'none'; script-src 'self'; worker-src 'self'; connect-src 'none'; frame-src 'none'",
        );
        response.end(request.url === "/" ? document : worklet);
    });
    await new Promise((resolve) => web.listen(0, "127.0.0.1", resolve));
    const page = context.pages()[0] || (await context.newPage());
    page.setDefaultTimeout(15000);
    await page.goto(`http://127.0.0.1:${web.address().port}/`);
    const prepared = await page.evaluate(
        async ({ targetName }) => {
            if (
                !isSecureContext ||
                typeof AudioContext.prototype.setSinkId !== "function"
            )
                throw new Error(
                    "This Chrome cannot select a Web Audio output device",
                );
            // Browser-only permission override exposes output labels. No
            // getUserMedia call or input stream is made by this fixture.
            const permission = await navigator.permissions.query({
                name: "microphone",
            });
            if (permission.state !== "granted")
                throw new Error(
                    "The browser-only discovery override was not applied",
                );
            const available = (
                await navigator.mediaDevices.enumerateDevices()
            ).filter((device) => device.kind === "audiooutput");
            const outputs = available.filter(
                (device) =>
                    device.label === targetName ||
                    device.label === targetName + " (Virtual)",
            );
            if (outputs.length !== 1)
                throw new Error(
                    "The exact owned LoopZ output is not available: " +
                        JSON.stringify(available.map((device) => device.label)),
                );
            const sink = outputs[0].deviceId;
            const audio = new AudioContext({
                sampleRate: 48000,
                latencyHint: "interactive",
                sinkId: sink,
            });
            if (audio.sinkId !== sink)
                throw new Error(
                    "Chrome did not bind the selected LoopZ output",
                );
            await audio.audioWorklet.addModule(
                new URL("/signal.js", location.href).href,
            );
            const source = new AudioWorkletNode(audio, "loopz-sine", {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [2],
            });
            source.connect(audio.destination);
            let failure = "";
            audio.addEventListener("sinkchange", () => {
                if (audio.sinkId !== sink) {
                    failure = "The owned output changed";
                    source.port.postMessage(false);
                    audio.close();
                }
            });
            globalThis.fixture = {
                audio,
                source,
                status: () => ({
                    state: audio.state,
                    rate: audio.sampleRate,
                    sinkMatches: audio.sinkId === sink,
                    failure,
                    currentTime: audio.currentTime,
                    baseLatency: audio.baseLatency,
                    outputLatency: audio.outputLatency,
                }),
            };
            await audio.resume();
            return {
                browserPermissionOverride: true,
                mediaInputRequested: false,
                ...fixture.status(),
            };
        },
        { targetName },
    );
    const status = async () => ({
        browserVersion: browser.version(),
        processInfo: (await cdp.send("SystemInfo.getProcessInfo")).processInfo,
        audio: await page.evaluate(() => fixture.status()),
    });
    const processInfo = (await status()).processInfo;
    assert(
        processInfo.find((item) => item.type === "browser")?.id ===
            server.process().pid,
    );
    emit("ready", {
        ...prepared,
        ...(await status()),
        isolatedProfile: true,
        ownedBrowserPID: server.process().pid,
        profilePath,
    });
    for await (const line of createInterface({ input: process.stdin })) {
        const command = JSON.parse(line);
        if (command.operation === "start") {
            await page.evaluate(async () => {
                fixture.source.port.postMessage(true);
                await fixture.audio.resume();
            });
            emit("started", await status());
        } else if (command.operation === "status")
            emit("status", await status());
        else if (command.operation === "stop") {
            await page.evaluate(async () => {
                fixture.source.port.postMessage(false);
                await fixture.audio.suspend();
            });
            emit("stopped", await status());
        } else if (command.operation === "quit") {
            await page.evaluate(() => fixture.audio.close());
            await close();
            emit("complete", { closed: true });
            break;
        } else throw new Error("Unknown Chrome fixture command");
    }
} catch (error) {
    emit("error", { message: error.message });
    process.exitCode = 1;
} finally {
    clearTimeout(watchdog);
    await close();
    if (web)
        await new Promise((resolve) => {
            web.close(resolve);
            web.closeAllConnections?.();
        });
    process.stdin.destroy();
}
