import { sourceCondition, monitorCondition } from "./connection-state.js";
import { normalizeAppearance } from "./appearance.js";
import { supportedSampleRates, initialDeviceRate } from "./sample-rates.js";
import { outputPairRemoval } from "./output-pairs.js";

export const dbToAmplitude = (db) => (db <= -60 ? 0 : 10 ** (db / 20));
export const amplitudeToDb = (gain) =>
    gain > 0 ? Math.max(-60, 20 * Math.log10(gain)) : -60;
export const gainText = (db) =>
    db <= -60 ? "−∞ dB" : `${Math.round(db * 10) / 10} dB`;
export const selected = (state) =>
    state.devices.find((d) => d.id === state.selected);
const kinds = {
    application: "app",
    "input-device": "hardware",
    "virtual-device": "virtual",
    "pass-through": "pass",
};
const nativeKinds = Object.fromEntries(
    Object.entries(kinds).map(([a, b]) => [b, a]),
);
const cachedName = (names, key) =>
    names && Object.hasOwn(names, key) && typeof names[key] === "string"
        ? names[key]
        : undefined;

function routes(connections, count, monitor = false) {
    return Array.from({ length: count }, (_, channel) =>
        connections
            .filter(
                (wire) =>
                    (monitor ? wire.outputChannel : wire.sourceChannel) ===
                    channel,
            )
            .map(
                (wire) =>
                    (monitor ? wire.sourceChannel : wire.outputChannel) + 1,
            )
            .sort((a, b) => a - b),
    );
}

export function project(
    envelope,
    preferences = {},
    flags = {},
    cachedDevices = null,
) {
    const snapshot = envelope?.snapshot;
    const document = snapshot?.document;
    const inventory = envelope?.inventory || { applications: [], devices: [] };
    const rawDevices = document?.devices || [];
    const devices =
        cachedDevices ||
        rawDevices.map((raw) => ({
            id: raw.id,
            name: raw.name,
            channels: raw.routing.outputChannels,
            rate: raw.sampleRate,
            enabled: raw.routing.enabled,
            muted: raw.routing.muted,
            gain: amplitudeToDb(raw.routing.gain),
            limiter: raw.routing.safetyLimiter,
            sources: raw.routing.sources.map((source) => {
                const binding = raw.bindings.find((b) => b.id === source.id);
                const kind = kinds[binding.kind];
                const name =
                    kind === "pass"
                        ? "Pass-through"
                        : kind === "virtual"
                          ? rawDevices.find((d) => d.id === binding.selector)
                                ?.name || "Missing virtual device"
                          : kind === "app"
                            ? inventory.applications.find(
                                  (a) => a.bundle === binding.selector,
                              )?.name ||
                              cachedName(
                                  preferences.appNames,
                                  binding.selector,
                              ) ||
                              binding.selector
                            : binding.selector === "default"
                              ? "Default input"
                              : inventory.devices.find(
                                    (d) => d.uid === binding.selector,
                                )?.name ||
                                cachedName(
                                    preferences.endpointNames,
                                    binding.selector,
                                ) ||
                                binding.selector;
                return {
                    ...source,
                    gain: amplitudeToDb(source.gain),
                    owner: raw.id,
                    name,
                    kind,
                    selector: binding.selector,
                    deviceId: kind === "virtual" ? binding.selector : undefined,
                    muteOriginal: binding.muteOriginal,
                    routes: routes(source.connections, source.channels),
                };
            }),
            monitors: raw.monitors.map((monitor) => ({
                ...monitor,
                owner: raw.id,
                selector: monitor.deviceSelector,
                name:
                    inventory.devices.find(
                        (d) => d.uid === monitor.deviceSelector,
                    )?.name ||
                    cachedName(
                        preferences.endpointNames,
                        monitor.deviceSelector,
                    ) ||
                    monitor.deviceSelector,
                gain: amplitudeToDb(monitor.gain),
                routes: routes(monitor.connections, monitor.channels, true),
            })),
        }));
    const chosen = devices.some((d) => d.id === preferences.selected)
        ? preferences.selected
        : devices[0]?.id || null;
    const connected = envelope?.connection?.connected === true;
    const transportReady = envelope?.connection?.ready === true;
    const phase = snapshot?.restoration?.phase;
    const service = !connected
        ? "offline"
        : [
                "no-document",
                "waiting-for-driver",
                "reconciling",
                "retry",
            ].includes(phase)
          ? "driver"
          : phase === "restored"
            ? "ready"
            : "applying";
    const monitorFaults = {};
    for (const m of devices.find((d) => d.id === chosen)?.monitors || []) {
        const status = monitorCondition(
            {
                connected,
                transportReady,
                meterExpired: flags.meterExpired,
                service,
                pending:
                    snapshot?.restoration?.desiredGeneration !==
                    snapshot?.restoration?.activeGeneration,
                snapshot,
            },
            m,
        );
        monitorFaults[m.id] = status.available ? "" : status.label;
    }
    return {
        ...flags,
        devices,
        selected: chosen,
        theme: normalizeAppearance(preferences.theme),
        sampleRates: supportedSampleRates(snapshot),
        canRemoveOutputPair:
            snapshot?.runtime?.editCapabilities?.removeOutputPair === true,
        inventory,
        snapshot,
        envelope,
        service,
        transportReady,
        connected,
        history: {
            supported:
                typeof snapshot?.history?.canUndo === "boolean" &&
                typeof snapshot?.history?.canRedo === "boolean",
            canUndo: snapshot?.history?.canUndo === true,
            canRedo: snapshot?.history?.canRedo === true,
        },
        canEdit:
            transportReady &&
            !!document &&
            !flags.meterExpired &&
            !flags.saving &&
            !flags.uncertain,
        canCreate:
            transportReady &&
            !document &&
            snapshot?.canCreateDocument === true &&
            !flags.meterExpired &&
            !flags.saving &&
            !flags.uncertain,
        pending:
            snapshot?.restoration?.desiredGeneration !==
            snapshot?.restoration?.activeGeneration,
        monitorFaults,
        recovery: "",
        notice: "",
        error:
            flags.error ||
            (flags.uncertain
                ? "Review current routing before making another change."
                : ""),
        permissions: snapshot?.runtime?.permissions || {},
        permissionSetup: envelope?.permissionSetup || {},
        installation: envelope?.installation || null,
        background: envelope?.background || {},
    };
}

