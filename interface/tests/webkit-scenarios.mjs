const helpers = `
const checks = [];
const check = (condition, description) => {
    if (!condition) throw new Error(description);
    checks.push(description);
};
const until = async (predicate) => {
    const deadline = performance.now() + 10000;
    while (!predicate()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({
            message: "UI readiness timed out", checks,
            dialog: document.querySelector('[role="dialog"]')?.getAttribute("aria-label"),
            focus: document.activeElement?.outerHTML?.slice(0, 250),
            body: document.body.innerText.slice(0, 500),
        }));
        await new Promise((resolve) => setTimeout(resolve, 20));
    }
};
const button = (text) => [...document.querySelectorAll("button")].find(
    (item) => item.textContent.trim() === text,
);
const key = (key, options = {}) => {
    document.activeElement.dispatchEvent(new KeyboardEvent("keydown", {
        key, code: key.length === 1 ? "Key" + key.toUpperCase() : key,
        bubbles: true, cancelable: true, ...options,
    }));
};
await until(() => document.querySelector('[aria-label="Select Second Mix"]'));
`;

export const beforeReload = `${helpers}
check(location.href === "loopz://app/index.html", "Production resource origin loaded");
check(document.styleSheets.length === 1, "Bundled SingZ styles loaded");
check(document.querySelector(".lz-window").getBoundingClientRect().width === 1360,
    "Desktop layout fills WebKit viewport");
check(document.documentElement.scrollWidth <= innerWidth, "No horizontal document overflow");
button("Appearance").click();
await until(() => document.documentElement.dataset.szPalette === "atelier");
check(JSON.parse(localStorage.getItem("loopz-appearance")).theme === "atelier",
    "Appearance saved through the interface");
document.querySelector('[aria-label="Select Second Mix"]').click();
await until(() => JSON.parse(localStorage.getItem("loopz-appearance")).selected === "second");
check(true, "Device selection saved through the interface");
const settings = button("Settings");
settings.focus();
settings.click();
await until(() => document.querySelector('[role="dialog"]'));
// Wait for React's effects, which install the focus trap after rendering.
await until(() => document.querySelector(".lz-window").inert);
const dialog = document.querySelector('[role="dialog"]');
check(dialog.getAttribute("aria-label") === "Settings", "Settings dialog has an accessible name");
check(dialog.contains(document.activeElement), "Settings receives focus");
const controls = [...dialog.querySelectorAll("button,input,select,summary,a[href]")]
    .filter(item => !item.matches(":disabled") && item.getClientRects().length);
controls.at(-1).focus();
key("Tab");
check(document.activeElement === controls[0], "Tab trap wraps to the first control");
key("Tab", {shiftKey: true});
check(document.activeElement === controls.at(-1), "Shift-Tab trap wraps to the last control");
key("Escape");
await until(() => !document.querySelector('[role="dialog"]'));
await until(() => document.activeElement === settings);
check(!document.querySelector(".lz-window").inert, "Dismissal restores workspace interaction and focus");
key("n", {metaKey: true});
await until(() => document.querySelector('[aria-label="Device name"]'));
check(document.activeElement.getAttribute("aria-label") === "Device name",
    "New-device shortcut focuses its name field");
key("Escape");
await until(() => !document.querySelector('[role="dialog"]'));
check(true, "New-device dialog dismisses without creating a route");
return {checks, theme: document.documentElement.dataset.szPalette};
`;

export const afterReload = `${helpers}
await until(() => document.documentElement.dataset.szPalette === "atelier");
check(document.documentElement.dataset.szPalette === "atelier", "Appearance survives document reload");
check(document.querySelector('[aria-label="Select Second Mix"]').getAttribute("aria-current") === "true",
    "Device selection survives document reload");
check(!document.querySelector('[role="dialog"]'), "Transient dialogs are not restored");
check(document.styleSheets.length === 1, "Styles reload from the production scheme handler");
return {checks, theme: document.documentElement.dataset.szPalette};
`;
