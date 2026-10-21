const helpers = `
const checks = [];
const check = (value, name) => {
    if (!value) throw new Error(JSON.stringify({ name, checks }));
    checks.push(name);
};
const until = async (predicate, name) => {
    const deadline = performance.now() + 10000;
    while (!(await predicate())) {
        if (performance.now() > deadline)
            throw new Error(
                JSON.stringify({ name, checks, body: document.body.innerText.slice(-900) }),
            );
        await new Promise((r) => setTimeout(r, 20));
    }
};
const button = (label) =>
    [...document.querySelectorAll("button")].find((node) => node.textContent.trim() === label);
const pair = (start) =>
    document.querySelector(
        '[aria-label="Remove output channels ' + start + "–" + (start + 1) + '"]',
    );
const fixture = (body) => window.webkit.messageHandlers.fixture.postMessage(body);
const state = () =>
    window.webkit.messageHandlers.loopz.postMessage({ version: 1, operation: "state" });
await until(
    () => document.querySelector('[aria-label="Configure output channels"]'),
    "Routing controls ready",
);
`;

export const beforePairReload = `${helpers}

check(!pair(1), "Older helpers expose no unsupported pair-removal action");
const envelope = await state();
envelope.snapshot.runtime.editCapabilities = { removeOutputPair: true };
const mix = envelope.snapshot.document.devices[0];
mix.routing.outputChannels = 8;
mix.routing.sources.push({
    id: "pass",
    channels: 8,
    gain: 1,
    enabled: true,
    muted: false,
    connections: Array.from({ length: 8 }, (_, ch) => ({
        sourceChannel: ch,
        outputChannel: ch,
        gain: 1,
    })),
});
mix.bindings.push({ id: "pass", kind: "pass-through", selector: "", muteOriginal: false });
const dependent = envelope.snapshot.document.devices[1];
dependent.routing.sources = [
    {
        id: "virtual",
        channels: 8,
        gain: 1,
        enabled: false,
        muted: false,
        connections: [
            { sourceChannel: 2, outputChannel: 0, gain: 0.5 },
            { sourceChannel: 6, outputChannel: 1, gain: 0.25 },
        ],
    },
];
dependent.bindings = [
    { id: "virtual", kind: "virtual-device", selector: "mix", muteOriginal: false },
];
await fixture({ operation: "replace-envelope", envelope });
await until(() => pair(3), "Middle-pair removal is exposed");
document.querySelector('[aria-label="Output send channel 7"]').click();
await until(
    () => document.body.innerText.includes("Channel 7 selected"),
    "Socket selection is visible",
);
const smaller = JSON.parse(JSON.stringify(envelope));
smaller.snapshot.document.devices[0].routing.outputChannels = 6;
smaller.snapshot.document.devices[0].routing.sources[1].channels = 6;
smaller.snapshot.document.devices[0].routing.sources[1].connections =
    smaller.snapshot.document.devices[0].routing.sources[1].connections.slice(0, 6);
smaller.snapshot.document.devices[1].routing.sources[0].channels = 6;
smaller.snapshot.document.devices[1].routing.sources[0].connections =
    smaller.snapshot.document.devices[1].routing.sources[0].connections.slice(0, 1);
await fixture({ operation: "replace-envelope", envelope: smaller });
await until(
    () => !document.body.innerText.includes("Channel 7 selected"),
    "Layout changes clear stale socket selection",
);
check(true, "Renumbering cannot leave a socket from the old layout selected");
await fixture({ operation: "replace-envelope", envelope });
await until(() => pair(7), "Eight channels restored");
pair(3).click();
await until(() => button("Remove pair"), "Pair confirmation opens");
check(
    document.querySelector('[role="dialog"]').textContent.includes("3 connections removed") &&
        document
            .querySelector('[role="dialog"]')
            .textContent.includes("1 other virtual device"),
    "Review includes deleted pass-through wires and disabled dependent sources",
);
await until(
    () => document.activeElement === button("Cancel"),
    "Confirmation initially focuses Cancel",
);
await fixture({ operation: "native-key", key: "Tab", shift: false, command: false });
await until(
    () => document.activeElement === button("Remove pair"),
    "Native Tab reaches explicit pair removal",
);
check(true, "Native Tab reaches explicit pair removal");
await fixture({ operation: "native-key", key: "Tab", shift: false, command: false });
await until(
    () => document.activeElement === button("Cancel"),
    "Native Tab remains trapped in the confirmation",
);
check(true, "Native Tab remains trapped in the confirmation");
dependent.routing.sources[0].connections[0].gain = 0.75;
await fixture({ operation: "replace-envelope", envelope });
await until(
    () => button("Remove pair").disabled,
    "Changed dependent wiring invalidates confirmation",
);
check(
    document.querySelector('[role="dialog"]').textContent.includes("Routing changed"),
    "Changed preview explains how to review again",
);
button("Cancel").click();
await until(() => !document.querySelector('[role="dialog"]'), "Stale review closes");
pair(3).click();
await until(
    () => button("Remove pair") && !button("Remove pair").disabled,
    "Updated review is actionable",
);
delete envelope.snapshot.runtime.editCapabilities;
await fixture({ operation: "replace-envelope", envelope });
await until(
    () => button("Remove pair").disabled,
    "Helper downgrade disables the open confirmation",
);
check(
    (await fixture({ operation: "commands" })).length === 0,
    "All previews and capability changes send no edit",
);
envelope.snapshot.runtime.editCapabilities = { removeOutputPair: true };
await fixture({ operation: "replace-envelope", envelope });
await until(() => !button("Remove pair").disabled, "Compatible helper restores the action");
button("Remove pair").focus();
await fixture({ operation: "native-key", key: "Enter", shift: false, command: false });
await until(
    async () =>
        (await fixture({ operation: "commands" })).some((value) => value.operation === "edit"),
    "One removal reaches the refusing bridge",
);
const edits = (await fixture({ operation: "commands" })).filter(
    (value) => value.operation === "edit",
);
check(
    edits.length === 1 &&
        edits[0].request.commands.length === 1 &&
        edits[0].request.commands[0].type === "removeOutputPair" &&
        edits[0].request.commands[0].firstChannel === 2,
    "Only the reviewed zero-based middle pair is submitted",
);
await until(() => button("Remove pair").disabled, "Rejected removal requires review");
check(
    !!document.querySelector('[role="dialog"]') &&
        (await state()).snapshot.document.devices[0].routing.outputChannels === 8,
    "Rejected removal keeps the confirmation and saved layout",
);
button("Cancel").click();
await until(() => !document.querySelector('[role="dialog"]'), "Rejected review closes");
return { checks };
`;

export const afterPairReload = `${helpers}

check(
    !document.querySelector('[role="dialog"]'),
    "Reload restores no destructive confirmation draft",
);
check(
    (await fixture({ operation: "commands" })).filter((value) => value.operation === "edit")
        .length === 1,
    "Reload does not replay the rejected removal",
);
if (document.documentElement.dataset.szPalette !== "atelier") button("Appearance").click();
await until(
    () => document.documentElement.dataset.szPalette === "atelier",
    "Pair confirmation also uses the actual light SingZ palette",
);
pair(3).click();
await until(() => button("Remove pair"), "Fresh review opens after reload");
for (const animation of document.querySelector(".modal-scrim").getAnimations({ subtree: true }))
    animation.finish();
const dialog = document.querySelector('[role="dialog"]');
check(
    dialog.scrollWidth <= dialog.clientWidth,
    "Removal explanation and controls fit the actual SingZ modal",
);
check(!button("Remove pair").disabled, "A fresh review remains usable after reload");
return { checks };
`;