function telemetry(state, device) {
    const current =
        state.connected &&
        state.transportReady &&
        !state.meterExpired &&
        state.snapshot?.runtime?.metersFresh
            ? state.snapshot.runtime.devices?.find((d) => d.id === device)
            : null;
    return current?.active ? current : null;
}
function level(meter, channel) {
    const value = meter?.valid && meter.peak?.[channel - 1];
    return Number.isFinite(value) ? Math.min(1, Math.max(0, value)) : 0;
}
export const outputAmplitude = (state, device, channel) =>
    level(telemetry(state, device)?.meter, channel);
export const sourceAmplitude = (state, source, channel) => {
    const current = telemetry(state, source.owner)?.sources?.find(
        (s) => s.id === source.id,
    );
    return current?.online &&
        sourceCondition(state, source).kind !== "permission"
        ? level(current.meter, channel)
        : 0;
};
export const monitorAmplitude = (state, monitor, channel) => {
    if (!state.connected || !state.transportReady || state.meterExpired)
        return 0;
    return level(
        state.snapshot?.runtime?.monitors?.find(
            (m) => m.device === monitor.owner && m.monitor === monitor.id,
        )?.meter,
        channel,
    );
};
export function sourceStatus(state, source) {
    return sourceCondition(state, source).label;
}
export function sourceLive(state, source) {
    return !!telemetry(state, source.owner)?.sources?.find(
        (s) => s.id === source.id,
    )?.online;
}
export function health(state, device) {
    if (!state.connected) return "Reconnecting";
    if (!state.transportReady || state.meterExpired)
        return "Status unavailable";
    if (!device.enabled) return "Disabled";
    if (state.service === "driver") return "Waiting for audio service";
    if (state.pending) return "Applying changes";
    if (device.sources.some((s) => !sourceCondition(state, s).available))
        return "Source waiting";
    if (
        state.snapshot?.runtime?.monitors?.some(
            (m) =>
                m.device === device.id &&
                !m.complete &&
                device.monitors.find((d) => d.id === m.monitor)?.enabled,
        )
    )
        return "Monitor waiting";
    return "Routing";
}
export function dependencies(state, deviceId) {
    return state.devices.flatMap((d) =>
        d.sources
            .filter((s) => s.kind === "virtual" && s.deviceId === deviceId)
            .map((s) => ({ device: d.id, name: d.name, source: s.id })),
    );
}
export function affectedWires(device, count) {
    return (
        device.sources.reduce(
            (n, s) =>
                n +
                s.connections.filter((w) => w.outputChannel >= count).length,
            0,
        ) +
        device.monitors.reduce(
            (n, m) =>
                n +
                m.connections.filter((w) => w.sourceChannel >= count).length,
            0,
        )
    );
}
export function wouldCycle(devices, from, target, seen = new Set()) {
    if (from === target) return true;
    if (seen.has(from)) return false;
    seen.add(from);
    return (
        devices
            .find((d) => d.id === from)
            ?.sources.some(
                (s) =>
                    s.kind === "virtual" &&
                    wouldCycle(devices, s.deviceId, target, seen),
            ) || false
    );
}

