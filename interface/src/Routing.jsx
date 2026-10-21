import { t } from "./i18n.js";
import React, { useRef, useState } from "react";
import { Button, Chip, StatusDot, Badge, LinkButton } from "@singz/ui";
import { Icon } from "@singz/ui/icons";
import { sourceCondition } from "./connection-state.js";
import { outputPairRemoval } from "./output-pairs.js";
import {
    sourceStatus,
    sourceLive,
    outputAmplitude,
    gainText,
    sourceAmplitude,
    monitorAmplitude,
} from "./model.js";
import { Gain } from "./components/Gain.jsx";
import { Switch } from "./components/Switch.jsx";
import { GearIcon, SpeakerState } from "./components/ControlIcons.jsx";
import { OutputControls } from "./components/OutputControls.jsx";
import {
    parseSocket,
    connectionAction,
} from "./components/connection-gestures.js";
export { Gain } from "./components/Gain.jsx";
export const MeterUpdates = React.createContext(true);
export function Meter({ level = 0, label = "Signal level" }) {
    const updating = React.useContext(MeterUpdates);
    const shown = useRef(level);
    if (updating) shown.current = level;
    level = shown.current;
    const amplitude = Math.max(
        0,
        Math.min(1, Number.isFinite(level) ? level : 0),
    );
    const previous = useRef(amplitude);
    const duration = amplitude > previous.current ? 35 : 80;
    previous.current = amplitude;
    const db = amplitude
        ? Math.max(-60, Math.round(20 * Math.log10(amplitude)))
        : -60;
    return (
        <span
            className="lz-level-meter"
            role="meter"
            aria-label={label}
            aria-valuemin={-60}
            aria-valuemax={0}
            aria-valuenow={db}
            aria-valuetext={amplitude ? `${db} decibels` : t("No signal")}
            data-updating={updating ? "true" : "false"}
            data-silent={amplitude === 0}
            data-clipping={amplitude >= 1}
            style={{
                "--signal": `${Math.sqrt(amplitude) * 100}%`,
                "--level": Math.sqrt(amplitude),
                "--meter-duration": `${duration}ms`,
            }}
        >
            <span className="lz-level-track" aria-hidden="true">
                <i className="lz-level-fill" />
                <b className="lz-level-peak" />
            </span>
            <span className="lz-level-reading" aria-hidden="true">
                {amplitude ? String(db).replace("-", "−") : "−∞"}
            </span>
        </span>
    );
}
export function SourceIdentity({ source }) {
    return (
        <span className="lz-node-icon">
            <Icon
                name={
                    source.kind === "app"
                        ? "play"
                        : source.kind === "hardware"
                          ? "pitch"
                          : source.kind === "virtual"
                            ? "interval"
                            : "replay"
                }
                size={20}
            />
        </span>
    );
}
const ConnectionTargets = React.createContext(null);
function Socket({
    kind,
    id,
    channel,
    onSocket,
    selection,
    connected,
    side,
    label,
}) {
    const key = `${kind}:${id}:${channel}`;
    const targeting = React.useContext(ConnectionTargets);
    const compatible = !!connectionAction(targeting?.start, {
        key,
        kind,
        id,
        channel,
    });
    const dropTarget = compatible && targeting?.target === key;
    return (
        <button
            className={`lz-socket ${side} ${connected ? "connected" : ""} ${selection?.key === key ? "selected" : ""} ${compatible ? "can-connect" : ""} ${dropTarget ? "drop-target" : ""}`}
            data-socket={key}
            data-connectable={compatible ? "true" : undefined}
            aria-description={
                compatible ? "Compatible connection target" : undefined
            }
            title={
                compatible
                    ? t("Connect here")
                    : `${label || kind} channel ${channel}`
            }
            data-od-id={`socket-${kind}-${id}-${channel}`}
            onClick={() =>
                onSocket({
                    key,
                    kind,
                    id,
                    channel,
                })
            }
            aria-label={`${label || kind} channel ${channel}`}
            aria-pressed={selection?.key === key}
        >
            <span />
        </button>
    );
}
function SourceNode({
    source: s,
    state,
    device,
    send,
    open,
    onSocket,
    selection,
}) {
    const condition = sourceCondition(state, s);
    const status = condition.label;
    const available = condition.available;
    const recover = (event) => {
        switch (condition.action?.kind) {
            case "settings":
                open(
                    {
                        kind: "settings",
                    },
                    event.currentTarget,
                );
                break;
            case "source-options":
                open(
                    {
                        kind: "source",
                        id: s.id,
                    },
                    event.currentTarget,
                );
                break;
            case "select-device":
                send({
                    type: "select",
                    id: condition.action.device,
                });
                break;
            case "retry":
                send({
                    type: "recover",
                });
                break;
        }
    };
    const rows = (start, end) =>
        Array.from(
            {
                length: end - start + 1,
            },
            (_, i) => i + start,
        ).map((ch) => (
            <div className="lz-channel-row" key={ch}>
                <span className="lz-channel-label">
                    {ch}
                    {s.channels === 2 ? (ch === 1 ? " · L" : " · R") : ""}
                </span>
                <Meter
                    level={sourceAmplitude(state, s, ch)}
                    label={`${s.name} channel ${ch} pre-gain meter`}
                />
                <Socket
                    kind="source"
                    label={`${s.name} source`}
                    id={s.id}
                    channel={ch}
                    side="right"
                    selection={selection}
                    onSocket={onSocket}
                    connected={s.routes[ch - 1]?.length > 0}
                />
            </div>
        ));
    return (
        <article className="lz-node lz-source" data-od-id={`source-${s.id}`}>
            <div className="lz-node-heading">
                <SourceIdentity source={s} />
                <div>
                    <h3>{s.name}</h3>
                    <p>
                        {s.kind === "app"
                            ? "Application · stereo"
                            : s.kind === "pass"
                              ? t("Apps using this device as output")
                              : s.kind === "virtual"
                                ? `Virtual device · ${s.channels} channels`
                                : `Input device · ${s.channels} channel${s.channels > 1 ? "s" : ""}`}
                    </p>
                </div>
                <Switch
                    checked={s.enabled}
                    label={`${s.name} enabled`}
                    onChange={(enabled) =>
                        send({
                            type: "source",
                            id: s.id,
                            patch: {
                                enabled,
                            },
                        })
                    }
                />
                <Button
                    size="sm"
                    icon
                    aria-label={`${s.name} options`}
                    onClick={(event) =>
                        open(
                            {
                                kind: "source",
                                id: s.id,
                            },
                            event.currentTarget,
                        )
                    }
                >
                    <GearIcon />
                </Button>
            </div>
            <div className="lz-node-controls">
                <Chip
                    active={s.muted}
                    aria-label={`${s.name} mute`}
                    title="Mute this source in the mix"
                    onClick={() =>
                        send({
                            type: "source",
                            id: s.id,
                            patch: {
                                muted: !s.muted,
                            },
                        })
                    }
                >
                    <SpeakerState muted={s.muted} />
                </Chip>
                <Gain
                    value={s.gain}
                    label={`${s.name} gain`}
                    onChange={(gain) =>
                        send({
                            type: "source",
                            id: s.id,
                            patch: {
                                gain,
                            },
                        })
                    }
                />
            </div>
            <div className="lz-channel-bank">
                {rows(1, Math.min(s.channels, 2))}
                {s.channels > 2 && (
                    <details open={s.channels <= 8 ? true : undefined}>
                        <summary>{s.channels - 2} more input channels</summary>
                        {rows(3, s.channels)}
                    </details>
                )}
            </div>
            <div className={`lz-node-foot ${!available ? "attention" : ""}`}>
                <StatusDot
                    tone={
                        !available
                            ? "warn"
                            : condition.kind === "available"
                              ? "ok"
                              : "idle"
                    }
                />
                <span>
                    {!available
                        ? `${status} · wiring saved`
                        : condition.kind === "idle"
                          ? t("Ready when used \xB7 connections retained")
                          : s.kind === "app"
                            ? `Original playback muted: ${s.muteOriginal ? t("On") : t("Off")}`
                            : s.enabled
                              ? t("Enabled \xB7 pre-gain meters")
                              : t("Disabled \xB7 connections retained")}
                </span>
                {!available && condition.action && (
                    <LinkButton
                        disabled={
                            condition.action.kind === "retry" && !state.canEdit
                        }
                        onClick={recover}
                    >
                        {condition.action.label}
                    </LinkButton>
                )}
            </div>
        </article>
    );
}
function OutputPair({
    start,
    device: d,
    state,
    selection,
    onSocket,
    open,
    send,
}) {
    return (
        <article
            className="lz-node lz-output-pair"
            data-od-id={`output-pair-${start}`}
        >
            <div className="lz-pair-title">
                <h3>
                    {t("Channels") + " "}
                    {start}–{start + 1}
                </h3>
                <Badge caps={false}>{t("Stereo")}</Badge>
                {state.canRemoveOutputPair && d.channels > 2 && (
                    <Button
                        size="sm"
                        icon
                        aria-label={`Remove output channels ${start}–${start + 1}`}
                        disabled={!state.canEdit || state.saving}
                        onClick={(event) =>
                            open(
                                {
                                    kind: "removePair",
                                    start,
                                    review: outputPairRemoval(
                                        state,
                                        d.id,
                                        start,
                                    ),
                                },
                                event.currentTarget,
                            )
                        }
                    >
                        −
                    </Button>
                )}
            </div>
            <div className="lz-output-rows">
                {[start, start + 1].map((ch) => (
                    <div className="lz-channel-row" key={ch}>
                        <Socket
                            kind="output-input"
                            label="Output input"
                            id="bus"
                            channel={ch}
                            side="left"
                            selection={selection}
                            onSocket={onSocket}
                            connected={d.sources.some((s) =>
                                s.routes.some((r) => r.includes(ch)),
                            )}
                        />
                        <span className="lz-channel-label">{ch}</span>
                        <Meter
                            level={outputAmplitude(state, d.id, ch)}
                            label={`Output ${ch} post-gain meter`}
                        />
                        <Socket
                            kind="output-send"
                            label="Output send"
                            id="bus"
                            channel={ch}
                            side="right"
                            selection={selection}
                            onSocket={onSocket}
                            connected={d.monitors.some((m) =>
                                m.routes.some((r) => r.includes(ch)),
                            )}
                        />
                    </div>
                ))}
            </div>
            {start === 1 && (
                <OutputControls
                    device={d}
                    state={state}
                    send={send}
                    open={open}
                />
            )}
        </article>
    );
}
function MonitorNode({
    monitor: m,
    state,
    device: d,
    send,
    open,
    onSocket,
    selection,
}) {
    const waiting = state.monitorFaults[m.id];
    return (
        <article className="lz-node lz-monitor" data-od-id={`monitor-${m.id}`}>
            <div className="lz-node-heading">
                <span className="lz-node-icon">
                    <Icon name="speaker" size={20} />
                </span>
                <div>
                    <h3>{m.name}</h3>
                    <p>
                        <StatusDot
                            tone={waiting ? "warn" : m.enabled ? "ok" : "idle"}
                        />
                        {waiting ||
                            (m.enabled
                                ? "Available · stereo"
                                : t("Monitor disabled"))}
                    </p>
                </div>
                <Switch
                    checked={m.enabled}
                    label={`${m.name} enabled`}
                    onChange={(enabled) =>
                        send({
                            type: "monitor",
                            id: m.id,
                            patch: {
                                enabled,
                            },
                        })
                    }
                />
                <Button
                    size="sm"
                    icon
                    aria-label={`${m.name} options`}
                    onClick={(event) =>
                        open(
                            {
                                kind: "monitor",
                                id: m.id,
                            },
                            event.currentTarget,
                        )
                    }
                >
                    <GearIcon />
                </Button>
            </div>
            <div className="lz-node-controls">
                <Chip
                    active={m.muted}
                    aria-label={`${m.name} mute`}
                    onClick={() =>
                        send({
                            type: "monitor",
                            id: m.id,
                            patch: {
                                muted: !m.muted,
                            },
                        })
                    }
                >
                    <SpeakerState muted={m.muted} />
                </Chip>
                <Gain
                    value={m.gain}
                    label={`${m.name} gain`}
                    onChange={(gain) =>
                        send({
                            type: "monitor",
                            id: m.id,
                            patch: {
                                gain,
                            },
                        })
                    }
                />
            </div>
            <div className="lz-channel-bank">
                {m.routes.map((outs, i) => (
                    <div className="lz-channel-row" key={i}>
                        <Socket
                            kind="monitor-input"
                            label={`${m.name} monitor input`}
                            id={m.id}
                            channel={i + 1}
                            side="left"
                            selection={selection}
                            onSocket={onSocket}
                            connected={outs.length > 0}
                        />
                        <span className="lz-channel-label">
                            {i + 1}
                            {m.channels === 2 ? (i ? " · R" : " · L") : ""}
                        </span>
                        <Meter
                            label={`${m.name} channel ${i + 1} meter`}
                            level={monitorAmplitude(state, m, i + 1)}
                        />
                    </div>
                ))}
            </div>
            <div className="lz-node-foot">
                <Icon name="speaker" size={13} />
                <span>
                    {waiting
                        ? "Reconnecting automatically. Connections retained."
                        : m.enabled
                          ? "Keeps routing active without a recording app."
                          : "Enable to listen without a recording app."}
                </span>
            </div>
        </article>
    );
}
function Wires({
    graphRef,
    state,
    device,
    selection,
    selectedWire,
    onSelect,
    onRemove,
    onBeginWire,
}) {
    const [paths, setPaths] = useState([]);
    const availability = JSON.stringify([
        state.connected,
        state.service,
        state.monitorFaults,
        state.snapshot?.runtime?.sources?.map((source) => [
            source.kind,
            source.selector,
            source.connected,
            source.complete,
            source.error,
        ]),
        state.snapshot?.runtime?.devices?.map((d) => [
            d.id,
            d.sources.map((s) => [s.id, s.online]),
        ]),
    ]);
    const geometry = JSON.stringify([
        device.id,
        device.channels,
        device.enabled,
        device.sources.map((source) => [
            source.id,
            source.name,
            source.channels,
            source.routes,
            source.muted,
        ]),
        device.monitors.map((monitor) => [
            monitor.id,
            monitor.channels,
            monitor.routes,
            monitor.enabled,
            monitor.muted,
        ]),
    ]);
    // Measure after the parent graph ref has attached on the initial commit.
    React.useEffect(() => {
        const graph = graphRef.current;
        if (!graph) return;
        const draw = () => {
            const box = graph.getBoundingClientRect();
            const result = [];
            const labels = new Map(
                [...graph.querySelectorAll("[data-socket]")].map((el) => [
                    el.dataset.socket,
                    el.getAttribute("aria-label"),
                ]),
            );
            const positions = new Map(
                [...graph.querySelectorAll("[data-socket]")]
                    .filter((el) => el.getClientRects().length)
                    .map((el) => [
                        el.dataset.socket,
                        el.getBoundingClientRect(),
                    ]),
            );
            const wire = (a, b, waiting, monitor) => {
                const A = positions.get(a),
                    B = positions.get(b);
                if (!A || !B || result.length >= 4096) return;
                const x = A.x + A.width / 2 - box.x,
                    y = A.y + A.height / 2 - box.y,
                    X = B.x + B.width / 2 - box.x,
                    Y = B.y + B.height / 2 - box.y,
                    c = Math.max(22, (X - x) * 0.6);
                result.push({
                    key: JSON.stringify([a, b]),
                    startKey: a,
                    action: connectionAction(parseSocket(a), parseSocket(b)),
                    label: `${labels.get(a)} → ${labels.get(b)}`,
                    path: `M${x},${y} C${x + c},${y} ${X - c},${Y} ${X},${Y}`,
                    waiting,
                    monitor,
                });
            };
            device.sources.forEach((s) =>
                s.routes.forEach((outs, i) =>
                    outs.forEach((n) =>
                        wire(
                            `source:${s.id}:${i + 1}`,
                            `output-input:bus:${n}`,
                            !device.enabled || !sourceLive(state, s) || s.muted,
                            false,
                        ),
                    ),
                ),
            );
            device.monitors.forEach((m) =>
                m.routes.forEach((outs, i) =>
                    outs.forEach((n) =>
                        wire(
                            `output-send:bus:${n}`,
                            `monitor-input:${m.id}:${i + 1}`,
                            !device.enabled ||
                                !m.enabled ||
                                m.muted ||
                                !!state.monitorFaults[m.id],
                            true,
                        ),
                    ),
                ),
            );
            setPaths(result);
        };
        const observer = new ResizeObserver(draw);
        observer.observe(graph);
        graph.addEventListener("toggle", draw, true);
        window.addEventListener("resize", draw);
        draw();
        return () => {
            observer.disconnect();
            graph.removeEventListener("toggle", draw, true);
            window.removeEventListener("resize", draw);
        };
    }, [geometry, selection, availability]);
    return (
        <svg className="lz-wires" aria-label={t("Channel connections")}>
            {paths.map((p) => (
                <g
                    key={p.key}
                    className={`lz-wire ${selectedWire?.key === p.key ? "is-selected" : ""}`}
                >
                    <path
                        className={`lz-wire-line ${p.monitor ? "monitor" : ""} ${p.waiting ? "waiting" : ""}`}
                        d={p.path}
                    />
                    <path
                        className="lz-wire-hit"
                        d={p.path}
                        tabIndex="0"
                        role="button"
                        aria-label={`Connection: ${p.label}`}
                        aria-pressed={selectedWire?.key === p.key}
                        onPointerDown={(e) => onBeginWire(e, p)}
                        onClick={() => onSelect(p)}
                        onKeyDown={(e) => {
                            if (e.key === "Enter" || e.key === " ") {
                                e.preventDefault();
                                onSelect(p);
                            }
                            if (e.key === "Delete" || e.key === "Backspace") {
                                e.preventDefault();
                                onRemove(p);
                            }
                            if (e.key === "Escape") onSelect(null);
                        }}
                    >
                        <title>{p.label} · Select to inspect or remove</title>
                    </path>
                </g>
            ))}
        </svg>
    );
}
export function Routing({
    state,
    device: d,
    send,
    open,
    selection,
    onSocket,
    onClearSelection,
}) {
    const graphRef = useRef(null),
        gesture = useRef(null),
        suppressClick = useRef(false);
    const [drag, setDrag] = useState(null),
        [selectedWire, setSelectedWire] = useState(null),
        [wireNotice, setWireNotice] = useState("");
    // Inventory/name refreshes recreate projected devices without changing a
    // socket or wire. Cancel gestures only when their actual endpoints change.
    const wiring = JSON.stringify([
        d.id,
        d.channels,
        d.sources.map((source) => [source.id, source.channels, source.routes]),
        d.monitors.map((monitor) => [
            monitor.id,
            monitor.channels,
            monitor.routes,
        ]),
    ]);
    React.useEffect(() => {
        setSelectedWire(null);
        setDrag(null);
        gesture.current = null;
    }, [d.id, state.canEdit]);
    React.useEffect(() => {
        gesture.current = null;
        setDrag(null);
        setSelectedWire(null);
    }, [wiring]);
    const restoreSocketFocus = (wire, graph = graphRef.current) => {
        if (
            !graph ||
            graphRef.current !== graph ||
            graph.dataset.device !== d.id
        )
            return;
        [...graph.querySelectorAll("[data-socket]")]
            .find((el) => el.dataset.socket === wire.startKey)
            ?.focus();
    };
    const dismissWire = () => {
        if (selectedWire) restoreSocketFocus(selectedWire);
        setSelectedWire(null);
    };
    const removeWire = async (wire) => {
        if (!state.canEdit || state.saving) return;
        const graph = graphRef.current;
        if (
            !(await send({
                ...wire.action,
                mode: "remove",
            }))
        )
            return;
        // The user may have changed view or device while the engine saved.
        if (
            !graph ||
            graphRef.current !== graph ||
            graph.dataset.device !== d.id
        )
            return;
        requestAnimationFrame(() => restoreSocketFocus(wire, graph));
        setSelectedWire(null);
        setWireNotice("Connection removed. Other connections are unchanged.");
    };
    function dragStart(e) {
        if (e.button !== 0 || !state.canEdit || state.saving) return;
        const socket = e.target.closest("[data-socket]");
        if (!socket) {
            if (!e.target.closest(".lz-wire-hit, .lz-wire-inspector"))
                setSelectedWire(null);
            return;
        }
        const rect = socket.getBoundingClientRect(),
            box = graphRef.current.getBoundingClientRect();
        gesture.current = {
            socket: parseSocket(socket.dataset.socket),
            clientX: e.clientX,
            clientY: e.clientY,
            x: rect.x + rect.width / 2 - box.x,
            y: rect.y + rect.height / 2 - box.y,
            moved: false,
        };
        socket.focus();
    }
    function beginWire(e, wire) {
        if (e.button !== 0 || !state.canEdit || state.saving) return;
        const socket = graphRef.current.querySelector(
            `[data-socket="${wire.startKey}"]`,
        );
        if (!socket) return;
        const rect = socket.getBoundingClientRect(),
            box = graphRef.current.getBoundingClientRect();
        gesture.current = {
            socket: parseSocket(wire.startKey),
            clientX: e.clientX,
            clientY: e.clientY,
            x: rect.x + rect.width / 2 - box.x,
            y: rect.y + rect.height / 2 - box.y,
            moved: false,
            rewire: wire.action,
        };
        setSelectedWire(wire);
    }
    function dragMove(e) {
        const g = gesture.current;
        if (!g) return;
        if (
            !g.moved &&
            Math.hypot(e.clientX - g.clientX, e.clientY - g.clientY) < 5
        )
            return;
        if (!g.moved) onClearSelection?.();
        g.moved = true;
        e.preventDefault();
        if (!graphRef.current.hasPointerCapture(e.pointerId))
            graphRef.current.setPointerCapture(e.pointerId);
        const box = graphRef.current.getBoundingClientRect();
        const target = document
            .elementFromPoint(e.clientX, e.clientY)
            ?.closest("[data-socket]");
        setDrag({
            ...g,
            X: e.clientX - box.x,
            Y: e.clientY - box.y,
            target: target?.dataset.socket,
            valid: !!connectionAction(
                g.socket,
                parseSocket(target?.dataset.socket),
            ),
        });
    }
    async function dragEnd(e) {
        const g = gesture.current;
        gesture.current = null;
        setDrag(null);
        if (graphRef.current.hasPointerCapture(e.pointerId))
            graphRef.current.releasePointerCapture(e.pointerId);
        if (!g?.moved) return;
        suppressClick.current = true;
        setTimeout(() => {
            suppressClick.current = false;
        }, 0);
        const target = document
            .elementFromPoint(e.clientX, e.clientY)
            ?.closest("[data-socket]");
        const action = connectionAction(
            g.socket,
            parseSocket(target?.dataset.socket),
        );
        if (!action) {
            setWireNotice(
                "No change. Drop on an output input or monitor input.",
            );
            return;
        }
        const nodes = action.kind === "source" ? d.sources : d.monitors;
        const exists = nodes
            .find((n) => n.id === action.id)
            ?.routes[action.channel - 1]?.includes(action.output);
        const same =
            g.rewire &&
            g.rewire.kind === action.kind &&
            g.rewire.id === action.id &&
            g.rewire.channel === action.channel &&
            g.rewire.output === action.output;
        if (!state.canEdit || state.saving) return;
        if (
            !same &&
            !(await send({
                ...action,
                mode: "add",
                previous: g.rewire,
            }))
        )
            return;
        setSelectedWire(null);
        setWireNotice(
            same
                ? "Connection unchanged."
                : g.rewire
                  ? "Connection moved and saved."
                  : exists
                    ? "These channels are already connected."
                    : "Connection added and saved.",
        );
    }
    return (
        <ConnectionTargets.Provider
            value={{
                start: drag?.socket || selection,
                target: drag?.target,
            }}
        >
            <div
                className="lz-graph"
                ref={graphRef}
                data-od-id="signal-workspace"
                data-device={d.id}
                onPointerDownCapture={dragStart}
                onPointerMove={dragMove}
                onPointerUp={dragEnd}
                onPointerCancel={() => {
                    gesture.current = null;
                    setDrag(null);
                }}
                onClickCapture={(e) => {
                    if (suppressClick.current) {
                        e.preventDefault();
                        e.stopPropagation();
                    }
                }}
                onKeyDownCapture={(e) => {
                    if (e.key === "Escape") {
                        gesture.current = null;
                        setDrag(null);
                        if (e.target.closest(".lz-wire-inspector"))
                            dismissWire();
                        else setSelectedWire(null);
                    }
                }}
            >
                {drag && (
                    <svg className="lz-drag-wire" aria-hidden="true">
                        <path
                            className={drag.valid ? "valid" : ""}
                            d={`M${drag.x},${drag.y} C${drag.x + (drag.X - drag.x) * 0.5},${drag.y} ${drag.X - (drag.X - drag.x) * 0.5},${drag.Y} ${drag.X},${drag.Y}`}
                        />
                    </svg>
                )}
                {selectedWire && (
                    <div
                        className="lz-wire-inspector"
                        data-od-id="selected-connection"
                        role="group"
                        aria-label={t("Selected connection")}
                    >
                        <span>{selectedWire.label}</span>
                        <Button
                            size="sm"
                            onClick={() => removeWire(selectedWire)}
                        >
                            {t("Remove connection")}
                        </Button>
                        <Button size="sm" onClick={dismissWire}>
                            {t("Done")}
                        </Button>
                    </div>
                )}
                <span className="sr-only" role="status">
                    {wireNotice}
                </span>
                <Wires
                    state={state}
                    device={d}
                    graphRef={graphRef}
                    selection={selection}
                    selectedWire={selectedWire}
                    onSelect={setSelectedWire}
                    onRemove={removeWire}
                    onBeginWire={beginWire}
                />
                <section className="lz-sources" data-od-id="sources">
                    <div className="lz-column-heading">
                        <span className="eyebrow">
                            {t("Sources") + " "}
                            <span>{d.sources.length}</span>
                        </span>
                        <Button
                            size="sm"
                            icon
                            aria-label={t("Add source")}
                            onClick={(event) =>
                                open(
                                    {
                                        kind: "add-source",
                                    },
                                    event.currentTarget,
                                )
                            }
                        >
                            +
                        </Button>
                    </div>
                    {!d.sources.length && (
                        <div
                            className="lz-source-empty"
                            data-od-id="empty-source-block"
                        >
                            <Icon name="interval" size={30} />
                            <h2>{t("Your mix starts here.")}</h2>
                            <p>
                                Add an app, microphone or virtual device. Your
                                output settings are ready.
                            </p>
                            <Button
                                variant="primary"
                                onClick={(event) =>
                                    open(
                                        {
                                            kind: "add-source",
                                        },
                                        event.currentTarget,
                                    )
                                }
                            >
                                {t("Add your first source")}
                            </Button>
                        </div>
                    )}
                    {d.sources.map((s) => (
                        <SourceNode
                            key={s.id}
                            source={s}
                            state={state}
                            device={d}
                            send={send}
                            open={open}
                            selection={selection}
                            onSocket={onSocket}
                        />
                    ))}
                    <p className="lz-canvas-note">
                        Drag between sockets to connect. Click a wire to select
                        it.
                        <br />
                        <LinkButton
                            onClick={(event) =>
                                open(
                                    {
                                        kind: "mapping",
                                    },
                                    event.currentTarget,
                                )
                            }
                        >
                            {t("Edit connections without dragging")}
                        </LinkButton>
                    </p>
                </section>
                <section className="lz-outputs" data-od-id="outputs">
                    <div className="lz-column-heading">
                        <span className="eyebrow">{t("Output channels")}</span>
                    </div>
                    <div className="lz-first-pair">
                        <OutputPair
                            start={1}
                            send={send}
                            open={open}
                            device={d}
                            state={state}
                            selection={selection}
                            onSocket={onSocket}
                        />
                    </div>
                    {d.channels > 2 && (
                        <details
                            className="lz-more-pairs"
                            open={d.channels === 8 ? true : undefined}
                        >
                            <summary>
                                {d.channels / 2 - 1} more stereo pairs
                            </summary>
                            {Array.from(
                                {
                                    length: d.channels / 2 - 1,
                                },
                                (_, i) => (
                                    <details
                                        key={i}
                                        open={
                                            d.channels === 8 ? true : undefined
                                        }
                                    >
                                        <summary>
                                            {t("Channels") + " "}
                                            {i * 2 + 3}–{i * 2 + 4}
                                        </summary>
                                        <OutputPair
                                            start={i * 2 + 3}
                                            send={send}
                                            open={open}
                                            device={d}
                                            state={state}
                                            selection={selection}
                                            onSocket={onSocket}
                                        />
                                    </details>
                                ),
                            )}
                        </details>
                    )}
                    <p className="lz-canvas-note">
                        {t("Connected sources sum at each output.")}
                        <br />
                        {t("Meters show post-gain signal.")}
                    </p>
                </section>
                <section className="lz-monitors" data-od-id="monitors">
                    <div className="lz-column-heading">
                        <span className="eyebrow">
                            {t("Monitors") + " "}
                            <span>{d.monitors.length}</span>
                        </span>
                        <Button
                            size="sm"
                            icon
                            aria-label={t("Add monitor")}
                            onClick={(event) =>
                                open(
                                    {
                                        kind: "add-monitor",
                                    },
                                    event.currentTarget,
                                )
                            }
                        >
                            +
                        </Button>
                    </div>
                    <div className="lz-first-monitor">
                        {d.monitors.map((m) => (
                            <MonitorNode
                                key={m.id}
                                monitor={m}
                                state={state}
                                device={d}
                                send={send}
                                open={open}
                                selection={selection}
                                onSocket={onSocket}
                            />
                        ))}
                    </div>
                    <p className="lz-canvas-note">
                        {t("Listen here. Record anywhere.")}
                        <br />
                        {t("Choose") + " "}
                        <strong>{d.name}</strong> as the input in your recording
                        app.
                    </p>
                </section>
            </div>
        </ConnectionTargets.Provider>
    );
}
export function ConnectionList({ device: d, send, open, state }) {
    const [page, setPage] = useState(0);
    const [filter, setFilter] = useState("");
    const all = React.useMemo(
        () => [
            ...d.sources.flatMap((s) =>
                s.connections.map((wire) => ({
                    kind: "source",
                    node: s,
                    channel: wire.sourceChannel + 1,
                    output: wire.outputChannel + 1,
                })),
            ),
            ...d.monitors.flatMap((m) =>
                m.connections.map((wire) => ({
                    kind: "monitor",
                    node: m,
                    channel: wire.outputChannel + 1,
                    output: wire.sourceChannel + 1,
                })),
            ),
        ],
        [d],
    );
    React.useEffect(() => {
        setPage(0);
        setFilter("");
    }, [d.id]);
    const rows = all.filter(
        (row) => !filter || JSON.stringify([row.kind, row.node.id]) === filter,
    );
    const pages = Math.max(1, Math.ceil(rows.length / 100));
    const current = Math.min(page, pages - 1);
    return (
        <div className="lz-connection-list">
            <OutputControls
                device={d}
                state={state}
                send={send}
                open={open}
                standalone
            />
            <div className="lz-list-heading">
                <label>
                    Show{" "}
                    <select
                        aria-label={t("Filter connections")}
                        value={filter}
                        onChange={(event) => {
                            setFilter(event.target.value);
                            setPage(0);
                        }}
                    >
                        <option value="">{t("All connections")}</option>
                        {[
                            ...d.sources.map((s) => ["source", s]),
                            ...d.monitors.map((m) => ["monitor", m]),
                        ].map(([kind, node]) => (
                            <option
                                key={JSON.stringify([kind, node.id])}
                                value={JSON.stringify([kind, node.id])}
                            >
                                {node.name} · {kind}
                            </option>
                        ))}
                    </select>
                </label>
                <Button
                    size="sm"
                    onClick={(event) =>
                        open(
                            {
                                kind: "mapping",
                            },
                            event.currentTarget,
                        )
                    }
                >
                    {t("Edit connections")}
                </Button>
            </div>
            <table>
                <thead>
                    <tr>
                        <th>{t("From")}</th>
                        <th>{t("To")}</th>
                        <th>Status</th>
                        <th>Action</th>
                    </tr>
                </thead>
                <tbody>
                    {rows
                        .slice(current * 100, (current + 1) * 100)
                        .map((row) => (
                            <tr
                                key={JSON.stringify([
                                    row.kind,
                                    row.node.id,
                                    row.channel,
                                    row.output,
                                ])}
                            >
                                <td>
                                    {row.kind === "source"
                                        ? `${row.node.name} · ${row.channel}`
                                        : `Output ${row.output}`}
                                </td>
                                <td>
                                    {row.kind === "source"
                                        ? `Output ${row.output}`
                                        : `${row.node.name} · ${row.channel}`}
                                </td>
                                <td>
                                    {row.kind === "source"
                                        ? sourceStatus(state, row.node)
                                        : state.monitorFaults[row.node.id] ||
                                          t("Connected")}
                                </td>
                                <td>
                                    <Button
                                        size="sm"
                                        disabled={!state.canEdit}
                                        onClick={() =>
                                            send({
                                                type: "route",
                                                kind: row.kind,
                                                id: row.node.id,
                                                channel: row.channel,
                                                output: row.output,
                                            })
                                        }
                                    >
                                        {t("Disconnect")}
                                    </Button>
                                </td>
                            </tr>
                        ))}
                </tbody>
            </table>
            <div className="lz-list-heading">
                <span>
                    {rows.length} connections · Page {current + 1} of {pages}
                </span>
                <Button
                    size="sm"
                    disabled={!current}
                    onClick={() => setPage(current - 1)}
                >
                    {t("Previous")}
                </Button>
                <Button
                    size="sm"
                    disabled={current + 1 >= pages}
                    onClick={() => setPage(current + 1)}
                >
                    {t("Next")}
                </Button>
            </div>
        </div>
    );
}
