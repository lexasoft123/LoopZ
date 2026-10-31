const helpers = `
const checks = [];
const check = (value, name) => { if (!value) throw Error(name); checks.push(name); };
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const native = (operation, value) => window.webkit.messageHandlers.fixture.postMessage({operation, ...(value ? {value} : {})});
const status = () => native('visibility-state');
const until = async (condition, name) => {
    const deadline = performance.now() + 5000;
    while (!(await condition())) {
        if (performance.now() > deadline) throw Error(name + ': ' + JSON.stringify(await status()));
        await wait(20);
    }
    check(true, name);
};
const resumed = async name => {
    const before = (await status()).reads;
    await native('visibility', name);
    await until(async () => document.documentElement.dataset.loopzUiVisible === 'true' && (await status()).reads > before, name + ' refreshes immediately');
};
const paused = async name => {
    await native('visibility', name);
    await until(() => document.documentElement.dataset.loopzUiVisible === 'false', name + ' reaches actual native hidden/occluded state');
    await wait(180);
    const before = (await status()).reads;
    await wait(450);
    check((await status()).reads === before, name + ' stops state reads');
    const meter = document.querySelector('.lz-level-fill');
    check(meter && getComputedStyle(meter).transitionDuration === '0s', name + ' pauses meter transitions');
};
`;

export const beforeVisibilityReload =
    helpers +
    `
await until(async () => (await status()).reads >= 2, 'visible UI is polling');
await paused('hide');
await resumed('show');
await paused('minimize');
await resumed('restore');
await paused('cover');
await resumed('uncover');
await native('visibility', 'partial-cover');
await until(() => document.documentElement.dataset.loopzUiVisible === 'true', 'partially visible window stays active');
const count = (await status()).reads;
await wait(350);
check((await status()).reads > count, 'partially covered meters keep updating');
await native('visibility', 'uncover');
const commands = await native('commands');
check(!commands.some(c => ['edit','initialize','retry','device-buffer'].includes(c.operation)), 'visibility never edits routing or changes hardware');
return {checks};
`;

export const afterVisibilityReload =
    helpers +
    `
await until(() => document.documentElement.dataset.loopzUiVisible === 'true', 'reload gets current native visibility');
const count = (await status()).reads;
await until(async () => (await status()).reads > count, 'reload resumes state reads');
await paused('cover');
await resumed('uncover');
return {checks};
`;