export const endpointLayout = (node) =>
    JSON.stringify([node.selector, node.channels, node.connections]);

export function endpointChoices(state, device, node, kind, extraApp) {
    let items;
    if (kind === "monitor")
        items = state.inventory.devices
            .filter((item) => item.outputChannels > 0)
            .map((item) => ({
                selector: item.uid,
                name: item.name,
                channels: Math.min(64, item.outputChannels),
            }));
    else if (node.kind === "app")
        items = [
            ...state.inventory.applications,
            ...(extraApp ? [extraApp] : []),
        ].map((item) => ({
            selector: item.bundle,
            name: item.name,
            channels: 2,
        }));
    else if (node.kind === "hardware")
        items = [
            {
                selector: "default",
                name: "Default input",
                channels:
                    state.inventory.devices.find((item) => item.defaultInput)
                        ?.inputChannels || null,
            },
            ...state.inventory.devices
                .filter((item) => item.inputChannels > 0)
                .map((item) => ({
                    selector: item.uid,
                    name: item.name,
                    channels: Math.min(64, item.inputChannels),
                })),
        ];
    else if (node.kind === "virtual")
        items = state.devices.map((item) => ({
            selector: item.id,
            name: item.name,
            channels: item.channels,
            blocked: wouldCycle(state.devices, item.id, device.id),
        }));
    else return [];
    const choices = new Map(items.map((item) => [item.selector, item]));
    if (!choices.has(node.selector))
        choices.set(node.selector, {
            selector: node.selector,
            name: `${node.name} · saved, unavailable`,
            channels: null,
            unavailable: true,
        });
    return [...choices.values()];
}

export function endpointWiresRemoved(node, kind, channels) {
    return node.connections.filter(
        (wire) =>
            (kind === "monitor" ? wire.outputChannel : wire.sourceChannel) >=
            channels,
    ).length;
}

