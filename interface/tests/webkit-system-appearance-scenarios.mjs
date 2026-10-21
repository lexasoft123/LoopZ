const helpers = `
const checks = [];
const check = (condition, name) => {
    if (!condition) throw new Error(name);
    checks.push(name);
};
const until = async (condition, name) => {
    const deadline = performance.now() + 10000;
    while (!condition()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({
            name, checks, palette: document.documentElement.dataset.szPalette,
            stored: localStorage.getItem('loopz-appearance'),
            nativeDark: matchMedia('(prefers-color-scheme: dark)').matches,
        }));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const button = (text) => [...document.querySelectorAll('button')].find(
    item => item.textContent.trim() === text,
);
const preference = () => JSON.parse(localStorage.getItem('loopz-appearance') || '{}').theme;
const ordered = (value) => Array.isArray(value) ? value.map(ordered)
    : value && typeof value === 'object' ? Object.fromEntries(Object.keys(value).sort().map(key => [key, ordered(value[key])])) : value;
const palette = () => document.documentElement.dataset.szPalette;
const nativeAppearance = async (value) => {
    const result = await window.webkit.messageHandlers.fixture.postMessage({
        operation: 'window-appearance', value,
    });
    check(result.ok === true, 'Private native window accepted ' + value);
    await until(() => matchMedia('(prefers-color-scheme: dark)').matches === (value === 'dark'),
        'Actual WebKit media query follows native ' + value);
};
const choose = async (label, value, resolved) => {
    const dialog = document.querySelector('[role="dialog"]');
    const control = [...dialog.querySelectorAll('button')].find(item => item.textContent.trim() === label);
    check(!!control, 'SingZ appearance choice is present: ' + label);
    control.click();
    await until(() => preference() === value && palette() === resolved, 'Appearance choice applies: ' + label);
};
const openSettings = async () => {
    button('Settings').click();
    await until(() => document.querySelector('[role="dialog"][aria-label="Settings"]') && document.querySelector('.lz-window').inert,
        'Settings dialog is interactive');
    const dialog = document.querySelector('[role="dialog"]');
    // Offscreen WebKit pauses animation timelines. Finish only the private
    // scenario's entrance animations to inspect the final layout and snapshot.
    for (const animation of document.querySelector('.modal-scrim').getAnimations({subtree: true}))
        animation.finish();
    const group = dialog.querySelector('[role="group"][aria-label="Appearance"]');
    check(!!group && group.querySelectorAll('button').length === 3, 'Actual SingZ control exposes all three appearance choices');
    const bounds = dialog.getBoundingClientRect();
    check([...group.querySelectorAll('button')].every(item => {
        const rect = item.getBoundingClientRect();
        return rect.left >= bounds.left && rect.right <= bounds.right;
    }), 'All appearance choices fit inside the Settings dialog');
};
const closeSettings = async () => {
    button('Done').click();
    await until(() => !document.querySelector('[role="dialog"]') && !document.querySelector('.lz-window').inert,
        'Settings closes');
};
await until(() => document.querySelector('[aria-label="Select Second Mix"]'), 'Routing interface ready');
`;

export const beforeSystemAppearanceReload = `${helpers}
const routingBefore = await window.webkit.messageHandlers.loopz.postMessage({version:1,operation:'state'});
check(matchMedia('(prefers-color-scheme: dark)').matches, 'Fixture starts with an actual dark native window');
await until(() => preference() === 'system' && palette() === 'night-studio', 'New preferences default to following macOS');
check(true, 'Fresh appearance defaults to Match system');
await nativeAppearance('light');
await until(() => palette() === 'atelier', 'Automatic palette changes to Atelier');
check(preference() === 'system', 'A native appearance change does not overwrite the saved choice');
check(getComputedStyle(document.documentElement).colorScheme === 'light', 'Native form controls use the light color scheme');
const lightBackground = getComputedStyle(document.body).backgroundColor;
await openSettings();
await choose('Night Studio', 'night-studio', 'night-studio');
await nativeAppearance('dark');
await nativeAppearance('light');
check(palette() === 'night-studio' && preference() === 'night-studio', 'Explicit Night Studio ignores native appearance changes');
check(getComputedStyle(document.documentElement).colorScheme === 'dark', 'Native form controls follow an explicit dark palette');
await choose('Atelier', 'atelier', 'atelier');
await nativeAppearance('dark');
check(palette() === 'atelier' && preference() === 'atelier', 'Explicit Atelier ignores a native dark window');
await choose('Match system', 'system', 'night-studio');
check(getComputedStyle(document.body).backgroundColor !== lightBackground, 'Actual SingZ palette colors change with native appearance');
await closeSettings();
await nativeAppearance('light');
await until(() => palette() === 'atelier', 'Match system resumes automatic updates');
button('Appearance').click();
await until(() => preference() === 'night-studio' && palette() === 'night-studio', 'Quick appearance toggle uses the resolved light palette');
check(true, 'Quick toggle exits automatic mode with an explicit opposite palette');
await openSettings();
await choose('Match system', 'system', 'atelier');
await closeSettings();
const commands = await window.webkit.messageHandlers.fixture.postMessage({operation:'commands'});
check(commands.length === 0, 'Appearance changes send no routing edits, login or permission actions');
const routingAfter = await window.webkit.messageHandlers.loopz.postMessage({version:1,operation:'state'});
check(JSON.stringify(ordered(routingBefore)) === JSON.stringify(ordered(routingAfter)), 'Canonical routing, revisions and runtime remain identical');
return {checks, preference: preference(), palette: palette(), routingUnchanged: true};
`;

export const afterSystemAppearanceReload = `${helpers}
await until(() => preference() === 'system' && palette() === 'atelier', 'Match system survives native document reload');
check(true, 'Reload retains the system choice rather than a resolved light palette');
await openSettings();
const selected = document.querySelector('[role="dialog"] button[aria-pressed="true"]');
check(selected?.textContent.trim() === 'Match system', 'SingZ control retains the selected automatic mode');
await closeSettings();
await nativeAppearance('dark');
await until(() => palette() === 'night-studio', 'Reconstructed document subscribes to native appearance changes');
check(preference() === 'system', 'Following native dark after reload preserves automatic choice');
await openSettings();
const commands = await window.webkit.messageHandlers.fixture.postMessage({operation:'commands'});
check(commands.length === 0, 'Reload and native appearance changes do not send system actions');
return {checks, preference: preference(), palette: palette()};
`;
