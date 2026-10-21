const helpers = `
const checks = [];
const check = (condition, description) => { if (!condition) throw new Error(description); checks.push(description); };
const until = async predicate => {
    const end = performance.now() + 10000;
    while (!await predicate()) {
        if (performance.now() > end) throw new Error(JSON.stringify({checks, body: document.body.innerText.slice(0, 1800)}));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const state = () => window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "state"});
const fixture = envelope => window.webkit.messageHandlers.fixture.postMessage({operation: "replace-envelope", envelope});
const button = name => [...document.querySelectorAll("button")].find(item => item.textContent.trim() === name);
const text = () => document.body.innerText;
const ready = () => ({checked: true, components: Object.fromEntries(["engine", "driver", "service"].map(id => [id, {state: "installed", detail: ""}]))});
const canonical = value => Array.isArray(value) ? value.map(canonical) : value && typeof value === "object" ? Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])])) : value;
await until(() => button("Settings"));
`;

export const beforeInstallationReload = `${helpers}
const data = await state();
const saved = JSON.stringify(canonical(data.snapshot.document));
data.installation = ready();
data.installation.components.engine = {state: "missing", detail: "Install the matching LoopZ Engine apps."};
data.connection = {connected: false, ready: false};
await fixture(data);
await until(() => text().includes("Finish LoopZ setup"));
check(!text().includes("Connecting to LoopZ"), "Missing components replace the generic connecting message");
button("Review setup").click();
await until(() => document.querySelector('[role="dialog"]') && text().includes("Audio engine installation"));
check(text().includes("Not installed") && text().includes("Audio driver installation") && text().includes("Audio service installation"), "Settings reports all three installed-component roles separately");
button("Done").click();
await until(() => !document.querySelector('[role="dialog"]'));
data.installation = ready();
data.connection = {connected: true, ready: true};
data.snapshot.restoration.phase = "waiting-for-driver";
await fixture(data);
await until(() => text().includes("Audio driver is not connected"));
check(text().includes("The audio components are installed"), "Installed files do not imply a connected driver");
data.snapshot.restoration.phase = "restored";
await fixture(data);
await until(() => !text().includes("Audio driver is not connected"));
check(JSON.stringify(canonical((await state()).snapshot.document)) === saved, "Setup guidance and recovery preserve every saved route");
return {checks};
`;

export const afterInstallationReload = `${helpers}
const data = await state();
data.installation = ready();
data.snapshot.restoration.phase = "no-document";
await fixture(data);
await until(() => !text().includes("Waiting for the audio service"));
check(!text().includes("Audio driver is not connected"), "An empty-document phase is not described as driver failure");
data.installation.components.service = {state: "invalid", detail: "The audio service configuration needs repair."};
await fixture(data);
await until(() => text().includes("LoopZ setup needs attention"));
check(text().includes("configuration needs repair"), "Invalid service configuration has distinct repair guidance");
data.installation.components.service = {state: "unknown", detail: "Retry the setup check."};
await fixture(data);
await until(() => text().includes("Could not check LoopZ setup"));
check(!text().includes("Finish LoopZ setup"), "An unknown inspection does not claim a missing component");
const commands = await window.webkit.messageHandlers.fixture.postMessage({operation: "commands"});
check(commands.length === 0, "Reviewing setup never edits routing, starts an installer or requests a permission");
return {checks, snapshotContext: "Unknown installation check in a private fixture"};
`;
