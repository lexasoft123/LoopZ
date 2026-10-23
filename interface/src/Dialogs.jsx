import PassThroughHelp from "./components/PassThroughHelp.jsx";
import { AudioSetup } from "./AudioSetup.jsx";
import { t } from "./i18n.js";
import { Language } from "./components/Language.jsx";
import { ReleaseUpdates } from "./ReleaseUpdates.jsx";
import { Switch } from "./components/Switch.jsx";
import React, { useEffect, useLayoutEffect, useRef, useState } from "react";
import {
    Button,
    Chip,
    SegmentedControl,
    Modal,
    ModalActions,
    Banner,
    Badge,
} from "@singz/ui";
import { affectedWires, dependencies, wouldCycle } from "./model.js";
import { newIdentity } from "./identity.js";
import EndpointEditor from "./EndpointEditor.jsx";
import OrderControls from "./OrderControls.jsx";
import { microphoneAccess, sourceCondition } from "./connection-state.js";
import { appearanceOptions } from "./appearance.js";
import { initialDeviceRate, sampleRateLabel } from "./sample-rates.js";
import OutputPairRemoval from "./OutputPairRemoval.jsx";
import HardwareBufferSettings from "./HardwareBufferSettings.jsx";
import { installationComponents } from "./installation.js";
import { windowsMicrophoneStatus } from "./windows-microphones.js";
function ModalFocus({ returnFocus, kind }) {
    const currentKind = useRef(kind);
    useEffect(() => {
        const frame = document.querySelector(".lz-window");
        if (frame) frame.inert = true;
        const trap = (event) => {
            if (event.key !== "Tab") return;
            const box = document.querySelector('[role="dialog"]');
            const items = [
                ...(box?.querySelectorAll(
                    "button,input,select,summary,a[href]",
                ) || []),
            ].filter(
                (el) => !el.matches(":disabled") && el.getClientRects().length,
            );
            if (!items.length) return;
            const [first, last] = [items[0], items.at(-1)];
            if (event.shiftKey && document.activeElement === first) {
                event.preventDefault();
                last.focus();
            }
            if (!event.shiftKey && document.activeElement === last) {
                event.preventDefault();
                first.focus();
            }
        };
        document.addEventListener("keydown", trap);
        return () => {
            if (frame) frame.inert = false;
            document.removeEventListener("keydown", trap);
            queueMicrotask(() => {
                if (!document.querySelector('[role="dialog"]')) {
                    const target = returnFocus.current?.isConnected
                        ? returnFocus.current
                        : currentKind.current === "removePair"
                          ? document.querySelector(
                                '[aria-label="Configure output channels"]',
                            )
                          : null;
                    target?.focus({
                        preventScroll: true,
                    });
                }
            });
        };
    }, []);
    useLayoutEffect(() => {
        currentKind.current = kind;
        const box = document.querySelector('[role="dialog"]');
        (
            box?.querySelector(
                "input:not([type=checkbox]):not(:disabled),select:not(:disabled)",
            ) || box?.querySelector("button:not(:disabled)")
        )?.focus();
    }, [kind]);
    return null;
}
export default function Dialogs({
    releaseUpdates,
    modal,
    state,
    send,
    close,
    setModal,
    returnFocus,
    transport,
    refresh,
    startTour,
}) {
    const d = state.devices.find((device) => device.id === modal.device);
    const source = d?.sources.find((s) => s.id === modal.id);
    const sourceState = source ? sourceCondition(state, source) : null;
    const monitor = d?.monitors.find((m) => m.id === modal.id);
    const [name, setName] = useState(
        modal.kind === "rename" ? d?.name || "" : t("Untitled Mix"),
    );
    const [count, setCount] = useState(d?.channels || 2);
    const [rate, setRate] = useState(() => initialDeviceRate(state));
    const [query, setQuery] = useState("");
    const [extraApp, setExtraApp] = useState(null);
    const [localError, setLocalError] = useState("");
    const [backgroundBusy, setBackgroundBusy] = useState(false);
    const [backgroundNotice, setBackgroundNotice] = useState("");
    const [microphoneBusy, setMicrophoneBusy] = useState(false);
    const [microphoneNotice, setMicrophoneNotice] = useState("");
    const microphonePending = useRef(false);
    const [nodeId, setNodeId] = useState("");
    const [channel, setChannel] = useState(1);
    useEffect(() => {
        if (modal.kind === "rename") setName(d?.name || "");
        if (modal.kind === "channels") setCount(d?.channels || 2);
        setQuery("");
        setLocalError("");
        setBackgroundNotice("");
    }, [modal.kind]);
    const commit = async (action) => {
        if (
            await send({
                device: modal.device,
                ...action,
            })
        )
            close();
    };
    const openPrivacy = async (pane) => {
        setLocalError("");
        try {
            const result = await transport({
                version: 1,
                operation: "privacy",
                pane,
            });
            if (!result?.ok) throw new Error("Settings did not open.");
        } catch {
            setLocalError(
                "Open Privacy & Security in System Settings to review LoopZ Engine’s access.",
            );
        }
    };
    const requestMicrophone = async () => {
        if (microphonePending.current) return;
        microphonePending.current = true;
        setMicrophoneBusy(true);
        setMicrophoneNotice("");
        setLocalError("");
        try {
            const result = await transport({
                version: 1,
                operation: "request-microphone",
            });
            if (!result?.ok)
                throw new Error(
                    result?.detail || "The microphone request could not start.",
                );
            setMicrophoneNotice(
                result.microphone === "authorized"
                    ? t(
                          "Microphone access is granted. Saved inputs reconnect automatically.",
                      )
                    : !result.completed
                      ? "The microphone request did not finish. Review any macOS dialog, then try again."
                      : "Microphone access is still off. You can review it in Privacy & Security.",
            );
            await refresh();
        } catch (error) {
            setLocalError(
                error.message || "The microphone request could not complete.",
            );
        } finally {
            microphonePending.current = false;
            setMicrophoneBusy(false);
        }
    };
    const backgroundAction = async (operation, fields = {}) => {
        if (backgroundBusy) return;
        setBackgroundBusy(true);
        setLocalError("");
        setBackgroundNotice("");
        try {
            const result = await transport({
                version: 1,
                operation,
                ...fields,
            });
            if (!result?.ok)
                throw new Error(
                    result?.detail ||
                        "The background setting could not be updated.",
                );
            if (result.microphoneSelectionsChanged)
                setBackgroundNotice(
                    t(
                        "Microphones were updated. Select your LoopZ microphones again in recording and calling apps.",
                    ),
                );
            await refresh();
        } catch (error) {
            setLocalError(error.message);
        } finally {
            setBackgroundBusy(false);
        }
    };
    const titles = {
        create:
            state.platform === "windows" ? "New mix" : t("New virtual device"),
        rename: t("Rename virtual device"),
        device: t("Device options"),
        "add-source": t("Add a source"),
        source: source?.name || t("Source options"),
        "add-monitor": t("Add a monitor"),
        monitor: monitor?.name || t("Monitor options"),
        mapping: t("Edit connections"),
        channels: t("Output channels"),
        confirmChannels: "Remove connected channels?",
        removePair: `Remove channels ${modal.start}–${modal.start + 1}?`,
        delete: "Delete this item?",
        settings: t("Settings"),
        shortcuts: t("Keyboard & connections"),
    };
    const writable =
        !state.saving &&
        (state.canEdit || (modal.kind === "create" && state.canCreate));
    const cancel = <Button onClick={close}>{t("Cancel")}</Button>;
    let content, actions;
    if (!["create", "settings", "shortcuts"].includes(modal.kind) && !d)
        content = (
            <p>
                This device is no longer available. Close this dialog to review
                the current routing.
            </p>
        );
    else if (["create", "rename"].includes(modal.kind))
        content = (
            <form
                onSubmit={async (event) => {
                    event.preventDefault();
                    if (
                        modal.kind === "create" &&
                        !state.sampleRates.includes(rate)
                    ) {
                        setLocalError("Choose a supported sample rate.");
                        return;
                    }
                    if (!name.trim()) {
                        setLocalError("Enter a device name.");
                        return;
                    }
                    if (
                        state.devices.some(
                            (x) =>
                                x.name.toLowerCase() ===
                                    name.trim().toLowerCase() &&
                                (modal.kind === "create" || x.id !== d.id),
                        )
                    ) {
                        setLocalError("Choose a unique device name.");
                        return;
                    }
                    await commit(
                        modal.kind === "create"
                            ? {
                                  type: "create",
                                  id: newIdentity(),
                                  name,
                                  channels: count,
                                  rate,
                              }
                            : {
                                  type: "rename",
                                  name,
                              },
                    );
                }}
            >
                <p className="modal-body">
                    {modal.kind === "create"
                        ? t(
                              "Combine apps and inputs into a device you can choose in your recording app.",
                          )
                        : "The new name appears in apps using this device."}
                </p>
                <label className="lz-field">
                    {t("Device name")}
                    <input
                        aria-label={t("Device name")}
                        value={name}
                        onChange={(event) => setName(event.target.value)}
                        maxLength={128}
                        required
                    />
                </label>
                {modal.kind === "create" && (
                    <label className="lz-field">
                        {t("Sample rate")}
                        <select
                            aria-label="New device sample rate"
                            value={rate}
                            onChange={(event) => setRate(+event.target.value)}
                        >
                            {!state.sampleRates.includes(rate) && (
                                <option value={rate} disabled>
                                    {sampleRateLabel(rate)} (unavailable)
                                </option>
                            )}
                            {state.sampleRates.map((rate) => (
                                <option key={rate} value={rate}>
                                    {sampleRateLabel(rate)}
                                </option>
                            ))}
                        </select>
                    </label>
                )}
                {modal.kind === "create" && (
                    <label className="lz-field">
                        {t("Output channels")}
                        <select
                            aria-label="New device channel count"
                            value={count}
                            onChange={(event) => setCount(+event.target.value)}
                        >
                            {Array.from(
                                {
                                    length:
                                        state.platform === "windows" ? 1 : 32,
                                },
                                (_, i) => (i + 1) * 2,
                            ).map((n) => (
                                <option key={n} value={n}>
                                    {n} channels · {n / 2} pairs
                                </option>
                            ))}
                        </select>
                    </label>
                )}
                <ModalActions>
                    {cancel}
                    <Button
                        variant="primary"
                        type="submit"
                        disabled={
                            !writable ||
                            (modal.kind === "create" &&
                                !state.sampleRates.includes(rate))
                        }
                    >
                        {modal.kind === "create"
                            ? t("Create device")
                            : t("Save name")}
                    </Button>
                </ModalActions>
            </form>
        );
    else if (modal.kind === "device")
        content = (
            <div className="lz-dialog-menu">
                <OrderControls
                    state={state}
                    device={d}
                    kind="device"
                    writable={writable}
                    send={send}
                />
                <Button
                    onClick={() =>
                        setModal({
                            kind: "rename",
                        })
                    }
                >
                    {t("Rename device")}
                </Button>
                <Button
                    onClick={() =>
                        setModal({
                            kind: "channels",
                        })
                    }
                >
                    {t("Configure output channels")}
                </Button>
                <Button
                    disabled={!writable}
                    onClick={() =>
                        commit({
                            type: "duplicate",
                            id: newIdentity(),
                            name: `${d.name} Copy`,
                        })
                    }
                >
                    {t("Duplicate device")}
                </Button>
                <Button
                    variant="danger"
                    onClick={() =>
                        setModal({
                            kind: "delete",
                            target: "device",
                        })
                    }
                >
                    {t("Delete device\u2026")}
                </Button>
            </div>
        );
    else if (modal.kind === "source" && source)
        content = (
            <>
                {source.kind === "pass" && <PassThroughHelp expanded />}
                {sourceState && !sourceState.available && (
                    <Banner tone="warning" heading={sourceState.label}>
                        {sourceState.detail}
                    </Banner>
                )}
                <p className="modal-body">
                    Source controls affect this mix. Connections stay saved when
                    a source is disabled or unavailable.
                </p>
                <div className="lz-setting-row">
                    <strong>{t("Include in this mix")}</strong>
                    <Switch
                        checked={source.enabled}
                        label={t("Include source in this mix")}
                        disabled={!writable}
                        onChange={(enabled) =>
                            send({
                                type: "source",
                                device: d.id,
                                id: source.id,
                                patch: {
                                    enabled,
                                },
                            })
                        }
                    />
                </div>
                {source.kind === "app" && state.canRouteAppPlayback && (
                    <div className="lz-setting-row">
                        <div>
                            <strong>{t("Mute original playback")}</strong>
                            <p>
                                {state.platform === "windows"
                                    ? t(
                                          "Route the app’s original output through LoopZ after capture is ready. Turn this off to restore its previous output.",
                                      )
                                    : "Capture the app here while silencing its original output."}
                            </p>
                        </div>
                        <Switch
                            checked={source.muteOriginal}
                            label={t("Mute original app playback")}
                            disabled={!writable}
                            onChange={(muteOriginal) =>
                                send({
                                    type: "source",
                                    device: d.id,
                                    id: source.id,
                                    patch: {
                                        muteOriginal,
                                    },
                                })
                            }
                        />
                    </div>
                )}
                {source.kind !== "pass" && (
                    <EndpointEditor
                        key={`source:${source.id}`}
                        state={state}
                        device={d}
                        node={source}
                        kind="source"
                        writable={writable}
                        send={send}
                        transport={transport}
                        refresh={refresh}
                    />
                )}
                <OrderControls
                    state={state}
                    device={d}
                    node={source}
                    kind="source"
                    writable={writable}
                    send={send}
                />
                <div className="lz-dialog-menu">
                    <Button
                        onClick={() =>
                            setModal({
                                kind: "mapping",
                                node: JSON.stringify(["source", source.id]),
                            })
                        }
                    >
                        {t("Edit channel connections")}
                    </Button>
                    <Button
                        variant="danger"
                        onClick={() =>
                            setModal({
                                kind: "delete",
                                target: "source",
                                id: source.id,
                            })
                        }
                    >
                        {t("Remove source\u2026")}
                    </Button>
                </div>
            </>
        );
    else if (modal.kind === "add-source") {
        const apps = [
            ...state.inventory.applications,
            ...(extraApp &&
            !state.inventory.applications.some(
                (a) => a.bundle === extraApp.bundle,
            )
                ? [extraApp]
                : []),
        ];
        const items = [
            ...apps.map((app) => ({
                name: app.name,
                selector: app.bundle,
                kind: "app",
                channels: 2,
                group: t("Applications"),
            })),
            ...state.inventory.devices
                .filter(
                    (x) =>
                        x.inputChannels > 0 &&
                        x.virtualRole !== "loopz-microphone",
                )
                .map((input) => ({
                    name: input.name,
                    selector: input.uid,
                    kind: "hardware",
                    channels: Math.min(64, input.inputChannels),
                    group: t("Input devices"),
                })),
            ...state.devices
                .filter((x) => x.id !== d.id)
                .map((device) => ({
                    name: device.name,
                    selector: device.id,
                    kind: "virtual",
                    channels: device.channels,
                    group: t("Virtual devices"),
                })),
            ...(!d.sources.some((s) => s.kind === "pass")
                ? [
                      {
                          name: t("Pass-through"),
                          selector: "",
                          kind: "pass",
                          channels: d.channels,
                          group: t("Pass-through"),
                      },
                  ]
                : []),
        ];
        content = (
            <>
                <p className="modal-body">
                    Select an app, input or virtual device. Unavailable sources
                    keep their saved wiring.
                </p>
                {state.inventory.devicesError && (
                    <Banner tone="warning" heading="Device list unavailable">
                        {state.inventory.devicesError}
                    </Banner>
                )}
                <label className="lz-field">
                    <span className="sr-only">Search sources</span>
                    <input
                        aria-label="Search sources"
                        placeholder={t("Search apps and devices\u2026")}
                        value={query}
                        onChange={(event) => setQuery(event.target.value)}
                    />
                </label>
                {state.platform !== "windows" && (
                    <Button
                        size="sm"
                        onClick={async () => {
                            try {
                                const app = await transport({
                                    version: 1,
                                    operation: "choose-application",
                                });
                                if (app) {
                                    setExtraApp(app);
                                    await refresh();
                                }
                            } catch {
                                setLocalError(
                                    "The application could not be selected.",
                                );
                            }
                        }}
                    >
                        {t("Choose another application\u2026")}
                    </Button>
                )}
                {state.platform === "windows" && (
                    <p>
                        {t(
                            state.canRouteAppPlayback
                                ? "Start playback in an application to list it here. You can route its original playback in Source options."
                                : "Start playback in an application to list it here. Capturing an app preserves its original playback.",
                        )}
                    </p>
                )}
                {[
                    t("Applications"),
                    t("Input devices"),
                    t("Virtual devices"),
                    t("Pass-through"),
                ].map((group) => {
                    const groupItems = items.filter(
                        (x) =>
                            x.group === group &&
                            x.name.toLowerCase().includes(query.toLowerCase()),
                    );
                    return (
                        groupItems.length > 0 && (
                            <div className="lz-picker-group" key={group}>
                                <p className="eyebrow">{group}</p>
                                {group === t("Pass-through") && (
                                    <PassThroughHelp expanded />
                                )}
                                {groupItems.map((item) => (
                                    <Button
                                        className="lz-picker-item"
                                        key={`${item.kind}:${item.selector}`}
                                        disabled={
                                            !writable ||
                                            (item.kind === "virtual" &&
                                                wouldCycle(
                                                    state.devices,
                                                    item.selector,
                                                    d.id,
                                                ))
                                        }
                                        onClick={() =>
                                            commit({
                                                type: "add-source",
                                                source: {
                                                    ...item,
                                                    id: newIdentity(),
                                                },
                                            })
                                        }
                                    >
                                        <span>
                                            <strong>{item.name}</strong>
                                            <small>
                                                {item.channels} channels
                                                {item.kind === "virtual" &&
                                                wouldCycle(
                                                    state.devices,
                                                    item.selector,
                                                    d.id,
                                                )
                                                    ? " · Would create a routing loop"
                                                    : ""}
                                            </small>
                                        </span>
                                        <span aria-hidden="true">+</span>
                                    </Button>
                                ))}
                            </div>
                        )
                    );
                })}
                {!items.some((x) =>
                    x.name.toLowerCase().includes(query.toLowerCase()),
                ) && <p>{t("No matching sources.")}</p>}
            </>
        );
    } else if (modal.kind === "channels") {
        content = (
            <>
                <p className="modal-body">
                    Output pairs support 2–64 channels. Valid wiring is
                    preserved.
                </p>
                <label className="lz-field">
                    {t("Channel count")}
                    <select
                        aria-label="Output channel count"
                        value={count}
                        onChange={(event) => setCount(+event.target.value)}
                    >
                        {Array.from(
                            {
                                length: state.platform === "windows" ? 1 : 32,
                            },
                            (_, i) => (i + 1) * 2,
                        ).map((n) => (
                            <option key={n} value={n}>
                                {n} channels · {n / 2} pairs
                            </option>
                        ))}
                    </select>
                </label>
            </>
        );
        actions = (
            <>
                {cancel}
                <Button
                    variant="primary"
                    disabled={!writable}
                    onClick={() => {
                        const wires = affectedWires(d, count);
                        if (count < d.channels && wires)
                            setModal({
                                kind: "confirmChannels",
                                count,
                                wires,
                            });
                        else
                            commit({
                                type: "channels",
                                count,
                            });
                    }}
                >
                    Apply channels
                </Button>
            </>
        );
    } else if (modal.kind === "removePair") {
        content = (
            <OutputPairRemoval
                state={state}
                device={d}
                start={modal.start}
                review={modal.review}
                writable={writable}
                cancel={cancel}
                commit={commit}
            />
        );
    } else if (modal.kind === "confirmChannels") {
        content = (
            <p>
                Changing to {modal.count} channels removes {modal.wires}{" "}
                connections on deleted sockets. Dependent virtual sources follow
                the new channel count.
            </p>
        );
        actions = (
            <>
                {cancel}
                <Button
                    variant="danger"
                    disabled={!writable}
                    onClick={() =>
                        commit({
                            type: "channels",
                            count: modal.count,
                        })
                    }
                >
                    Remove channels
                </Button>
            </>
        );
    } else if (modal.kind === "mapping") {
        const nodes = [
            ...d.sources.map((s) => ({
                ...s,
                routeKind: "source",
            })),
            ...d.monitors.map((m) => ({
                ...m,
                routeKind: "monitor",
            })),
        ];
        const active =
            nodes.find(
                (n) =>
                    JSON.stringify([n.routeKind, n.id]) ===
                    (nodeId || modal.node),
            ) || nodes[0];
        const ch = Math.min(channel, active?.channels || 1);
        content = (
            <>
                <p className="modal-body">
                    Select a channel, then connect it to one or more outputs.
                    Sources on the same output are summed.
                </p>
                {!active ? (
                    <p>Add a source or monitor to edit connections.</p>
                ) : (
                    <>
                        <label className="lz-field">
                            Source or monitor
                            <select
                                aria-label="Connection source"
                                value={JSON.stringify([
                                    active.routeKind,
                                    active.id,
                                ])}
                                onChange={(event) => {
                                    setNodeId(event.target.value);
                                    setChannel(1);
                                }}
                            >
                                {nodes.map((node) => (
                                    <option
                                        key={`${node.routeKind}:${node.id}`}
                                        value={JSON.stringify([
                                            node.routeKind,
                                            node.id,
                                        ])}
                                    >
                                        {node.name} · {node.routeKind}
                                    </option>
                                ))}
                            </select>
                        </label>
                        <label className="lz-field">
                            Channel
                            <select
                                aria-label="Connection channel"
                                value={ch}
                                onChange={(event) =>
                                    setChannel(+event.target.value)
                                }
                            >
                                {Array.from(
                                    {
                                        length: active.channels,
                                    },
                                    (_, i) => (
                                        <option key={i} value={i + 1}>
                                            {i + 1}
                                        </option>
                                    ),
                                )}
                            </select>
                        </label>
                        <div className="lz-route-options">
                            {Array.from(
                                {
                                    length: d.channels,
                                },
                                (_, i) => i + 1,
                            ).map((n) => (
                                <label key={n}>
                                    <input
                                        type="checkbox"
                                        disabled={!writable}
                                        checked={
                                            active.routes[ch - 1]?.includes(
                                                n,
                                            ) || false
                                        }
                                        aria-label={
                                            active.routeKind === "monitor"
                                                ? `Output ${n} to ${active.name} channel ${ch}`
                                                : `${active.name} channel ${ch} to output ${n}`
                                        }
                                        onChange={() =>
                                            send({
                                                type: "route",
                                                device: d.id,
                                                kind: active.routeKind,
                                                id: active.id,
                                                channel: ch,
                                                output: n,
                                            })
                                        }
                                    />
                                    {t("Output") + " "}
                                    {n}
                                </label>
                            ))}
                        </div>
                    </>
                )}
            </>
        );
        actions = <Button onClick={close}>{t("Done")}</Button>;
    } else if (modal.kind === "add-monitor")
        content = (
            <>
                <p className="modal-body">
                    Monitoring keeps a mix active without a recording app.
                    Choose an output device to listen.
                </p>
                {state.inventory.devicesError && (
                    <Banner tone="warning" heading="Device list unavailable">
                        {state.inventory.devicesError}
                    </Banner>
                )}
                <label className="lz-field">
                    <span className="sr-only">Search monitors</span>
                    <input
                        aria-label="Search monitors"
                        placeholder={t("Search output devices\u2026")}
                        value={query}
                        onChange={(event) => setQuery(event.target.value)}
                    />
                </label>
                {state.inventory.devices
                    .filter(
                        (x) =>
                            x.outputChannels > 0 &&
                            x.virtualRole !== "loopz-feed" &&
                            x.name.toLowerCase().includes(query.toLowerCase()),
                    )
                    .map((output) => (
                        <Button
                            className="lz-picker-item"
                            key={output.uid}
                            disabled={!writable}
                            onClick={() =>
                                commit({
                                    type: "add-monitor",
                                    monitor: {
                                        id: newIdentity(),
                                        selector: output.uid,
                                        channels: Math.min(
                                            64,
                                            output.outputChannels,
                                        ),
                                    },
                                })
                            }
                        >
                            <span>
                                <strong>{output.name}</strong>
                                <small>
                                    {output.outputChannels} output channels
                                </small>
                            </span>
                            +
                        </Button>
                    ))}
            </>
        );
    else if (modal.kind === "monitor" && monitor)
        content = (
            <>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Enable monitor")}</strong>
                        <p>
                            Keeps the mix active independently of recording
                            apps.
                        </p>
                    </div>
                    <Switch
                        checked={monitor.enabled}
                        label={t("Monitor enabled")}
                        disabled={!writable}
                        onChange={(enabled) =>
                            send({
                                type: "monitor",
                                device: d.id,
                                id: monitor.id,
                                patch: {
                                    enabled,
                                },
                            })
                        }
                    />
                </div>
                <EndpointEditor
                    key={`monitor:${monitor.id}`}
                    state={state}
                    device={d}
                    node={monitor}
                    kind="monitor"
                    writable={writable}
                    send={send}
                    transport={transport}
                    refresh={refresh}
                />
                <OrderControls
                    state={state}
                    device={d}
                    node={monitor}
                    kind="monitor"
                    writable={writable}
                    send={send}
                />
                <div className="lz-dialog-menu">
                    <Button
                        onClick={() =>
                            setModal({
                                kind: "mapping",
                                node: JSON.stringify(["monitor", monitor.id]),
                            })
                        }
                    >
                        {t("Edit monitor connections")}
                    </Button>
                    <Button
                        variant="danger"
                        onClick={() =>
                            setModal({
                                kind: "delete",
                                target: "monitor",
                                id: monitor.id,
                            })
                        }
                    >
                        {t("Remove monitor\u2026")}
                    </Button>
                </div>
            </>
        );
    else if (modal.kind === "delete") {
        const refs = modal.target === "device" ? dependencies(state, d.id) : [];
        content = (
            <>
                <p>
                    {modal.target === "device"
                        ? d.name
                        : modal.target === "source"
                          ? source?.name
                          : monitor?.name}{" "}
                    and its connections will be removed.{" "}
                    {modal.target === "device"
                        ? "Apps using this virtual input will lose it."
                        : "Other saved sources keep their settings."}
                </p>
                {refs.length > 0 && (
                    <p>
                        This also removes {refs.length} virtual source
                        {refs.length === 1 ? "" : "s"} from{" "}
                        {refs.map((r) => r.name).join(", ")}. Their other
                        sources remain saved.
                    </p>
                )}
            </>
        );
        actions = (
            <>
                {cancel}
                <Button
                    variant="danger"
                    disabled={!writable}
                    onClick={() =>
                        commit({
                            type:
                                modal.target === "device"
                                    ? "delete-device"
                                    : `remove-${modal.target}`,
                            id: modal.id,
                            removeDependencies: refs.length > 0,
                        })
                    }
                >
                    {t("Delete") + " "}
                    {modal.target}
                </Button>
            </>
        );
    } else if (modal.kind === "settings" && state.platform === "windows")
        content = (
            <>
                <div className="lz-setting-row">
                    <strong>{t("Language")}</strong>
                    <Language />
                </div>
                <p>{t(state.previewNotice)}</p>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Start routing at login")}</strong>
                        <p>
                            {t(
                                "Start the audio engine when you sign in to Windows.",
                            )}
                        </p>
                    </div>
                    <Switch
                        checked={!!state.background.login?.registered}
                        label={t("Start routing at login")}
                        disabled={
                            backgroundBusy || !state.background.login?.available
                        }
                        onChange={(enabled) =>
                            backgroundAction("login", { enabled })
                        }
                    />
                </div>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("LoopZ microphones")}</strong>
                        <p>
                            {t(
                                "Install the microphone service once. Saved mix changes then update microphones automatically. Windows asks for administrator approval during installation or repair.",
                            )}
                        </p>
                    </div>
                    <Button
                        disabled={backgroundBusy}
                        onClick={() => backgroundAction("driver-sync")}
                    >
                        {t("Install or repair microphones")}
                    </Button>
                </div>
                <p role="status">{t(windowsMicrophoneStatus(state).message)}</p>
                {windowsMicrophoneStatus(state).detail && (
                    <details>
                        <summary>{t("Setup details")}</summary>
                        <p>{windowsMicrophoneStatus(state).detail}</p>
                    </details>
                )}
                {backgroundNotice && <p role="status">{backgroundNotice}</p>}
                <div className="lz-setting-row">
                    <strong>{t("Getting started")}</strong>
                    <Button onClick={startTour}>{t("Take a tour")}</Button>
                </div>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Audio engine")}</strong>
                        <p>
                            {t(
                                "Routing continues when this window closes. Open LoopZ again to reconnect.",
                            )}
                        </p>
                    </div>
                    <Button
                        disabled={backgroundBusy}
                        onClick={() => backgroundAction("retry-session")}
                    >
                        {t("Retry now")}
                    </Button>
                </div>
                <Button
                    disabled={backgroundBusy}
                    onClick={() => backgroundAction("shutdown")}
                >
                    {t("Stop routing and quit")}
                </Button>
                <p>
                    {t(
                        "Each mix keeps its saved sample rate and channel layout. Changing a microphone format can briefly interrupt recording.",
                    )}
                </p>
                <p>
                    {t(
                        "If your microphone is unavailable, allow desktop microphone access in Windows Settings → Privacy → Microphone.",
                    )}
                </p>
                <div className="lz-setting-row">
                    <strong>{t("Appearance")}</strong>
                    <SegmentedControl
                        aria-label={t("Appearance")}
                        options={appearanceOptions.map((option) => ({
                            ...option,
                            label: t(option.label),
                        }))}
                        value={state.theme}
                        onChange={(value) => send({ type: "theme", value })}
                    />
                </div>
            </>
        );
    else if (modal.kind === "settings")
        content = (
            <>
                <div className="lz-setting-row">
                    <strong>{t("Language")}</strong>
                    <Language />
                </div>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Getting started")}</strong>
                        <p>
                            {t(
                                "A guided tour of sources, connections and monitors. Your audio settings won’t change.",
                            )}
                        </p>
                    </div>
                    <Button onClick={startTour}>{t("Take a tour")}</Button>
                </div>
                {releaseUpdates && <ReleaseUpdates value={releaseUpdates} />}
                <AudioSetup
                    state={state}
                    transport={transport}
                    refresh={refresh}
                />
                {installationComponents(state).map((component) => (
                    <div className="lz-setting-row" key={component.id}>
                        <div>
                            <strong>{component.name} installation</strong>
                            {component.detail && <p>{component.detail}</p>}
                        </div>
                        <Badge caps={false}>{component.label}</Badge>
                    </div>
                ))}
                <HardwareBufferSettings
                    inventory={state.inventory}
                    transport={transport}
                    refresh={refresh}
                />
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Audio engine")}</strong>
                        <p>
                            {state.connected
                                ? t(
                                      "Connected. Your graph is saved by the engine.",
                                  )
                                : state.background.detail ||
                                  t(
                                      "Reconnecting automatically. Saved settings stay with the engine.",
                                  )}
                        </p>
                    </div>
                    <Button
                        size="sm"
                        disabled={backgroundBusy}
                        onClick={() => backgroundAction("retry-session")}
                    >
                        {t("Retry now")}
                    </Button>
                </div>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Background routing")}</strong>
                        <p>
                            {state.background.detail ||
                                t("Checking background recovery.")}
                        </p>
                    </div>
                    <Badge caps={false}>
                        {state.background.managed
                            ? t("Automatic recovery")
                            : t("Setup needed")}
                    </Badge>
                </div>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Start routing at login")}</strong>
                        <p>
                            {state.background.login?.requiresApproval
                                ? t(
                                      "Allow LoopZ in System Settings \u2192 General \u2192 Login Items & Extensions.",
                                  )
                                : state.background.login?.detail ||
                                  t("Checking login startup.")}
                        </p>
                        {state.background.login?.requiresApproval && (
                            <Button
                                size="sm"
                                disabled={backgroundBusy}
                                onClick={() =>
                                    backgroundAction("login-settings")
                                }
                            >
                                {t("Open login settings")}
                            </Button>
                        )}
                    </div>
                    <Switch
                        checked={!!state.background.login?.registered}
                        label={t("Start routing at login")}
                        disabled={
                            backgroundBusy || !state.background.login?.available
                        }
                        onChange={(enabled) =>
                            backgroundAction("login", {
                                enabled,
                            })
                        }
                    />
                </div>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Microphone & input devices")}</strong>
                        <p>{microphoneAccess(state.permissions.microphone)}</p>
                        {microphoneNotice && (
                            <p role="status">{microphoneNotice}</p>
                        )}
                    </div>
                    {state.permissions.microphone === "not-determined" ? (
                        <Button
                            size="sm"
                            disabled={
                                microphoneBusy ||
                                state.permissionSetup.microphonePending
                            }
                            onClick={requestMicrophone}
                        >
                            {microphoneBusy ||
                            state.permissionSetup.microphonePending
                                ? t("Waiting for macOS\u2026")
                                : t("Enable microphone access")}
                        </Button>
                    ) : (
                        <Button
                            size="sm"
                            onClick={() => openPrivacy("microphone")}
                        >
                            {t("Open settings")}
                        </Button>
                    )}
                </div>
                <div className="lz-setting-row">
                    <div>
                        <strong>{t("Application audio")}</strong>
                        <p>
                            {t(
                                "macOS controls system audio-recording access. Reconnects do not request it again.",
                            )}
                        </p>
                    </div>
                    <Button size="sm" onClick={() => openPrivacy("audio")}>
                        {t("Open settings")}
                    </Button>
                </div>
                <div className="lz-setting-row">
                    <strong>{t("Appearance")}</strong>
                    <SegmentedControl
                        aria-label={t("Appearance")}
                        options={appearanceOptions.map((option) => ({
                            ...option,
                            label: t(option.label),
                        }))}
                        value={state.theme}
                        onChange={(value) =>
                            send({
                                type: "theme",
                                value,
                            })
                        }
                    />
                </div>
                <ModalActions>
                    <Button onClick={close}>{t("Done")}</Button>
                </ModalActions>
            </>
        );
    else if (modal.kind === "shortcuts")
        content = (
            <>
                <p>All connections can be edited without dragging.</p>
                <dl className="lz-shortcuts">
                    <dt>Tab / Shift Tab</dt>
                    <dd>Move through controls and sockets</dd>
                    <dt>Enter / Space</dt>
                    <dd>Select a socket or button</dd>
                    <dt>Arrow keys</dt>
                    <dd>Adjust sliders and selectors</dd>
                    <dt>Escape</dt>
                    <dd>Dismiss a modal or socket selection</dd>
                    <dt>
                        {state.platform === "windows"
                            ? "Ctrl N / Ctrl ,"
                            : "⌘ N / ⌘ ,"}
                    </dt>
                    <dd>New device / Settings</dd>
                    {state.history.supported && (
                        <>
                            <dt>
                                {state.platform === "windows"
                                    ? "Ctrl Z / Ctrl Shift Z"
                                    : "⌘ Z / ⇧ ⌘ Z"}
                            </dt>
                            <dd>Undo / Redo a routing change</dd>
                        </>
                    )}
                </dl>
                <p>
                    Choose a source socket, then an output input. To monitor,
                    choose an output send and a monitor input. The Connections
                    view provides the same edits.
                </p>
            </>
        );
    else
        content = (
            <p>
                This item is no longer available. Close this dialog to review
                the current routing.
            </p>
        );
    return (
        <Modal
            onClose={close}
            busy={state.saving}
            aria-label={titles[modal.kind]}
            cardClassName={
                ["mapping", "settings", "add-source"].includes(modal.kind)
                    ? "lz-wide-modal"
                    : "lz-modal"
            }
        >
            <ModalFocus kind={modal.kind} returnFocus={returnFocus} />
            <h2 className="modal-title">{titles[modal.kind]}</h2>
            {content}
            {(state.error || localError) && (
                <Banner
                    tone="danger"
                    heading={
                        state.uncertain
                            ? t("Review current routing")
                            : modal.kind === "settings"
                              ? t("Action could not be completed")
                              : t("Change not saved")
                    }
                    footer={
                        state.uncertain ? (
                            <Button
                                onClick={async () => {
                                    close();
                                    await send({
                                        type: "acknowledge",
                                    });
                                }}
                            >
                                {t("Review current routing")}
                            </Button>
                        ) : undefined
                    }
                >
                    {localError || state.error}
                </Banner>
            )}
            {actions && <ModalActions>{actions}</ModalActions>}
        </Modal>
    );
}
