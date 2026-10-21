const helpers = `
const checks = [];
const check = (value, name) => { if (!value) throw new Error(JSON.stringify({name,checks})); checks.push(name); };
const until = async (predicate, name) => {
    const deadline = performance.now() + 10000;
    while (!await predicate()) {
        if (performance.now() > deadline) throw new Error(JSON.stringify({name,checks,body:document.body.innerText.slice(0,500)}));
        await new Promise(resolve => setTimeout(resolve,20));
    }
};
const button = label => [...document.querySelectorAll('button')].find(item => item.textContent.trim() === label);
const rate = () => document.querySelector('[aria-label="New device sample rate"]');
const sendFixture = body => window.webkit.messageHandlers.fixture.postMessage(body);
const header = () => document.querySelector('[aria-label="Sample rate"]');
const allRates = [44100,48000,88200,96000,176400,192000];
await until(() => header(), 'Rate controls ready');
`;

export const beforeRateReload = `${helpers}
check([...header().options].map(option => +option.value).join(',') === '44100,48000',
    'An older helper exposes only its compatible two-rate selector');
const envelope = await window.webkit.messageHandlers.loopz.postMessage({version:1,operation:'state'});
envelope.snapshot.runtime.sampleRates = allRates;
const output = envelope.inventory.devices.find(device => device.uid === 'phones');
Object.assign(output,{defaultOutput:true,rate:96000});
envelope.inventory.devicesFresh = true;
await sendFixture({operation:'replace-envelope',envelope});
await until(() => header().options.length === 6, 'New helper capabilities reach the selector');
check([...header().options].map(option => +option.value).join(',') === allRates.join(','),
    'All six engine-advertised sample rates are available');
button('+ New virtual device').click();
await until(() => rate(), 'New-device dialog opens');
check(rate().value === '96000', 'Creation defaults to the current supported hardware output rate');
rate().value = '192000';
rate().dispatchEvent(new Event('change',{bubbles:true}));
delete envelope.snapshot.runtime.sampleRates;
await sendFixture({operation:'replace-envelope',envelope});
await until(() => rate().selectedOptions[0]?.disabled && button('Create device').disabled,
    'An open draft detects a capability downgrade');
check(rate().value === '192000' && rate().selectedOptions[0].textContent.includes('unavailable'),
    'The reviewed draft is retained visibly instead of silently selecting another format');
document.querySelector('[role="dialog"] form').dispatchEvent(new Event('submit',{bubbles:true,cancelable:true}));
await until(() => document.querySelector('[role="dialog"]').textContent.includes('Choose a supported sample rate'),
    'Unsupported programmatic submission is rejected locally');
check((await sendFixture({operation:'commands'})).length === 0, 'The downgraded draft sends no edit');
rate().value = '44100';
rate().dispatchEvent(new Event('change',{bubbles:true}));
await until(() => !button('Create device').disabled, 'A supported replacement enables creation');
check(true, 'Users can resolve an unavailable format explicitly');
button('Cancel').click();
await until(() => !rate(), 'Draft closes');
envelope.snapshot.runtime.sampleRates = allRates;
envelope.inventory.devicesFresh = false;
await sendFixture({operation:'replace-envelope',envelope});
await until(() => header().options.length === 6, 'High-rate capabilities return');
button('+ New virtual device').click();
await until(() => rate(), 'Second creation draft opens');
check(rate().value === '48000', 'Stale default-output metadata falls back to a supported default');
button('Cancel').click();
await until(() => !rate(), 'No transient draft remains');
return {checks};
`;

export const afterRateReload = `${helpers}
check(header().options.length === 6, 'Capabilities are read from the helper after document reload');
check(!document.querySelector('[role="dialog"]'), 'Rate drafts are not restored on reload');
check((await sendFixture({operation:'commands'})).length === 0, 'Creation previews and reload send no routing edits');
button('+ New virtual device').click();
await until(() => rate(), 'Creation controls reopen');
for (const animation of document.querySelector('.modal-scrim').getAnimations({subtree:true})) animation.finish();
const dialog = document.querySelector('[role="dialog"]');
check(dialog.scrollWidth <= dialog.clientWidth, 'Name, rate and channel controls fit the actual SingZ modal');
return {checks};
`;
