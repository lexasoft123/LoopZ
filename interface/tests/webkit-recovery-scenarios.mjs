const helpers = `
const checks = [];
const check = (condition, description) => {
    if (!condition) throw new Error(description);
    checks.push(description);
};
const until = async (predicate) => {
    const deadline = performance.now() + 10000;
    while (!await predicate()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({checks,
            body: document.body.innerText.slice(0, 1500)}));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const state = () => window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "state"});
const fixture = (operation, fields = {}) => window.webkit.messageHandlers.fixture.postMessage({operation, ...fields});
const button = text => [...document.querySelectorAll("button")].find(el => el.textContent.trim() === text);
const close = async () => {
    document.activeElement.dispatchEvent(new KeyboardEvent("keydown", {
        key: "Escape", code: "Escape", bubbles: true, cancelable: true,
    }));
    await until(() => !document.querySelector('[role="dialog"]'));
};
const foot = () => document.querySelector('[data-od-id="source-app"] .lz-node-foot');
const canonical = value => Array.isArray(value) ? value.map(canonical)
    : value && typeof value === "object"
      ? Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])]))
      : value;
await until(() => document.querySelector('[aria-label="Select Mix"]'));
`;

export const beforeRecoveryReload = `${helpers}
const data = await state();
data.snapshot.document.devices[0].bindings[0] = {
    id: "app", kind: "input-device", selector: "input", muteOriginal: false,
};
data.inventory.devices[0].inputChannels = 2;
data.snapshot.revision.generation = (BigInt(data.snapshot.revision.generation) + 1n).toString();
data.snapshot.restoration.activeGeneration = data.snapshot.restoration.desiredGeneration = data.snapshot.revision.generation;
data.snapshot.runtime.permissions = {microphone: "authorized"};
data.snapshot.runtime.sources = [{kind: "input-device", selector: "input", demanded: true,
    connected: true, complete: true, error: ""}];
const saved = JSON.stringify(canonical(data.snapshot.document));
await fixture("replace-envelope", {envelope: data});
await until(() => document.querySelector('[aria-label="Microphone options"]'));
check(!foot().classList.contains("attention"), "Healthy source has no recovery action");

for (const permission of ["denied", "restricted", "not-determined"]) {
    data.snapshot.runtime.permissions.microphone = permission;
    await fixture("replace-envelope", {envelope: data});
    await until(() => foot().textContent.includes("Microphone access") && button("Review access"));
    await until(() => foot().closest("article").querySelector('[role="meter"]').getAttribute("aria-valuenow") === "-60");
    button("Review access").click();
    await until(() => document.querySelector('[aria-label="Settings"]'));
    const expected = permission === "denied" ? "turned off" : permission === "restricted" ? "restricted on this Mac" : "not been granted yet";
    await until(() => document.querySelector('[aria-label="Settings"]').textContent.includes(expected));
    check(true, "Settings explains " + permission + " microphone access and clears cached input meters");
    await close();
}

data.snapshot.runtime.permissions.microphone = "authorized";
data.snapshot.runtime.sources[0].complete = false;
await fixture("replace-envelope", {envelope: data});
await until(() => button("Review source"));
button("Review source").click();
await until(() => document.querySelector('[role="dialog"]')?.textContent.includes("Available channels keep working"));
check(true, "Partial inputs offer source review and preserve available channels");
await close();

data.snapshot.runtime.sources[0].connected = false;
data.snapshot.runtime.sources[0].error = "Internal access lookup failed";
await fixture("replace-envelope", {envelope: data});
await until(() => foot().textContent.includes("Waiting for input device") && button("Retry now"));
check(!foot().textContent.includes("Internal access"), "Native error wording does not become recovery instructions");
button("Retry now").click();
await until(async () => (await fixture("commands")).length === 1);
check((await fixture("commands"))[0].operation === "retry", "Retry calls the explicit recovery operation");

data.connection.ready = false;
await fixture("replace-envelope", {envelope: data});
await until(() => foot().textContent.includes("Status unavailable"));
check(!foot().querySelector("button"), "Disconnected state offers no misleading source retry");
check(document.querySelector('[data-od-id="monitor-monitor"]').textContent.includes("Status unavailable"),
    "Disconnected state does not advertise a healthy monitor");

data.connection.ready = true;
data.snapshot.runtime.sources[0].connected = true;
data.snapshot.runtime.sources[0].complete = true;
data.snapshot.runtime.sources[0].error = "";
await fixture("replace-envelope", {envelope: data});
await until(() => !foot().classList.contains("attention"));
check(!foot().querySelector("button"), "Fresh healthy state removes recovery actions automatically");
check(JSON.stringify(canonical((await state()).snapshot.document)) === saved, "Permission and disconnect handling never changes saved intent");
return {checks, commands: await fixture("commands")};
`;

export const afterRecoveryReload = `${helpers}
await until(() => document.querySelector('[aria-label="Microphone options"]'));
check(!foot().classList.contains("attention"), "Reopened interface reflects the restored source");
check((await fixture("commands")).length === 1, "Reload does not replay recovery or request permission");
const data = await state();
data.snapshot.runtime.permissions.microphone = "denied";
await fixture("replace-envelope", {envelope: data});
await until(() => button("Review access"));
button("Review access").click();
await until(() => document.querySelector('[aria-label="Settings"]')?.textContent.includes("turned off"));
const bounds = document.querySelector('[aria-label="Settings"]').getBoundingClientRect();
check(bounds.top >= 0 && bounds.bottom <= innerHeight && bounds.left >= 0 && bounds.right <= innerWidth,
    "Permission guidance stays inside the dialog viewport");
check((await fixture("commands")).length === 1, "Access guidance opens without requesting a system action");
// The fixture's window is hidden. Finish finite entrance animations so the
// snapshot represents the settled dialog rather than its transparent first frame.
for (const animation of document.getAnimations()) {
    if (Number.isFinite(animation.effect?.getComputedTiming().endTime)) animation.finish();
}
return {checks, snapshotContext: "Simulated denied-microphone settings"};
`;
