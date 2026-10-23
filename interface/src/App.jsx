import { createTourDemoState } from "./tour-demo.js";
import StartupTour from "./components/StartupTour.jsx";
import { tourSeen, rememberTour } from "./tour.js";
import React, {
    useEffect,
    useMemo,
    useCallback,
    useRef,
    useState,
    useSyncExternalStore,
} from "react";
import {
    Button,
    Chip,
    SegmentedControl,
    StatusDot,
    Banner,
    LinkButton,
} from "@singz/ui";
import { Icon } from "@singz/ui/icons";
import { selected, health, outputAmplitude } from "./model.js";
import {
    Routing,
    ConnectionList,
    Gain,
    Meter,
    MeterUpdates,
} from "./Routing.jsx";
import Dialogs from "./Dialogs.jsx";
import { LoopZMark } from "./components/LoopZMark.jsx";
import { Language } from "./components/Language.jsx";
import { getLocale, subscribeLocale, t } from "./i18n.js";
import { useReleaseUpdates } from "./ReleaseUpdates.jsx";
import useAppearance from "./useAppearance.js";
import { Switch } from "./components/Switch.jsx";
import { GearIcon } from "./components/ControlIcons.jsx";
import { installationCondition } from "./installation.js";
import { backupRecoveryNotice } from "./document-recovery.js";
export default function App({
    state: liveState,
    send: sendLive,
    transport: liveTransport,
    refresh,
}) {
    const locale = useSyncExternalStore(subscribeLocale, getLocale);
    const [tourOpen, setTourOpen] = useState(false);
    const demoState = useMemo(
        () => (tourOpen ? createTourDemoState(liveState.theme) : null),
        [tourOpen, liveState.theme, locale],
    );
    const state = demoState || liveState;
    // Even programmatic controls or native menu events cannot write demo IDs to
    // the real graph. Read-only update status and menu context stay available.
    const send = useCallback(
        (action) => (tourOpen ? Promise.resolve(false) : sendLive(action)),
        [tourOpen, sendLive],
    );
    const transport = useCallback(
        (request) => {
            if (
                tourOpen &&
                !["updates", "menu-context"].includes(request.operation)
            )
                return Promise.resolve({ ok: false, code: "demo-preview" });
            return liveTransport(request);
        },
        [tourOpen, liveTransport],
    );
    const releaseUpdates = useReleaseUpdates(transport);
    const palette = useAppearance(state.theme);
    const [view, setView] = useState("routing");
    const [modal, setModal] = useState(null);
    const [selection, setSelection] = useState(null);
    const [sidebar, setSidebar] = useState(false);
    const [message, setMessage] = useState("");
    const [showWelcome, setShowWelcome] = useState(() => !tourSeen());
    const tourReturnView = useRef("routing");
    const dismissWelcome = () => {
        rememberTour();
        setShowWelcome(false);
    };
    const startTour = () => {
        tourReturnView.current = view;
        setModal(null);
        setSidebar(false);
        setView("routing");
        setTourOpen(true);
    };
    const finishTour = () => {
        dismissWelcome();
        setTourOpen(false);
        setView(tourReturnView.current);
    };
    const returnFocus = useRef(null);
    const d = selected(state);
    const windowsMicrophone =
        state.platform === "windows"
            ? state.snapshot?.runtime?.devices?.find((row) => row.id === d?.id)
                  ?.microphone
            : null;
    const setupIssue = installationCondition(state);
    const backupNotice = backupRecoveryNotice(state);
    const layoutIdentity = JSON.stringify(
        d && [
            d.id,
            d.channels,
            d.sources.map((source) => [source.id, source.channels]),
            d.monitors.map((monitor) => [monitor.id, monitor.channels]),
        ],
    );
    const open = (next, invoker) => {
        if (tourOpen) return;
        returnFocus.current = invoker || document.activeElement;
        send({
            type: "clear-error",
        });
        setModal({
            device: state.selected,
            ...next,
        });
    };
    const moveHistory = async (direction) => {
        if (
            await send({
                type: direction,
            })
        )
            setSelection(null);
    };
    useEffect(() => {
        let active = true;
        const textEditing = () => {
            const target = document.activeElement;
            return !!(
                target?.isContentEditable ||
                target?.tagName === "TEXTAREA" ||
                (target?.tagName === "INPUT" &&
                    !["range", "checkbox", "radio", "button"].includes(
                        target.type,
                    ))
            );
        };
        const context = () => {
            if (!active) return;
            transport({
                version: 1,
                operation: "menu-context",
                textEditing: textEditing(),
                canUndo:
                    !modal &&
                    !tourOpen &&
                    state.canEdit &&
                    state.history.supported &&
                    state.history.canUndo,
                canRedo:
                    !modal &&
                    !tourOpen &&
                    state.canEdit &&
                    state.history.supported &&
                    state.history.canRedo,
            }).catch(() => {});
        };
        const focus = () => queueMicrotask(context);
        const action = (event) => {
            if (
                !modal &&
                !tourOpen &&
                !textEditing() &&
                state.canEdit &&
                state.history.supported &&
                ["undo", "redo"].includes(event.detail) &&
                state.history[event.detail === "undo" ? "canUndo" : "canRedo"]
            )
                moveHistory(event.detail);
        };
        context();
        document.addEventListener("focusin", focus);
        document.addEventListener("focusout", focus);
        window.addEventListener("loopz-history", action);
        return () => {
            active = false;
            document.removeEventListener("focusin", focus);
            document.removeEventListener("focusout", focus);
            window.removeEventListener("loopz-history", action);
        };
    }, [
        modal,
        tourOpen,
        state.canEdit,
        state.history.supported,
        state.history.canUndo,
        state.history.canRedo,
        transport,
    ]);
    useEffect(() => {
        setSelection(null);
    }, [state.selected, layoutIdentity]);
    useEffect(() => {
        const key = (event) => {
            if (modal || tourOpen) return;
            const target = event.target;
            const textEditing =
                target?.isContentEditable ||
                (target?.tagName === "INPUT" &&
                    !["range", "checkbox", "radio", "button"].includes(
                        target.type,
                    )) ||
                target?.tagName === "TEXTAREA";
            if (
                (event.metaKey || event.ctrlKey) &&
                event.key.toLowerCase() === "z" &&
                state.history.supported &&
                !textEditing
            ) {
                event.preventDefault();
                const direction = event.shiftKey ? "redo" : "undo";
                if (
                    !event.repeat &&
                    state.canEdit &&
                    state.history[event.shiftKey ? "canRedo" : "canUndo"]
                )
                    moveHistory(direction);
                return;
            }
            if (event.key === "Escape") setSelection(null);
            if (
                (event.metaKey || event.ctrlKey) &&
                ["n", ","].includes(event.key)
            ) {
                event.preventDefault();
                open({
                    kind: event.key === "n" ? "create" : "settings",
                });
            }
        };
        window.addEventListener("keydown", key);
        return () => window.removeEventListener("keydown", key);
    }, [
        modal,
        tourOpen,
        state.selected,
        state.canEdit,
        state.history.supported,
        state.history.canUndo,
        state.history.canRedo,
    ]);
    useEffect(() => {
        if (!message) return;
        const timer = setTimeout(() => setMessage(""), 3000);
        return () => clearTimeout(timer);
    }, [message]);
    async function onSocket(socket) {
        if (["source", "output-send"].includes(socket.kind)) {
            setSelection(selection?.key === socket.key ? null : socket);
            return;
        }
        if (!selection) {
            setMessage(
                "Choose a source or output send first, or use Edit connections.",
            );
            return;
        }
        let action;
        if (selection.kind === "source" && socket.kind === "output-input")
            action = {
                type: "route",
                kind: "source",
                id: selection.id,
                channel: selection.channel,
                output: socket.channel,
            };
        if (selection.kind === "output-send" && socket.kind === "monitor-input")
            action = {
                type: "route",
                kind: "monitor",
                id: socket.id,
                channel: socket.channel,
                output: selection.channel,
            };
        if (!action) {
            setMessage(
                "Connect a source to an output, or an output send to a monitor.",
            );
            return;
        }
        if (
            await send({
                ...action,
                mode: "add",
            })
        ) {
            setSelection(null);
            setMessage("Connection saved.");
        }
    }
    const problem =
        state.error ||
        (!state.snapshot?.canCreateDocument &&
        state.snapshot?.runtime?.documentError
            ? "Your saved routing could not be read. LoopZ keeps any already-running routing and retries when a valid saved file becomes available. Your files are preserved."
            : "") ||
        (state.snapshot?.runtime?.controlError
            ? "The audio service needs attention. Your saved graph will be retried automatically."
            : "");
    return (
        <MeterUpdates.Provider value={!modal}>
            <div
                className={`app lz-window${state.platform === "windows" ? " lz-windows" : ""}`}
            >
                <header className="lz-titlebar">
                    <div className="lz-brand">
                        <LoopZMark />
                        <strong>LoopZ</strong>
                    </div>
                    <div className="lz-title-actions">
                        <Language compact />
                        {releaseUpdates.updates?.state === "available" && (
                            <Button
                                size="sm"
                                disabled={releaseUpdates.busy}
                                onClick={() =>
                                    releaseUpdates.action("open-update")
                                }
                            >
                                {t("Download update")}{" "}
                                {releaseUpdates.updates.version}
                            </Button>
                        )}
                        {state.history.supported && (
                            <>
                                <Button
                                    size="sm"
                                    aria-label="Undo routing change"
                                    disabled={
                                        !state.canEdit || !state.history.canUndo
                                    }
                                    onClick={() => moveHistory("undo")}
                                >
                                    {t("Undo")}
                                </Button>
                                <Button
                                    size="sm"
                                    aria-label="Redo routing change"
                                    disabled={
                                        !state.canEdit || !state.history.canRedo
                                    }
                                    onClick={() => moveHistory("redo")}
                                >
                                    {t("Redo")}
                                </Button>
                            </>
                        )}
                        <Button
                            size="sm"
                            icon
                            className="lz-sidebar-trigger"
                            data-tour="devices-menu"
                            aria-label={t("Show devices")}
                            onClick={() => setSidebar(!sidebar)}
                        >
                            ☰
                        </Button>
                        <Button
                            size="sm"
                            onClick={() =>
                                send({
                                    type: "theme",
                                    value:
                                        palette === "atelier"
                                            ? "night-studio"
                                            : "atelier",
                                })
                            }
                        >
                            {t("Appearance")}
                        </Button>
                        <Button
                            size="sm"
                            data-tour="settings"
                            onClick={(event) =>
                                open(
                                    {
                                        kind: "settings",
                                    },
                                    event.currentTarget,
                                )
                            }
                        >
                            <Icon name="settings" size={15} />
                            {" " + t("Settings")}
                        </Button>
                    </div>
                </header>
                <aside
                    className={`lz-sidebar ${sidebar ? "open" : ""}`}
                    aria-label={t("Virtual devices")}
                >
                    <div className="lz-sidebar-heading">
                        <span className="eyebrow">{t("Virtual devices")}</span>
                        <span>{state.devices.length} / 32</span>
                    </div>
                    <nav className="lz-device-list">
                        {state.devices.map((device) => (
                            <div
                                className={`lz-device-entry ${device.id === state.selected ? "selected" : ""}`}
                                key={device.id}
                            >
                                <button
                                    className="lz-device-select"
                                    aria-current={
                                        device.id === state.selected
                                            ? "true"
                                            : undefined
                                    }
                                    onClick={() => {
                                        send({
                                            type: "select",
                                            id: device.id,
                                        });
                                        setSidebar(false);
                                    }}
                                    aria-label={`Select ${device.name}`}
                                >
                                    <span className="lz-device-mark">
                                        <Icon name="interval" size={20} />
                                    </span>
                                    <span>
                                        <strong>{device.name}</strong>
                                        <small>
                                            <StatusDot
                                                tone={
                                                    device.enabled
                                                        ? health(
                                                              state,
                                                              device,
                                                          ) === "Routing"
                                                            ? "ok"
                                                            : "warn"
                                                        : "idle"
                                                }
                                            />
                                            {health(state, device)} ·{" "}
                                            {device.channels} ch
                                        </small>
                                        <Meter
                                            level={outputAmplitude(
                                                state,
                                                device.id,
                                                1,
                                            )}
                                            label={`${device.name} output`}
                                        />
                                    </span>
                                </button>
                                <span className="lz-device-power">
                                    <Switch
                                        checked={device.enabled}
                                        label={`${device.name} device enabled`}
                                        disabled={!state.canEdit}
                                        onChange={(enabled) =>
                                            send({
                                                type: "device",
                                                device: device.id,
                                                patch: {
                                                    enabled,
                                                },
                                            })
                                        }
                                    />
                                </span>
                            </div>
                        ))}
                    </nav>
                    <div className="lz-sidebar-bottom">
                        <Button
                            size="sm"
                            data-tour="new-device"
                            disabled={
                                !(state.canEdit || state.canCreate) ||
                                state.devices.length >= 32
                            }
                            onClick={(event) =>
                                open(
                                    {
                                        kind: "create",
                                    },
                                    event.currentTarget,
                                )
                            }
                        >
                            {state.platform === "windows"
                                ? t("+ New mix")
                                : t("+ New virtual device")}
                        </Button>
                        <p>
                            {t("Close this window to leave")}
                            <br />
                            {t("your engine running.")}
                        </p>
                    </div>
                </aside>
                <main className="lz-main">
                    <div className="lz-recovery">
                        {state.platform === "windows" &&
                            !tourOpen &&
                            d &&
                            state.connected && (
                                <div
                                    className="lz-windows-status"
                                    role="status"
                                >
                                    <div>
                                        <strong>
                                            {t(
                                                windowsMicrophone?.connected
                                                    ? "Microphone ready"
                                                    : "Set up your microphone",
                                            )}
                                        </strong>
                                        <p>
                                            {t(
                                                windowsMicrophone?.connected
                                                    ? "Choose this mix in your recording app."
                                                    : "Your mix is saved. Install its microphone to use it in other apps.",
                                            )}
                                        </p>
                                    </div>
                                    <Button
                                        size="sm"
                                        onClick={(event) =>
                                            open(
                                                { kind: "settings" },
                                                event.currentTarget,
                                            )
                                        }
                                    >
                                        {t("Settings")}
                                    </Button>
                                    {showWelcome && (
                                        <LinkButton onClick={startTour}>
                                            {t("Take a tour")}
                                        </LinkButton>
                                    )}
                                    {showWelcome && (
                                        <LinkButton onClick={dismissWelcome}>
                                            {t("Not now")}
                                        </LinkButton>
                                    )}
                                </div>
                            )}
                        {state.platform !== "windows" &&
                            state.previewNotice && (
                                <Banner heading="Windows preview">
                                    {state.previewNotice}
                                </Banner>
                            )}
                        {tourOpen && (
                            <Banner heading={t("Demo project")}>
                                {t(
                                    "Example levels only. No audio is captured or played, and your own project is unchanged.",
                                )}
                            </Banner>
                        )}
                        {showWelcome &&
                            !tourOpen &&
                            state.platform !== "windows" && (
                                <Banner
                                    heading={t("New to LoopZ?")}
                                    footer={
                                        <div className="lz-welcome-actions">
                                            <Button
                                                size="sm"
                                                onClick={startTour}
                                            >
                                                {t("Take a tour")}
                                            </Button>
                                            <LinkButton
                                                onClick={dismissWelcome}
                                            >
                                                {t("Not now")}
                                            </LinkButton>
                                        </div>
                                    }
                                >
                                    {t(
                                        "Learn how to combine sound and use your mix in other apps. You can replay this tour from Settings.",
                                    )}
                                </Banner>
                            )}
                        {backupNotice && (
                            <Banner
                                tone="warning"
                                heading={backupNotice.heading}
                            >
                                {backupNotice.detail}
                            </Banner>
                        )}
                        {setupIssue && (
                            <Banner
                                heading={setupIssue.heading}
                                footer={
                                    <LinkButton
                                        onClick={(event) =>
                                            open(
                                                {
                                                    kind: "settings",
                                                },
                                                event.currentTarget,
                                            )
                                        }
                                    >
                                        {t("Review setup")}
                                    </LinkButton>
                                }
                            >
                                {setupIssue.detail}
                            </Banner>
                        )}
                        {!setupIssue && !state.connected && (
                            <Banner
                                heading={
                                    state.background.detail &&
                                    !state.background.managed
                                        ? t("Audio couldn’t start")
                                        : "Connecting to LoopZ"
                                }
                                icon={<Icon name="replay" size={20} />}
                                footer={
                                    state.background.detail &&
                                    !state.background.managed ? (
                                        <LinkButton
                                            onClick={(event) =>
                                                open(
                                                    {
                                                        kind: "settings",
                                                    },
                                                    event.currentTarget,
                                                )
                                            }
                                        >
                                            {t("Review setup")}
                                        </LinkButton>
                                    ) : undefined
                                }
                            >
                                {t(
                                    state.background.detail ||
                                        "Your saved devices will reconnect automatically.",
                                )}
                            </Banner>
                        )}
                        {!setupIssue &&
                            state.connected &&
                            state.service === "driver" &&
                            state.platform !== "windows" &&
                            state.snapshot?.restoration?.phase !==
                                "no-document" && (
                                <Banner heading="Waiting for the audio service">
                                    Your desired routing stays saved. Virtual
                                    devices will appear when the audio service
                                    is available.
                                </Banner>
                            )}
                        {state.connected &&
                            state.service === "applying" &&
                            state.pending && (
                                <Banner heading="Applying saved changes">
                                    The engine keeps the last working graph
                                    until the new routing is confirmed.
                                </Banner>
                            )}
                        {problem && (
                            <Banner
                                tone="danger"
                                heading={
                                    state.uncertain
                                        ? "Review the current routing"
                                        : "This change needs attention"
                                }
                                footer={
                                    state.uncertain ? (
                                        <LinkButton
                                            onClick={async () => {
                                                setModal(null);
                                                await send({
                                                    type: "acknowledge",
                                                });
                                            }}
                                        >
                                            Show current routing before editing
                                        </LinkButton>
                                    ) : undefined
                                }
                            >
                                {problem}
                            </Banner>
                        )}
                    </div>
                    {d ? (
                        <>
                            <fieldset
                                className="lz-editing"
                                disabled={!state.canEdit}
                            >
                                <header className="lz-device-header">
                                    <div>
                                        <div className="lz-device-title">
                                            <h1>{d.name}</h1>
                                            <Button
                                                size="sm"
                                                icon
                                                aria-label={t("Device options")}
                                                onClick={(event) =>
                                                    open(
                                                        {
                                                            kind: "device",
                                                        },
                                                        event.currentTarget,
                                                    )
                                                }
                                            >
                                                <GearIcon />
                                            </Button>
                                        </div>
                                        <p>
                                            {t("Sources")}: {d.sources.length} ·{" "}
                                            {t("Output channels")}: {d.channels}
                                        </p>
                                    </div>
                                    <div className="lz-header-controls">
                                        <Switch
                                            checked={d.enabled}
                                            label={t("Device enabled")}
                                            onChange={(enabled) =>
                                                send({
                                                    type: "device",
                                                    patch: {
                                                        enabled,
                                                    },
                                                })
                                            }
                                        />
                                    </div>
                                </header>
                            </fieldset>
                            <div className="lz-toolbar">
                                <SegmentedControl
                                    aria-label="Workspace view"
                                    options={[
                                        {
                                            value: "routing",
                                            label: t("Routing"),
                                        },
                                        {
                                            value: "connections",
                                            label: t("Connections"),
                                        },
                                    ]}
                                    value={view}
                                    onChange={setView}
                                />
                                {selection && (
                                    <div
                                        className="lz-connection-hint"
                                        role="status"
                                    >
                                        <span>
                                            Channel {selection.channel}{" "}
                                            selected. Choose{" "}
                                            {selection.kind === "source"
                                                ? "an output input"
                                                : "a monitor input"}
                                            .
                                        </span>
                                        <Button
                                            size="sm"
                                            onClick={() => setSelection(null)}
                                        >
                                            {t("Cancel")}
                                        </Button>
                                    </div>
                                )}
                                <span className="lz-saved">
                                    {state.saving
                                        ? t("Saving\u2026")
                                        : !state.transportReady
                                          ? "Waiting for current state"
                                          : state.pending
                                            ? "Saved · awaiting activation"
                                            : t("Saved")}
                                </span>
                            </div>
                            <div className="lz-scroll">
                                {view === "routing" ? (
                                    <fieldset
                                        className="lz-editing"
                                        disabled={!state.canEdit}
                                    >
                                        <Routing
                                            state={state}
                                            device={d}
                                            send={send}
                                            open={open}
                                            selection={selection}
                                            onSocket={onSocket}
                                            onClearSelection={() =>
                                                setSelection(null)
                                            }
                                        />
                                    </fieldset>
                                ) : (
                                    <ConnectionList
                                        state={state}
                                        device={d}
                                        send={send}
                                        open={open}
                                    />
                                )}
                            </div>
                        </>
                    ) : (
                        <div className="lz-empty">
                            <LoopZMark size={48} />
                            <h2>
                                {state.connected
                                    ? t("Connect your sound.")
                                    : "Your audio workspace"}
                            </h2>
                            <p>
                                {state.canEdit || state.canCreate
                                    ? t(
                                          "Create a device to combine apps and inputs.",
                                      )
                                    : state.connected
                                      ? "Your routing document needs attention before editing."
                                      : t(
                                            "Once audio is connected, your saved devices will appear here.",
                                        )}
                            </p>
                            <Button
                                variant="primary"
                                data-tour="new-device"
                                disabled={!(state.canEdit || state.canCreate)}
                                onClick={(event) =>
                                    open(
                                        {
                                            kind: "create",
                                        },
                                        event.currentTarget,
                                    )
                                }
                            >
                                {t("Create your first device")}
                            </Button>
                        </div>
                    )}
                </main>
                <footer className="lz-footer">
                    <div className="lz-footer-status">
                        <StatusDot
                            tone={
                                !state.connected
                                    ? "warn"
                                    : d?.enabled
                                      ? state.transportReady &&
                                        !state.meterExpired &&
                                        state.snapshot?.runtime?.restored
                                          ? "ok"
                                          : "warn"
                                      : "idle"
                            }
                        />
                        <span>
                            {tourOpen
                                ? t("Demo project")
                                : !state.connected
                                  ? t("Reconnecting")
                                  : d
                                    ? health(state, d)
                                    : t("No virtual devices")}
                        </span>
                    </div>
                    <div className="lz-footer-actions">
                        {d && (
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
                                {t("Edit connections")}
                            </LinkButton>
                        )}
                        <Button
                            size="sm"
                            icon
                            aria-label={t("Keyboard shortcuts")}
                            onClick={(event) =>
                                open(
                                    {
                                        kind: "shortcuts",
                                    },
                                    event.currentTarget,
                                )
                            }
                        >
                            {state.platform === "windows" ? "?" : "⌘"}
                        </Button>
                    </div>
                </footer>
            </div>
            {modal && (
                <Dialogs
                    releaseUpdates={releaseUpdates}
                    modal={modal}
                    state={state}
                    send={send}
                    close={() => setModal(null)}
                    setModal={(next) =>
                        setModal({
                            device: modal.device,
                            ...next,
                        })
                    }
                    returnFocus={returnFocus}
                    transport={transport}
                    refresh={refresh}
                    startTour={startTour}
                />
            )}
            {tourOpen && (
                <StartupTour deviceName={d?.name || ""} onFinish={finishTour} />
            )}
            <div className="sr-only" role="status" aria-live="polite">
                {message || state.error}
            </div>
            {message && (
                <div className="lz-feedback" role="status">
                    {message}
                </div>
            )}
        </MeterUpdates.Provider>
    );
}
