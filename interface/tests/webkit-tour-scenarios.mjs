const helpers = `
const checks = [];
const check = (condition, description) => {
    if (!condition) throw new Error(description);
    checks.push(description);
};
const until = async (predicate) => {
    const deadline = performance.now() + 10000;
    while (!predicate()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({ message: "Tour readiness timed out", checks, title: document.querySelector("#lz-tour-title")?.textContent, highlight: !!document.querySelector(".lz-tour svg > rect[stroke]"), target: document.querySelector('[data-tour="new-device"]')?.getBoundingClientRect().toJSON() }));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const button = (text) => [...document.querySelectorAll("button")].find(item => item.textContent.trim() === text);
const nativeKey = async (key, command = false) => {
    const result = await window.webkit.messageHandlers.fixture.postMessage({operation: "native-key", key, shift: false, command});
    check(result.ok, "Native key delivered: " + key);
};
await until(() => document.querySelector('[aria-label="Select Second Mix"]'));
`;

export const beforeTourReload = `${helpers}
check(!!button("Take a tour"), "First-run invitation is available");
button("Take a tour").click();
await until(() => document.querySelector("dialog.lz-tour[open]"));
check(!!document.querySelector('[aria-label="Select Demo Mix"]'), "Demo device is displayed instead of the real project");
check(document.body.textContent.includes("Demo headphones"), "Demo includes a monitor");
check(document.querySelector("dialog.lz-tour").contains(document.activeElement), "Tour receives focus in native WebKit");
await nativeKey("n", true);
window.dispatchEvent(new CustomEvent("loopz-history", {detail: "undo"}));
check(document.querySelectorAll('[role="dialog"]').length === 1, "App shortcuts cannot open dialogs or edit routing during the tour");
button("Next").click();
await until(() => document.querySelector("#lz-tour-title").textContent === "Create a virtual device");
await until(() => document.querySelector(".lz-tour svg > rect[stroke]"));
check(!!document.querySelector(".lz-tour svg > rect[stroke]"), "The real create-device control is highlighted");
await nativeKey("Escape");
await until(() => !document.querySelector("dialog.lz-tour"));
check(!!document.querySelector('[aria-label="Select Second Mix"]'), "Real project returns after exiting demo");
check(localStorage.getItem("loopz-tour-v1") === "seen", "Escape remembers dismissal");
return {checks};
`;

export const afterTourReload = `${helpers}
check(!button("Take a tour"), "Invitation remains dismissed after reload");
button("Settings").click();
await until(() => button("Take a tour"));
button("Take a tour").click();
await until(() => document.querySelector("dialog.lz-tour[open]"));
check(document.querySelectorAll('[role="dialog"]').length === 1, "Replaying closes Settings before showing the tour");
for (let step = 0; step < 12 && !button("Got it"); ++step) {
    const previous = document.querySelector("#lz-tour-title").textContent;
    button("Next").click();
    await until(() => document.querySelector("#lz-tour-title").textContent !== previous);
}
check(!!button("Got it"), "Full routing walkthrough reaches its final step");
const controls = [...document.querySelectorAll("dialog.lz-tour button")];
controls.at(-1).focus();
await nativeKey("Tab");
check(document.activeElement === controls[0], "Native Tab stays inside the tour");
button("Got it").click();
await until(() => !document.querySelector("dialog.lz-tour"));
check(document.activeElement === button("Settings"), "Completion returns focus to Settings");
return {checks};
`;