export function commandsFor(state, action) {
    const d = state.devices.find(
        (x) => x.id === (action.device || state.selected),
    );
    const source = () => {
        const s = d?.sources.find((s) => s.id === action.id);
        if (!s) throw new Error("This source is no longer in the mix.");
        return s;
    };
    const monitor = () => {
        const m = d?.monitors.find((m) => m.id === action.id);
        if (!m) throw new Error("This monitor is no longer in the mix.");
        return m;
    };
    const controls = (patch) =>
        Object.fromEntries(
            Object.entries(patch)
                .filter(([key]) => ["gain", "enabled", "muted"].includes(key))
                .map(([key, value]) => [
                    key,
                    key === "gain" ? dbToAmplitude(value) : value,
                ]),
        );
    if (action.type === "create") {
        const rate = action.rate ?? initialDeviceRate(state);
        if (!state.sampleRates.includes(rate))
            throw new Error("Choose a supported sample rate.");
        return [
            {
                type: "createDevice",
                id: action.id,
                name: action.name.trim(),
                channels: action.channels,
                sampleRate: rate,
                passThrough: false,
            },
        ];
    }
    if (!d) throw new Error("This device is no longer available.");
    switch (action.type) {
        case "move": {
            const items =
                action.kind === "device"
                    ? state.devices
                    : action.kind === "source"
                      ? d.sources
                      : action.kind === "monitor"
                        ? d.monitors
                        : null;
            if (!items || ![-1, 1].includes(action.offset))
                throw new Error("Choose a valid list movement.");
            const ids = items.map((item) => item.id);
            if (
                action.expectedOrder &&
                JSON.stringify(action.expectedOrder) !== JSON.stringify(ids)
            )
                throw new Error(
                    "The list changed. Review its current order before moving an item.",
                );
            const index = ids.indexOf(action.id),
                target = index + action.offset;
            if (index < 0 || target < 0 || target >= ids.length)
                throw new Error(
                    "This item cannot move further in that direction.",
                );
            [ids[index], ids[target]] = [ids[target], ids[index]];
            return [
                {
                    type:
                        action.kind === "device"
                            ? "reorderDevices"
                            : action.kind === "source"
                              ? "reorderSources"
                              : "reorderMonitors",
                    ...(action.kind === "device" ? {} : { device: d.id }),
                    ids,
                },
            ];
        }
        case "rename":
            return [
                {
                    type: "renameDevice",
                    device: d.id,
                    name: action.name.trim(),
                },
            ];
        case "duplicate":
            return [
                {
                    type: "duplicateDevice",
                    device: d.id,
                    id: action.id,
                    name: action.name,
                },
            ];
        case "delete-device": {
            const refs = dependencies(state, d.id);
            if (refs.length && !action.removeDependencies)
                throw new Error(
                    "Remove dependent virtual sources before deleting this device.",
                );
            return [
                ...refs.map((r) => ({
                    type: "removeSource",
                    device: r.device,
                    source: r.source,
                })),
                { type: "deleteDevice", device: d.id },
            ];
        }
        case "remove-output-pair": {
            if (!state.canRemoveOutputPair)
                throw new Error(
                    "Update LoopZ to remove individual output pairs.",
                );
            const review = outputPairRemoval(state, d.id, action.start);
            if (!action.basis || action.basis !== review.basis)
                throw new Error(
                    "Connections changed. Review the pair before removing it.",
                );
            return [
                {
                    type: "removeOutputPair",
                    device: d.id,
                    firstChannel: action.start - 1,
                },
            ];
        }
        case "channels":
            return [
                { type: "resizeDevice", device: d.id, channels: action.count },
            ];
        case "device":
            return [
                ...(Object.keys(controls(action.patch)).length ||
                "limiter" in action.patch
                    ? [
                          {
                              type: "setDeviceControls",
                              device: d.id,
                              controls: controls(action.patch),
                              ...("limiter" in action.patch
                                  ? { safetyLimiter: action.patch.limiter }
                                  : {}),
                          },
                      ]
                    : []),
                ...("rate" in action.patch
                    ? [
                          {
                              type: "setDeviceRate",
                              device: d.id,
                              sampleRate: action.patch.rate,
                          },
                      ]
                    : []),
            ];
        case "add-source":
            return [
                {
                    type: "addSource",
                    device: d.id,
                    id: action.source.id,
                    kind: nativeKinds[action.source.kind],
                    selector: action.source.selector || "",
                    channels: action.source.channels,
                    muteOriginal: action.source.kind === "app",
                    connectByDefault: false,
                },
            ];
        case "source":
            return [
                {
                    type: "setSourceControls",
                    device: d.id,
                    source: source().id,
                    controls: controls(action.patch),
                    ...("muteOriginal" in action.patch
                        ? { muteOriginal: action.patch.muteOriginal }
                        : {}),
                },
            ];
        case "remove-source":
            return [
                { type: "removeSource", device: d.id, source: source().id },
            ];
        case "source-endpoint": {
            const node = source();
            if (
                action.expectedLayout !== undefined &&
                action.expectedLayout !== endpointLayout(node)
            )
                throw new Error(
                    "Connections changed. Review the selection before applying it.",
                );
            if (node.kind === "pass")
                throw new Error("Pass-through uses this virtual device.");
            if (typeof action.selector !== "string" || !action.selector)
                throw new Error("Choose an application or input device.");
            if (
                node.kind === "virtual" &&
                (!state.devices.some((item) => item.id === action.selector) ||
                    wouldCycle(state.devices, action.selector, d.id))
            )
                throw new Error(
                    "Choose a virtual device that does not create a routing loop.",
                );
            const commands = [
                {
                    type: "setSourceSelector",
                    device: d.id,
                    source: node.id,
                    selector: action.selector,
                },
            ];
            if (node.kind === "hardware") {
                if (
                    !Number.isInteger(action.channels) ||
                    action.channels < 1 ||
                    action.channels > 64
                )
                    throw new Error("Choose 1–64 input channels.");
                commands.push({
                    type: "setInputChannels",
                    device: d.id,
                    source: node.id,
                    channels: action.channels,
                });
            }
            return commands;
        }
        case "add-monitor":
            return [
                {
                    type: "addMonitor",
                    device: d.id,
                    id: action.monitor.id,
                    selector: action.monitor.selector,
                    channels: action.monitor.channels,
                },
            ];
        case "monitor":
            return [
                {
                    type: "setMonitorControls",
                    device: d.id,
                    monitor: monitor().id,
                    controls: controls(action.patch),
                },
            ];
        case "remove-monitor":
            return [
                { type: "removeMonitor", device: d.id, monitor: monitor().id },
            ];
        case "monitor-endpoint": {
            const node = monitor();
            if (
                action.expectedLayout !== undefined &&
                action.expectedLayout !== endpointLayout(node)
            )
                throw new Error(
                    "Connections changed. Review the selection before applying it.",
                );
            if (
                typeof action.selector !== "string" ||
                !action.selector ||
                !Number.isInteger(action.channels) ||
                action.channels < 1 ||
                action.channels > 64
            )
                throw new Error(
                    "Choose an output device and 1–64 monitor channels.",
                );
            return [
                {
                    type: "setMonitorEndpoint",
                    device: d.id,
                    monitor: node.id,
                    selector: action.selector,
                    channels: action.channels,
                },
            ];
        }
        case "route": {
            const m = action.kind === "monitor";
            const node = m ? monitor() : source();
            const from = (m ? action.output : action.channel) - 1;
            const to = (m ? action.channel : action.output) - 1;
            const exists = node.connections.some(
                (w) => w.sourceChannel === from && w.outputChannel === to,
            );
            let connections = node.connections;
            const preceding = [];
            const previous = action.previous;
            let gain = 1;
            if (previous) {
                if (previous.kind !== action.kind)
                    throw new Error("Choose a compatible destination.");
                const oldNode =
                    previous.id === node.id
                        ? node
                        : (m ? d.monitors : d.sources).find(
                              (item) => item.id === previous.id,
                          );
                if (!oldNode)
                    throw new Error(
                        "The original routing block is unavailable.",
                    );
                const oldFrom = (m ? previous.output : previous.channel) - 1;
                const oldTo = (m ? previous.channel : previous.output) - 1;
                const old = oldNode.connections.find(
                    (w) =>
                        w.sourceChannel === oldFrom &&
                        w.outputChannel === oldTo,
                );
                if (!old)
                    throw new Error(
                        "This connection changed. Review the current routing.",
                    );
                gain = old.gain;
                const retained = oldNode.connections.filter((w) => w !== old);
                if (oldNode.id === node.id) connections = retained;
                else
                    preceding.push({
                        type: m ? "setMonitorWiring" : "setSourceWiring",
                        device: d.id,
                        [m ? "monitor" : "source"]: oldNode.id,
                        connections: retained,
                    });
            }
            const remove = action.mode === "remove" || (!action.mode && exists);
            if (remove)
                connections = connections.filter(
                    (w) =>
                        !(w.sourceChannel === from && w.outputChannel === to),
                );
            else if (
                !connections.some(
                    (w) => w.sourceChannel === from && w.outputChannel === to,
                )
            )
                connections = [
                    ...connections,
                    { sourceChannel: from, outputChannel: to, gain },
                ];
            return [
                ...preceding,
                {
                    type: m ? "setMonitorWiring" : "setSourceWiring",
                    device: d.id,
                    [m ? "monitor" : "source"]: node.id,
                    connections,
                },
            ];
        }
        default:
            throw new Error("This action is not available.");
    }
}
