const helpers = `
const checks = [];
const check = (condition, description) => {
    if (!condition) throw new Error(description);
    checks.push(description);
};
const until = async predicate => {
    const end = performance.now() + 10000;
    while (!await predicate()) {
        if (performance.now() > end) throw new Error(JSON.stringify({checks, body: document.body.innerText.slice(0, 1600)}));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const state = () => window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "state"});
const fixture = (operation, fields = {}) => window.webkit.messageHandlers.fixture.postMessage({operation, ...fields});
const foot = () => document.querySelector('[data-od-id="source-app"] .lz-node-foot');
const show = () => [...foot().querySelectorAll("button")].find(button => button.textContent.trim() === "Show device");
const canonical = value => Array.isArray(value) ? value.map(canonical)
    : value && typeof value === "object"
      ? Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])]))
      : value;
await until(() => document.querySelector('[aria-label="Select Mix"]'));
`;

export const beforeUpstreamReload = `${helpers}
const data = await state();
const consumer = data.snapshot.document.devices[0];
const physical = structuredClone(consumer);
physical.id = "physical-input";
physical.name = "Physical input";
physical.monitors = [];
physical.bindings[0] = {id: "app", kind: "input-device", selector: "input", muteOriginal: false};
const middle = structuredClone(physical);
middle.id = "dependency";
middle.name = "Dependency";
middle.bindings[0] = {id: "app", kind: "virtual-device", selector: physical.id, muteOriginal: false};
consumer.bindings[0] = {id: "app", kind: "virtual-device", selector: middle.id, muteOriginal: false};
data.snapshot.document.devices.push(middle, physical);
data.snapshot.runtime.permissions = {microphone: "authorized"};
data.snapshot.runtime.sources = [{kind: "input-device", selector: "input", demanded: true, connected: true, complete: true, error: ""}];
const saved = JSON.stringify(canonical(data.snapshot.document));
await fixture("replace-envelope", {envelope: data});
await until(() => document.querySelector('[aria-label="Dependency options"]'));
check(!foot().classList.contains("attention"), "Healthy virtual dependencies remain available");
data.snapshot.runtime.sources[0].connected = false;
data.snapshot.runtime.sources[0].complete = false;
await fixture("replace-envelope", {envelope: data});
await until(() => foot().textContent.includes("Upstream source unavailable") && show());
check(foot().classList.contains("attention"), "Disconnected upstream inputs mark the virtual source for attention");
const meter = foot().closest("article").querySelector('[role="meter"]');
check(Number(meter.getAttribute("aria-valuenow")) > -60, "Partial upstream loss preserves live signal meters");
show().click();
await until(() => document.querySelector('[aria-label="Microphone options"]'));
check(document.querySelector("h1").textContent === "Physical input", "Show device selects the actual upstream input owner across two dependencies");
document.querySelector('[aria-label="Select Mix"]').click();
await until(() => document.querySelector('[aria-label="Dependency options"]'));
data.snapshot.runtime.sources[0].connected = true;
data.snapshot.runtime.sources[0].complete = true;
await fixture("replace-envelope", {envelope: data});
await until(() => !foot().classList.contains("attention"));
check(!show(), "Fresh provider recovery automatically removes the upstream warning");
check(JSON.stringify(canonical((await state()).snapshot.document)) === saved, "Upstream review and recovery preserve the routing document");
return {checks};
`;

export const afterUpstreamReload = `${helpers}
await until(() => document.querySelector('[aria-label="Dependency options"]'));
check(!foot().classList.contains("attention"), "Reload preserves the recovered virtual source status");
const data = await state();
data.snapshot.runtime.permissions.microphone = "denied";
await fixture("replace-envelope", {envelope: data});
await until(() => foot().textContent.includes("Upstream source unavailable") && show());
show().click();
await until(() => document.querySelector('[aria-label="Microphone options"]'));
check(foot().textContent.includes("Microphone access required"), "Upstream navigation reveals the permission condition at its owner");
check((await fixture("commands")).every(command => !["edit", "retry", "microphone"].includes(command.operation)), "Source review does not edit routing, force retries or request permission");
return {checks, snapshotContext: "Upstream microphone access condition in a fixture"};
`;
