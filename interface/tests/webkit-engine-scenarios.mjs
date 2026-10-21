import { gainScenario } from "./webkit-gain-scenario.mjs";
import { outputPairScenario } from "./webkit-output-pair-durable-scenario.mjs";

const helpers = `
const checks = [];
const check = (condition, name) => {
    if (!condition) throw new Error(JSON.stringify({name, checks}));
    checks.push(name);
};
const until = async (predicate, name) => {
    const deadline = performance.now() + 15000;
    while (!await predicate()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({
            name, checks, inert: document.querySelector(".lz-window")?.inert,
            body: document.body.innerText.slice(0, 1500), snapshot: await state(),
        }));
        await new Promise(resolve => setTimeout(resolve, 30));
    }
};
const state = () => window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "state"});
const nativeKey = (key, {command = false, shift = false} = {}) =>
    window.webkit.messageHandlers.microphoneFixture.postMessage({operation: "native-key", key, command, shift});
const button = (text) => [...document.querySelectorAll("button")]
    .find(item => item.textContent.trim() === text);
const click = async (text) => {
    await until(() => button(text) && !button(text).matches(":disabled") && !button(text).closest("[inert]"), "Button ready: " + text);
    button(text).click();
};
const clickLabel = async label => {
    const find = () => document.querySelector('[aria-label="' + label + '"]');
    await until(() => find() && !find().matches(":disabled") && !find().closest("[inert]"), "Control ready: " + label);
    find().click();
};
const name = async (value) => {
    await until(() => document.querySelector('[aria-label="Device name"]'), "Name input ready");
    const input = document.querySelector('[aria-label="Device name"]');
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set.call(input, value);
    input.dispatchEvent(new Event("input", {bubbles: true}));
};
await until(async () => (await state()).connection.ready, "Authenticated Engine ready");
await until(async () => (await window.webkit.messageHandlers.microphoneFixture.postMessage({operation: "installation-status"})).pending, "Installation inspection started off the transport queue");
const duringInspection = await state();
check(duringInspection.connection.ready && duringInspection.installation.checked === false, "Authenticated state reads continue while installation inspection is blocked");
await window.webkit.messageHandlers.microphoneFixture.postMessage({operation: "installation-release"});
await until(async () => (await state()).installation.checked, "Installation inspection published on the transport queue");
check(Object.values((await state()).installation.components).every(component => component.state === "installed"), "Fixture installation checks report all three components after release");
`;

