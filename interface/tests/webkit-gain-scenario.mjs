export const gainScenario = `
{
    const gain = () => document.querySelector('[aria-label="Device gain"]');
    const valueOf = snapshot => snapshot.snapshot.document.devices[0].routing.gain;
    const revisionOf = snapshot => BigInt(snapshot.snapshot.revision.generation);
    const amplitude = db => db <= -60 ? 0 : Math.fround(10 ** (db / 20));
    const ready = async () => {
        await until(() => gain() && !gain().matches(':disabled'), 'Gain control ready');
        gain().focus();
    };
    const changePosition = async (db, committed = false, inputEvent = true) => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(gain(), String(db));
        if (inputEvent) {
            gain().dispatchEvent(new Event('input', {bubbles:true}));
            await new Promise(resolve => setTimeout(resolve, 0));
        }
        if (committed) gain().dispatchEvent(new Event('change', {bubbles:true}));
    };
    const savedOnce = async (before, db, description) => {
        await until(async () => valueOf(await state()) === amplitude(db), description);
        const after = await state();
        check(revisionOf(after) === revisionOf(before) + 1n, description + ' commits once');
        const disk = await window.webkit.messageHandlers.microphoneFixture.postMessage({operation:'saved-device-gains'});
        check(disk.fromPrimary && disk.devices[0].gain === amplitude(db), description + ' reaches the primary saved file');
        await until(() => !gain().matches(':disabled') && +gain().value === db, 'Saved gain reaches the control');
        return after;
    };
    await ready();
    let before = await state();
    await changePosition(-6, true);
    await savedOnce(before, -6, 'Semantic input/change saves without pointer/key release or blur');

    before = await state();
    gain().dispatchEvent(new Event('change', {bubbles:true}));
    gain().dispatchEvent(new PointerEvent('pointerup', {bubbles:true}));
    button('Appearance').focus();
    check(revisionOf(await state()) === revisionOf(before), 'Duplicate committed, release and blur events do not add history');

    await ready();
    before = await state();
    gain().dispatchEvent(new PointerEvent('pointerdown', {bubbles:true}));
    await changePosition(-8);
    await changePosition(-9);
    check(revisionOf(await state()) === revisionOf(before), 'Pointer input drafts do not save each intermediate position');
    gain().dispatchEvent(new PointerEvent('pointerup', {bubbles:true}));
    await savedOnce(before, -9, 'Pointer release saves the final position');

    await ready();
    before = await state();
    gain().dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowLeft', bubbles:true}));
    await changePosition(-10, true);
    gain().dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowLeft', repeat:true, bubbles:true}));
    await changePosition(-12, true);
    check(revisionOf(await state()) === revisionOf(before), 'Held-key change events retain one uncommitted gesture');
    gain().dispatchEvent(new KeyboardEvent('keyup', {key:'ArrowLeft', bubbles:true}));
    await savedOnce(before, -12, 'Key release commits the final repeated-key position');

    await ready();
    before = await state();
    gain().dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowRight', bubbles:true}));
    await changePosition(-13, true);
    button('Appearance').focus();
    await savedOnce(before, -13, 'Focus loss commits an unfinished key gesture');

    await ready();
    before = await state();
    await changePosition(-15, true, false);
    await savedOnce(before, -15, 'Change-only commit works without an input event');

    await ready();
    before = await state();
    const observed = [];
    const observe = event => observed.push({key:event.key, trusted:event.isTrusted});
    gain().addEventListener('keydown', observe);
    check(!(await nativeKey('ArrowLeft')).menuHandled, 'Native range key goes to the owned WebKit window');
    await savedOnce(before, -16, 'Actual native left arrow saves one gain step');
    check(observed.length === 1 && observed[0].key === 'ArrowLeft' && observed[0].trusted,
        'WebKit receives a trusted native range key');
    gain().removeEventListener('keydown', observe);
    await ready();
    before = await state();
    await nativeKey('ArrowRight');
    await savedOnce(before, -15, 'Actual native right arrow restores one gain step');

    // An external client can save a gain that does not lie on the UI's dB steps.
    const external = await state();
    await window.webkit.messageHandlers.loopz.postMessage({version:1, operation:'edit', request:{
        version:1, expected:external.snapshot.revision, commands:[{type:'setDeviceControls',
            device:external.snapshot.document.devices[0].id, controls:{gain:0.5}}],
    }});
    await until(async () => valueOf(await state()) === 0.5, 'Fractional external gain saved');
    await until(() => !gain().matches(':disabled') && +gain().value === -6, 'Fractional gain appears at its nearest slider position');
    before = await state();
    await ready();
    button('Appearance').focus();
    const unchanged = await state();
    check(valueOf(unchanged) === 0.5 && revisionOf(unchanged) === revisionOf(before),
        'Focus and blur preserve fractional saved gain without a spurious edit');

    await until(() => !gain().matches(':disabled'), 'Gain ready while another control has focus');
    check(document.activeElement !== gain(), 'The final semantic change starts without slider focus');
    before = await state();
    await changePosition(0, true);
    await savedOnce(before, 0, 'Restore unity gain');
    await clickLabel('Undo routing change');
    await until(async () => valueOf(await state()) === 0.5, 'Undo gain');
    await clickLabel('Redo routing change');
    await until(async () => valueOf(await state()) === 1, 'Redo gain');
    check(true, 'Gain commit is one undoable durable routing edit');
}
`;
