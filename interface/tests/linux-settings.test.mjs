import test, { after } from "node:test";
import assert from "node:assert/strict";
import { build } from "esbuild";
import { mkdtemp, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve, join } from "node:path";
import { createRequire } from "node:module";

// Render the real settings tree, including UIKit controls, without a browser.
// This catches platform-specific actions leaking into the visible dialog.
const root = resolve(import.meta.dirname, "..");
const temporary = await mkdtemp(join(tmpdir(), "loopz-linux-settings-"));
after(() => rm(temporary, { recursive: true, force: true }));
const built = await build({
    stdin: {
        contents: `
            import React from 'react';
            import {renderToStaticMarkup} from 'react-dom/server';
            import Dialogs from './src/Dialogs.jsx';
            import {Routing} from './src/Routing.jsx';
            import {project} from './src/model.js';
            import {setLanguage} from './src/i18n.js';
            import {engineFixture} from './tests/fixtures.mjs';
            export function render(platform, language='en', kind='settings') {
                setLanguage(language);
                const envelope=engineFixture();
                envelope.snapshot.runtime.platform=platform;
                if(kind==='routing') {
                    envelope.snapshot.document.devices[0].routing.sources=[];
                    envelope.snapshot.document.devices[0].bindings=[];
                    envelope.snapshot.document.devices[0].monitors=[];
                }
                if(kind==='add-source') {
                    const extra=structuredClone(envelope.snapshot.document.devices[0]);
                    extra.id='other'; extra.name='Other mix';
                    envelope.snapshot.document.devices.push(extra);
                }
                const state=project(envelope);
                if(kind==='routing') return renderToStaticMarkup(React.createElement(Routing, {
                    state, device:state.devices[0], send:()=>{}, open:()=>{},
                    selection:null, onSocket:()=>{}, onClearSelection:()=>{}
                }));
                return renderToStaticMarkup(React.createElement(Dialogs, {
                    modal:{kind,device:state.devices[0].id}, state, returnFocus:{current:null},
                    send:()=>{}, close:()=>{}, setModal:()=>{}, transport:()=>{},
                    refresh:()=>{}, startTour:()=>{}
                }));
            }
        `,
        resolveDir: root,
        loader: "jsx",
    },
    plugins: [
        {
            name: "language-browser-subscription",
            setup(builder) {
                // Language's external-store hook has no server snapshot. Its menu
                // is outside these settings checks; retain the real localized copy.
                builder.onLoad(
                    { filter: /[/\\]components[/\\]Language\.jsx$/ },
                    () => ({
                        contents: "export function Language() { return null; }",
                        loader: "js",
                    }),
                );
            },
        },
    ],
    bundle: true,
    platform: "node",
    format: "cjs",
    write: false,
    define: { "process.env.NODE_ENV": '"production"' },
});
const modulePath = join(temporary, "settings.cjs");
await writeFile(modulePath, built.outputFiles[0].contents);
const { render } = createRequire(import.meta.url)(modulePath);

test("Linux settings explain actual input access and service lifetime without unsupported actions", () => {
    const html = render("linux");
    assert.match(html, /Linux device permissions control access/);
    assert.match(html, /Application audio capture is unavailable/);
    assert.match(html, /even after you close this window/);
    assert.match(html, /Manage automatic startup through your Linux session/);
    assert.doesNotMatch(
        html,
        /Checking login startup|Checking background recovery/,
    );
    assert.doesNotMatch(
        html,
        /Open settings|Enable microphone access|Waiting for macOS/,
    );
    assert.doesNotMatch(
        html,
        /Automatic recovery|Setup needed|Hardware buffer/,
    );
    assert.doesNotMatch(html, /role="switch"/);
});

test("existing host keeps microphone and login settings actions", () => {
    const html = render("macos");
    assert.match(html, /macOS controls system audio-recording access/);
    assert.match(html, /Open settings/);
    assert.match(html, /Checking login startup/);
    assert.match(html, /role="switch"/);
});

test("Linux access and background explanations render in Russian and Simplified Chinese", () => {
    const russian = render("linux", "ru");
    assert.match(russian, /правами устройств Linux/);
    assert.match(russian, /предварительной версии для Linux/);
    const chinese = render("linux", "zh-CN");
    assert.match(chinese, /Linux 设备权限/);
    assert.match(chinese, /Linux 预览版/);
    assert.doesNotMatch(
        russian + chinese,
        /Linux device permissions control access/,
    );
});

test("Linux source picker offers hardware and internal mixes without unsupported capture actions", () => {
    const html = render("linux", "en", "add-source");
    assert.match(html, /Select a hardware input or another mix/);
    assert.match(html, /Microphone/);
    assert.match(html, /Other mix/);
    assert.doesNotMatch(
        html,
        /Applications|Pass-through|Choose another application|Search apps/,
    );
    const mac = render("macos", "en", "add-source");
    assert.match(mac, /Applications/);
    assert.match(mac, /Pass-through/);
    assert.match(mac, /Choose another application/);
});

test("Linux empty routing and creation explain hardware mixes instead of recording endpoints", () => {
    const routing = render("linux", "en", "routing");
    assert.match(routing, /Add a hardware input or another mix/);
    assert.match(
        routing,
        /Add a monitor to route this mix to a hardware output/,
    );
    assert.doesNotMatch(routing, /Record anywhere|recording app|Add an app/);
    const create = render("linux", "en", "create");
    assert.match(create, /New mix/);
    assert.match(create, /Combine hardware inputs and other mixes/);
    assert.doesNotMatch(
        create,
        /choose in your recording app|New virtual device/,
    );
    assert.match(
        render("macos", "en", "create"),
        /choose in your recording app/,
    );
});