export const beforeRestart = `${helpers}
let snapshot = await state();
for (const request of [
    {key: "q", command: true, shift: false},
    {key: "x", command: true, shift: false},
    {key: "z", command: 1, shift: false},
    {key: "x", command: false, shift: true},
    {key: "z", command: true, shift: false, extra: true},
]) {
    const rejected = await window.webkit.messageHandlers.microphoneFixture.postMessage({
        operation: "native-key", ...request,
    }).then(() => false, () => true);
    check(rejected, "Private native-key handler rejects unsupported keys, flags and fields");
}
check(await window.webkit.messageHandlers.loopz.postMessage({version: 1,
    operation: "native-key", key: "x", command: false, shift: false}).then(() => false, () => true),
    "The production bridge exposes no native keyboard injection operation");
await click("Settings");
await until(() => button("Enable microphone access"), "First-time microphone action appears");
const enable = button("Enable microphone access");
enable.click();
enable.click();
const microphone = operation => window.webkit.messageHandlers.microphoneFixture.postMessage({operation});
await until(async () => (await microphone("status")).pending, "Permission request starts");
check(button("Waiting for macOS…")?.disabled, "Explicit consent shows a pending disabled action");
check((await state()).connection.ready && (await microphone("status")).pending,
    "State requests continue while consent is pending");
const duplicate = await window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "request-microphone"});
check(!duplicate.ok && duplicate.code === "busy" && (await microphone("status")).requests === 1,
    "Repeated consent requests cannot launch another helper");
const malformed = await window.webkit.messageHandlers.loopz.postMessage({version: 1,
    operation: "request-microphone", path: "/untrusted/helper"}).then(() => false, () => true);
check(malformed && (await microphone("status")).requests === 1,
    "The page cannot inject permission-helper arguments");
await click("Done");
await until(() => !document.querySelector('[role="dialog"]'), "Pending settings closes");
await click("Settings");
await until(() => button("Waiting for macOS…")?.disabled, "Reopened settings observes pending consent");
check((await microphone("status")).requests === 1, "Reopening settings retains the pending request");
await microphone("release");
await until(() => document.querySelector('[role="dialog"]')?.textContent.includes("Access granted to LoopZ Engine"), "Consent result displayed");
await until(async () => (await state()).snapshot.runtime.permissions.microphone === "authorized", "Permission metadata refreshes");
await until(() => !button("Enable microphone access") && !button("Waiting for macOS…"), "Granted permission reaches the interface");
check((await microphone("status")).requests === 1, "Granted access refreshes without another request");
await click("Done");
await until(() => !document.querySelector('[role="dialog"]'), "Settings closes");
check(snapshot.snapshot.canCreateDocument && !snapshot.snapshot.document,
    "First run offers explicit creation through the actual bridge");
await click("+ New virtual device");
await name("Acceptance Mix");
const channels = document.querySelector('[aria-label="New device channel count"]');
channels.value = "8";
channels.dispatchEvent(new Event("change", {bubbles: true}));
const newRate = document.querySelector('[aria-label="New device sample rate"]');
check([...newRate.options].some(option => option.value === '192000'), 'New-device choices follow the high-rate engine capability');
newRate.value = '96000';
newRate.dispatchEvent(new Event('change', {bubbles:true}));
await click("Create device");
await until(() => document.querySelector('[aria-label="Select Acceptance Mix"]'), "Created device appears");
snapshot = await state();
check(snapshot.snapshot.document.devices[0].routing.outputChannels === 8,
    "Eight-channel device creation reaches the real runtime");
check(snapshot.snapshot.document.devices[0].sampleRate === 96000, 'New device saves the explicitly selected 96 kHz format');
check(snapshot.snapshot.document.devices[0].routing.sources.length === 0 && snapshot.snapshot.document.devices[0].bindings.length === 0,
    'New device creation adds no implicit pass-through source or connections');
await until(() => !document.querySelector('[role="dialog"]'), "Creation closes dialog");
const firstDeviceID = snapshot.snapshot.document.devices[0].id;
await clickLabel("Undo routing change");
await until(async () => (await state()).snapshot.document.devices.length === 0, "Undo first creation");
check(!(await state()).snapshot.canCreateDocument, "Undo creation preserves the existing saved document");
await clickLabel("Redo routing change");
await until(async () => (await state()).snapshot.document.devices[0]?.id === firstDeviceID, "Redo retains created identity");
check(true, "Create undo/redo traverses the actual bridge and preserves device identity");
${outputPairScenario}
${gainScenario}
const rateControl = document.querySelector('[aria-label="Sample rate"]');
await until(() => !rateControl.matches(':disabled'), 'Rate control ready');
rateControl.value = '192000';
rateControl.dispatchEvent(new Event('change', {bubbles:true}));
await until(async () => (await state()).snapshot.document.devices[0].sampleRate === 192000 && (await state()).snapshot.runtime.restored,
    '192 kHz edit reaches the active private graph');
const savedRate = await window.webkit.messageHandlers.microphoneFixture.postMessage({operation:'saved-device-gains'});
check(savedRate.fromPrimary && savedRate.devices[0].sampleRate === 192000, '192 kHz format reaches the primary saved file');
await clickLabel("Mute device");
await until(async () => (await state()).snapshot.document.devices[0].routing.muted, "Mute committed");
check(true, "Device controls save through authenticated XPC");
await until(async () => (await microphone("edit-menu-status")).undo, "Native menu receives routing context");
await microphone("menu-undo");
await until(async () => !(await state()).snapshot.document.devices[0].routing.muted, "Native Edit menu undoes mute");
await until(async () => (await microphone("edit-menu-status")).redo, "Native redo becomes enabled");
await microphone("menu-redo");
await until(async () => (await state()).snapshot.document.devices[0].routing.muted, "Native Edit menu redoes mute");
check(true, "Native menu actions traverse the authenticated history service");
await until(async () => (await microphone("edit-menu-status")).undo, "Native shortcut undo ready");
const shortcutRevision = (await state()).snapshot.revision;
check((await nativeKey("z", {command: true})).menuHandled, "Command-Z uses the product Edit menu binding");
await until(async () => (await state()).snapshot.document.devices[0]?.routing.muted === false, "Command-Z undoes mute");
check(BigInt((await state()).snapshot.revision.generation) === BigInt(shortcutRevision.generation) + 1n,
    "Command-Z commits exactly one history transition");
await until(async () => (await microphone("edit-menu-status")).redo, "Native shortcut redo ready");
check((await nativeKey("z", {command: true, shift: true})).menuHandled,
    "Command-Shift-Z uses the product Edit menu binding");
await until(async () => (await state()).snapshot.document.devices[0]?.routing.muted === true, "Command-Shift-Z redoes mute");
check(BigInt((await state()).snapshot.revision.generation) === BigInt(shortcutRevision.generation) + 2n,
    "Command-Shift-Z commits exactly one history transition");
await until(() => !document.querySelector('[aria-label="Mute device"]').disabled, "Controls ready");
await clickLabel("Device options");
await click("Rename device");
await name("Renamed Acceptance Mix");
const beforeTextMenu = (await state()).snapshot.revision;
document.querySelector('[aria-label="Device name"]').focus();
await new Promise(resolve => setTimeout(resolve, 50));
await microphone("menu-undo");
await new Promise(resolve => setTimeout(resolve, 50));
const afterTextMenu = (await state()).snapshot.revision;
check(afterTextMenu.epoch === beforeTextMenu.epoch && afterTextMenu.generation === beforeTextMenu.generation,
    "Text-field menu undo cannot undo the routing graph");
const nameInput = document.querySelector('[aria-label="Device name"]');
nameInput.setSelectionRange(nameInput.value.length, nameInput.value.length);
const typedEvents = [];
const observeTyping = event => typedEvents.push({key: event.key, trusted: event.isTrusted});
nameInput.addEventListener("keydown", observeTyping);
check(!(await nativeKey("x")).menuHandled, "Plain typing is delivered to the fixture window");
await until(() => nameInput.value === "Renamed Acceptance Mixx", "Native typing edits the text field");
nameInput.removeEventListener("keydown", observeTyping);
check(typedEvents.length === 1 && typedEvents[0].key === "x" && typedEvents[0].trusted,
    "WebKit receives one trusted native text key");
await until(async () => (await microphone("edit-menu-status")).undo, "Text undo enabled in native menu");
check((await nativeKey("z", {command: true})).menuHandled, "Text undo uses the native menu");
await until(() => nameInput.value === "Renamed Acceptance Mix", "Native menu restores text before typing");
await until(async () => (await microphone("edit-menu-status")).redo, "Text redo enabled in native menu");
check((await nativeKey("z", {command: true, shift: true})).menuHandled, "Text redo uses the native menu");
await until(() => nameInput.value === "Renamed Acceptance Mixx", "Native menu redoes typed text");
await until(async () => (await microphone("edit-menu-status")).undo, "Text undo ready again");
await nativeKey("z", {command: true});
await until(() => nameInput.value === "Renamed Acceptance Mix", "Final text undo restores the intended name");
const afterTyping = (await state()).snapshot.revision;
check(afterTyping.epoch === beforeTextMenu.epoch && afterTyping.generation === beforeTextMenu.generation,
    "Typing, text undo and text redo leave routing history unchanged");
await click("Save name");
await until(() => document.querySelector('[aria-label="Select Renamed Acceptance Mix"]'), "Rename observed");
check(true, "Rename returns through the service to the interface");
await until(() => !document.querySelector('[role="dialog"]'), "Rename closes dialog");
await clickLabel("Device options");
await click("Duplicate device");
await until(async () => (await state()).snapshot.document.devices.length === 2, "Duplicate committed");
await until(() => document.querySelector('[aria-label="Select Renamed Acceptance Mix Copy"]'), "Duplicate appears");
snapshot = await state();
const copy = snapshot.snapshot.document.devices.find(device => device.name.endsWith(" Copy"));
check(copy.routing.muted && copy.routing.outputChannels === 8,
    "Duplicate retains channel layout and controls");
await until(() => !document.querySelector('[role="dialog"]'), "Duplicate closes dialog");
await clickLabel("Select Renamed Acceptance Mix Copy");
await until(() => document.querySelector('[aria-label="Select Renamed Acceptance Mix Copy"]').getAttribute("aria-current") === "true",
    "Duplicate selected");
await clickLabel("Device options");
await click("Delete device…");
await click("Delete device");
await until(async () => (await state()).snapshot.document.devices.length === 1, "Deletion committed");
await until(() => !document.querySelector('[role="dialog"]'), "Deletion closes dialog");
await until(async () => (await state()).snapshot.runtime.restored, "Runtime restored after edits");
check(true, "Confirmed deletion preserves the original device");
await clickLabel("Undo routing change");
await until(async () => (await state()).snapshot.document.devices.some(device => device.id === copy.id), "Undo deletion restores saved identity");
check(true, "Deleted device and its saved controls are undoable through the interface");
await clickLabel("Redo routing change");
await until(async () => (await state()).snapshot.document.devices.length === 1, "Redo deletion");
await until(async () => (await state()).snapshot.runtime.restored, "Redone graph reaches its active generation");
snapshot = await state();
// Leave an owned fixture request pending to exercise actual bridge shutdown.
globalThis.pendingMicrophoneOnClose = window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "request-microphone"});
globalThis.pendingMicrophoneOnClose.catch(() => {});
await until(async () => (await microphone("status")).requests === 2 && (await microphone("status")).pending, "Close-time request pending");
check(true, "Editor close is exercised with a pending consent operation");
return {checks, revision: snapshot.snapshot.revision, device: snapshot.snapshot.document.devices[0].id};
`;

