const helpers = `
const checks = [];
const check = (condition, message) => {
    if (!condition) throw new Error(message);
    checks.push(message);
};
const until = async predicate => {
    const deadline = performance.now() + 10000;
    while (!predicate()) {
        if (performance.now() > deadline) throw new Error("Status UI timed out");
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const state = () => window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "state"});
const replace = envelope => window.webkit.messageHandlers.fixture.postMessage({operation: "replace-envelope", envelope});
const geometry = () => [".lz-device-header", ".lz-toolbar", ".lz-socket"].map(selector => {
    const box = document.querySelector(selector).getBoundingClientRect();
    return [box.x, box.y, box.width, box.height];
});
await until(() => document.querySelector(".lz-socket"));
`;

export const beforeStatusReload = `${helpers}
const before = JSON.stringify(geometry());
const data = await state();
data.snapshot.runtime.controlError = "Temporary service error";
await replace(data);
await until(() => document.querySelector(".lz-recovery").textContent.includes("This change needs attention"));
check(JSON.stringify(geometry()) === before, "Showing an alert leaves the header, toolbar and sockets in place");
check(getComputedStyle(document.querySelector(".lz-recovery")).position === "absolute", "Alerts float outside the routing layout");
data.snapshot.runtime.controlError = "";
await replace(data);
await until(() => !document.querySelector(".lz-recovery").textContent.includes("This change needs attention"));
check(JSON.stringify(geometry()) === before, "Clearing an alert leaves the routing layout unchanged");
data.snapshot.restoration.phase = "applying";
data.snapshot.restoration.desiredGeneration = "9007199254740994";
await replace(data);
await until(() => document.querySelector(".lz-footer-status").textContent.includes("Applying changes"));
check(!document.querySelector(".lz-recovery").textContent.includes("Applying saved changes"), "Routine apply progress stays in the footer");
check(JSON.stringify(geometry()) === before, "Applying changes does not move the routing canvas");
data.snapshot.restoration.phase = "restored";
data.snapshot.restoration.activeGeneration = data.snapshot.restoration.desiredGeneration;
await replace(data);
await until(() => !document.querySelector(".lz-footer-status").textContent.includes("Applying changes"));
return {checks};
`;

export const afterStatusReload = `${helpers}
check(!document.querySelector(".lz-recovery").textContent.includes("This change needs attention"), "Reload does not revive the cleared alert");
return {checks};
`;
