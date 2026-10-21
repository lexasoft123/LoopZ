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
            name, checks, active: document.activeElement?.outerHTML?.slice(0, 300),
            dialog: document.querySelector('[role="dialog"]')?.getAttribute('aria-label'),
            keys: globalThis.observedKeys, body: document.body.innerText.slice(0, 300),
        }));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
globalThis.observedKeys = [];
window.addEventListener('keydown', event => observedKeys.push({
    key: event.key, trusted: event.isTrusted, command: event.metaKey, shift: event.shiftKey,
}), true);
const nativeKey = async (key, {command = false, shift = false} = {}) => {
    const count = observedKeys.length;
    const result = await window.webkit.messageHandlers.fixture.postMessage({
        operation: 'native-key', key, command, shift,
    });
    check(result.ok, 'Native ' + key + ' accepted by the private fixture');
    await until(() => observedKeys.length > count, 'WebKit received native ' + key);
    check(observedKeys.at(-1).trusted, 'WebKit marks native ' + key + ' as trusted');
    check(observedKeys.at(-1).key === (key === 'Space' ? ' ' : key)
        && observedKeys.at(-1).command === command && observedKeys.at(-1).shift === shift,
        'Native key and modifiers match ' + key);
};
const buttons = () => [...document.querySelectorAll('button')];
const button = label => buttons().find(item => item.textContent.trim() === label);
const controls = dialog => [...dialog.querySelectorAll('button,input,select,summary,a[href]')]
    .filter(item => !item.matches(':disabled') && item.getClientRects().length);
await until(() => document.querySelector('[aria-label="Select Second Mix"]'), 'Initial snapshot');
`;

export const beforeKeyboardReload = `${helpers}
for (const request of [
    {key: 'q', command: true, shift: false},
    {key: 'Tab', command: true, shift: false},
    {key: 'Tab', command: 1, shift: false},
    {key: 'Tab', command: false, shift: false, extra: true},
]) {
    const rejected = await window.webkit.messageHandlers.fixture.postMessage({
        operation: 'native-key', ...request,
    }).then(() => false, () => true);
    check(rejected, 'Private fixture rejects unsupported native key request');
}
const settings = button('Settings');
settings.focus();
await nativeKey('Enter');
await until(() => document.querySelector('[role="dialog"]') && document.querySelector('.lz-window').inert,
    'Settings opened by native Enter');
const dialog = document.querySelector('[role="dialog"]');
check(dialog.getAttribute('aria-label') === 'Settings', 'Native Enter activates the SingZ Settings button');
await nativeKey('n', {command: true});
check(document.querySelector('[role="dialog"]') === dialog,
    'Application shortcuts do not replace an open modal');
const all = controls(dialog);
check(all.length >= 3, 'Settings has multiple keyboard controls');
all[0].focus();
for (let index = 1; index < all.length; index++) {
    await nativeKey('Tab');
    await until(() => document.activeElement === all[index], 'Tab visits every Settings control at ' + index);
}
await nativeKey('Tab');
await until(() => document.activeElement === all[0], 'Native Tab wraps to first modal control');
await nativeKey('Tab', {shift: true});
await until(() => document.activeElement === all.at(-1), 'Native Shift-Tab wraps to last modal control');
check(dialog.contains(document.activeElement), 'Native tab traversal stays inside the modal');
await nativeKey('Escape');
await until(() => !document.querySelector('[role="dialog"]') && !document.querySelector('.lz-window').inert,
    'Escape dismisses Settings');
await until(() => document.activeElement === settings, 'Focus returns to Settings invoker');
await nativeKey('n', {command: true});
await until(() => document.querySelector('[aria-label="Device name"]'), 'Command-N opens create dialog');
check(document.activeElement.getAttribute('aria-label') === 'Device name', 'Native Command-N focuses the device name');
await nativeKey('Escape');
await until(() => !document.querySelector('[role="dialog"]') && !document.querySelector('.lz-window').inert,
    'Cancel create without editing');
await nativeKey(',', {command: true});
await until(() => document.querySelector('[aria-label="Settings"]') && document.querySelector('.lz-window').inert,
    'Command-comma opens Settings');
check(document.querySelector('[role="dialog"]').contains(document.activeElement), 'Native settings shortcut moves focus into the dialog');
await nativeKey('Escape');
await until(() => !document.querySelector('[role="dialog"]') && !document.querySelector('.lz-window').inert,
    'Dismiss settings shortcut');
const socket = document.querySelector('[data-socket^="source:"]');
check(socket, 'Routing board exposes a keyboard source socket');
socket.focus();
await nativeKey('Space');
await until(() => socket.getAttribute('aria-pressed') === 'true', 'Native Space selects the source socket');
await nativeKey('Escape');
await until(() => socket.getAttribute('aria-pressed') === 'false', 'Native Escape clears the pending connection');
const commands = await window.webkit.messageHandlers.fixture.postMessage({operation: 'commands'});
check(commands.length === 0, 'Keyboard navigation sends no edits, permission requests or system actions');
return {checks, observedKeys};
`;

export const afterKeyboardReload = `${helpers}
check(!document.querySelector('[role="dialog"]'), 'Reload does not reopen transient keyboard dialogs');
const settings = button('Settings');
settings.focus();
await nativeKey('Space');
await until(() => document.querySelector('[aria-label="Settings"]') && document.querySelector('.lz-window').inert,
    'Space activates Settings after reload');
await nativeKey('Escape');
await until(() => !document.querySelector('[role="dialog"]') && document.activeElement === settings,
    'Reloaded native focus restoration');
return {checks, observedKeys};
`;