export const afterRestart = `${helpers}
await until(() => document.querySelector('[aria-label="Select Renamed Acceptance Mix"]'), "Saved device restored");
const snapshot = await state();
const device = snapshot.snapshot.document.devices[0];
check(snapshot.snapshot.document.devices.length === 1 && !snapshot.snapshot.canCreateDocument,
    "Reconstructed runtime loads the existing document");
check(!snapshot.snapshot.history.canUndo && !snapshot.snapshot.history.canRedo,
    "A new runtime starts with empty routing history");
check(device.name === "Renamed Acceptance Mix" && device.routing.outputChannels === 8 && device.routing.muted,
    "Saved name, layout and controls survive runtime reconstruction");
check(device.routing.gain === 1, "Saved final gain survives runtime reconstruction");
check(device.sampleRate === 192000 && document.querySelector('[aria-label="Sample rate"]').value === '192000',
    '192 kHz format survives runtime reconstruction and editor reopen');
check(document.querySelector('[aria-label="Mute device"]').classList.contains("active"),
    "Reopened interface reflects saved mute state");
await until(async () => (await state()).snapshot.runtime.restored, "Reconstructed graph restored");
check(true, "Reopened editor receives restored runtime state");
const microphone = await window.webkit.messageHandlers.microphoneFixture.postMessage({operation: "status"});
check(microphone.requests === 2 && microphone.canceled === 1 && !microphone.pending,
    "Editor close cancels its owned request and reopen does not replay it");
return {checks, revision: snapshot.snapshot.revision, device: device.id};
`;
