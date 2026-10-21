const helpers = `
const checks = [];
const check = (value, name) => {
    if (!value) throw new Error(JSON.stringify({name, checks}));
    checks.push(name);
};
const until = async (predicate, name) => {
    const deadline = performance.now() + 10000;
    while (!await predicate()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({name, checks,
            body:document.body.innerText.slice(0, 700)}));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const input = label => document.querySelector('[aria-label="' + label + '"]');
const button = text => [...document.querySelectorAll('button')].find(item => item.textContent.trim() === text);
const commands = () => window.webkit.messageHandlers.fixture.postMessage({operation:'commands'});
await until(() => input('Player gain') && input('Headphones gain') && !input('Device gain').matches(':disabled'),
    'Device, source and monitor gain controls ready');
`;

export const beforeGainReload = `${helpers}
for (const label of ['Device gain', 'Player gain', 'Headphones gain']) {
    const control = input(label);
    const position = control.value;
    const text = control.getAttribute('aria-valuetext');
    const count = (await commands()).length;
    control.focus();
    button('Appearance').focus();
    check((await commands()).length === count, label + ' focus/blur leaves saved fractional gain untouched');
    control.focus();
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(control, '-20');
    control.dispatchEvent(new Event('change', {bubbles:true}));
    await until(async () => (await commands()).length === count + 1, label + ' committed change sends one edit');
    await until(() => control.value === position && control.getAttribute('aria-valuetext') === text,
        label + ' rejected edit restores the latest saved value');
    check(true, label + ' rejected save restores the visible and accessible gain');
    await until(() => button('Show current routing before editing'), 'Uncertain save requires review');
    check(control.matches(':disabled'), label + ' cannot continue editing an uncertain save');
    button('Show current routing before editing').click();
    await until(() => !control.matches(':disabled'), 'Fresh state is acknowledged');
    control.focus();
    button('Appearance').focus();
    check((await commands()).length === count + 1, label + ' reset/blur does not resubmit the failed value');
}
const sent = await commands();
check(sent.length === 3 && sent.every(item => item.operation === 'edit'), 'Only the three deliberate gain edits reached the refusing bridge');
check(sent.map(item => item.request.commands[0].type).join(',') === 'setDeviceControls,setSourceControls,setMonitorControls',
    'Device, source and monitor gain commits use their correct command paths');
return {checks, commandCount:sent.length};
`;

export const afterGainReload = `${helpers}
check(input('Device gain').value === '0', 'Reload restores the unchanged saved device gain');
check(input('Player gain').value === '-6' && input('Headphones gain').value === '-6',
    'Reload restores unchanged fractional source and monitor gains');
check((await commands()).length === 3, 'Reload does not replay rejected gain edits');
check(!document.querySelector('[role="dialog"]'), 'Rejected gain drafts do not restore transient UI');
return {checks, commandCount:(await commands()).length};
`;
