export function persistenceScenario(phase) {
    return `
const checks = [];
const check = (value, message) => {
    if (!value) throw new Error(message);
    checks.push(message);
};
const until = async (predicate, message) => {
    const deadline = performance.now() + 10000;
    while (!predicate()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({message, checks,
            initial: globalThis.__fixtureInitialPreference,
            stored: localStorage.getItem('loopz-appearance'),
            palette: document.documentElement.dataset.szPalette,
        }));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const phase = ${JSON.stringify(phase)};
check(!globalThis.__fixtureInitialPreference?.error, 'Local storage is available at document start');
const initial = globalThis.__fixtureInitialPreference.stored;
if (phase.fresh) {
    check(initial === null, 'Unique application identity begins with no appearance preferences');
} else {
    check(initial !== null, 'Preferences were read before the new interface could write them');
    const saved = JSON.parse(initial);
    check(saved.theme === phase.expectedTheme, 'Saved appearance survives complete application process exit');
    check(saved.selected === 'second', 'Saved device selection survives complete application process exit');
}
await until(() => document.querySelector('[aria-label="Select Second Mix"]'), 'Interface is ready');
const expectedPalette = phase.expectedTheme === 'system'
    ? (phase.native === 'dark' ? 'night-studio' : 'atelier') : phase.expectedTheme;
await until(() => document.documentElement.dataset.szPalette === expectedPalette, 'Saved appearance resolves on startup');
check(matchMedia('(prefers-color-scheme: dark)').matches === (phase.native === 'dark'),
    'WebKit sees the requested private native appearance');
const expectedSelection = phase.fresh ? 'Mix' : 'Second Mix';
check(document.querySelector('[aria-label="Select ' + expectedSelection + '"]').getAttribute('aria-current') === 'true',
    'The expected device is selected after startup');
const button = text => [...document.querySelectorAll('button')].find(item => item.textContent.trim() === text);
if (phase.writeTheme) {
    document.querySelector('[aria-label="Select Second Mix"]').click();
    button('Settings').click();
    await until(() => document.querySelector('[role="dialog"][aria-label="Settings"]'), 'Settings opens');
    const labels = {system:'Match system', atelier:'Atelier', 'night-studio':'Night Studio'};
    const group = document.querySelector('[role="group"][aria-label="Appearance"]');
    const choice = [...group.querySelectorAll('button')].find(item => item.textContent.trim() === labels[phase.writeTheme]);
    check(!!choice, 'The preference is chosen through the actual SingZ control');
    choice.click();
    await until(() => JSON.parse(localStorage.getItem('loopz-appearance')).theme === phase.writeTheme
        && JSON.parse(localStorage.getItem('loopz-appearance')).selected === 'second', 'Preferences save locally');
    button('Done').click();
    await until(() => !document.querySelector('[role="dialog"]'), 'Settings closes');
}
return {checks, initialPreference: initial ? JSON.parse(initial) : null,
    finalPreference: JSON.parse(localStorage.getItem('loopz-appearance')),
    palette: document.documentElement.dataset.szPalette,
    nativeDark: matchMedia('(prefers-color-scheme: dark)').matches};
`;
}
