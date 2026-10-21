export const outputPairScenario = `
{
    const target = firstDeviceID;
    const dependentID = "pair-removal-dependent";
    const current = async () => (await state()).snapshot.document;
    const canonical = (value) =>
        JSON.stringify(value, (key, item) =>
            item && typeof item === "object" && !Array.isArray(item)
                ? Object.fromEntries(
                      Object.entries(item).sort(([a], [b]) => a.localeCompare(b)),
                  )
                : item,
        );
    const direct = async (commands) => {
        const before = await state();
        await window.webkit.messageHandlers.loopz.postMessage({
            version: 1,
            operation: "edit",
            request: { version: 1, expected: before.snapshot.revision, commands },
        });
    };
    const disk = () =>
        window.webkit.messageHandlers.microphoneFixture.postMessage({
            operation: "saved-device-gains",
        });
    const count = async () =>
        (await current()).devices.find((device) => device.id === target).routing.outputChannels;
    const remove = (start) =>
        document.querySelector(
            '[aria-label="Remove output channels ' + start + "–" + (start + 1) + '"]',
        );
    const settled = async () => {
        await until(
            async () => (await state()).snapshot.runtime.restored,
            "Pair edit reaches the active private graph",
        );
        await until(
            () => remove(1) && !remove(1).matches(":disabled"),
            "Pair controls are current",
        );
    };
    check(
        (await state()).snapshot.runtime.editCapabilities.removeOutputPair === true,
        "Real Engine advertises individual output-pair editing",
    );
    await direct([
        {
            type: "addSource",
            device: target,
            id: "pass-through",
            kind: "pass-through",
            selector: "",
            channels: 8,
            connectByDefault: true,
        },
        {
            type: "createDevice",
            id: dependentID,
            name: "Pair Removal Dependent",
            channels: 2,
            passThrough: false,
        },
        {
            type: "addSource",
            device: dependentID,
            id: "virtual",
            kind: "virtual-device",
            selector: target,
            connectByDefault: false,
        },
        {
            type: "setSourceWiring",
            device: dependentID,
            source: "virtual",
            connections: [
                { sourceChannel: 2, outputChannel: 0, gain: 0.25 },
                { sourceChannel: 6, outputChannel: 1, gain: 0.75 },
            ],
        },
        {
            type: "setSourceControls",
            device: dependentID,
            source: "virtual",
            controls: { enabled: false, gain: 0.5 },
        },
    ]);
    await until(
        async () => (await current()).devices.length === 2,
        "Private dependent route saved",
    );
    await settled();
    const before = canonical(await current());
    remove(3).click();
    await until(() => button("Remove pair"), "Middle-pair confirmation opens");
    check(
        document.querySelector('[role="dialog"]').textContent.includes("3 connections removed"),
        "Actual saved pass-through and dependent connections are reviewed",
    );
    button("Cancel").click();
    await until(() => !document.querySelector('[role="dialog"]'), "Pair review cancels");
    check(canonical(await current()) === before, "Cancel preserves the entire saved graph");
    remove(3).click();
    await until(
        () => button("Remove pair") && !button("Remove pair").disabled,
        "Reviewed pair is removable",
    );
    const generation = BigInt((await state()).snapshot.revision.generation);
    button("Remove pair").click();
    await until(async () => (await count()) === 6, "Middle-pair removal commits");
    await until(
        () => !document.querySelector('[role="dialog"]'),
        "Committed pair dialog closes",
    );
    let saved = await disk();
    const primary = saved.document.devices.find((device) => device.id === target);
    const dependent = saved.document.devices.find((device) => device.id === dependentID);
    check(
        saved.fromPrimary &&
            primary.routing.outputChannels === 6 &&
            primary.routing.sources[0].channels === 6,
        "Pair removal reaches the actual primary file with a resized pass-through source",
    );
    check(
        dependent.routing.sources[0].channels === 6 &&
            !dependent.routing.sources[0].enabled &&
            dependent.routing.sources[0].gain === 0.5 &&
            dependent.routing.sources[0].connections.length === 1 &&
            dependent.routing.sources[0].connections[0].sourceChannel === 4 &&
            dependent.routing.sources[0].connections[0].outputChannel === 1 &&
            dependent.routing.sources[0].connections[0].gain === 0.75,
        "Saved disabled dependent retains controls and fractional surviving wiring",
    );
    check(
        BigInt((await state()).snapshot.revision.generation) === generation + 1n,
        "The entire pair edit is one durable revision",
    );
    await clickLabel("Undo routing change");
    await until(async () => (await count()) === 8, "Undo restores the removed pair");
    saved = await disk();
    check(
        canonical(saved.document) === before,
        "Undo restores the exact whole graph from before removal",
    );
    await clickLabel("Redo routing change");
    await until(async () => (await count()) === 6, "Redo removes the same middle pair");
    check(
        (await disk()).document.devices.find((device) => device.id === target).routing
            .outputChannels === 6,
        "Redo is durable too",
    );
    await settled();
    remove(1).click();
    await until(
        () => button("Remove pair") && !button("Remove pair").disabled,
        "First pair can be reviewed",
    );
    button("Remove pair").click();
    await until(async () => (await count()) === 4, "First-pair removal commits");
    await until(() => !document.querySelector('[role="dialog"]'), "First-pair dialog closes");
    // Four-channel secondary pairs are normally collapsed. Open the actual
    // disclosure before reviewing its last pair.
    for (const details of document.querySelectorAll(".lz-more-pairs details, .lz-more-pairs"))
        details.open = true;
    await settled();
    remove(3).click();
    await until(
        () => button("Remove pair") && !button("Remove pair").disabled,
        "Last pair can be reviewed",
    );
    button("Remove pair").click();
    await until(async () => (await count()) === 2, "Last removable pair commits");
    await until(() => !document.querySelector('[role="dialog"]'), "Last-pair dialog closes");
    await until(
        () =>
            document.activeElement?.getAttribute("aria-label") === "Configure output channels",
        "Focus returns to the surviving channel control when the invoker was removed",
    );
    check(!remove(1), "The final stereo pair exposes no removal action");
    check(
        (await disk()).document.devices.find((device) => device.id === target).routing
            .outputChannels === 2,
        "First and last pair removals also reach the primary file",
    );
    for (const expected of [4, 6, 8]) {
        await clickLabel("Undo routing change");
        await until(
            async () => (await count()) === expected,
            "Undo restores layout " + expected,
        );
    }
    await direct([{ type: "deleteDevice", device: dependentID }]);
    await until(
        async () => (await current()).devices.length === 1,
        "Private dependent test route removed",
    );
    await until(
        async () => (await state()).snapshot.runtime.restored,
        "Original eight-channel graph restored",
    );
    check(
        (await disk()).document.devices[0].routing.outputChannels === 8,
        "Subsequent acceptance continues from the restored original device",
    );
}
`;
