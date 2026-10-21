import { t } from "./i18n.js";
import React, { useEffect, useState } from "react";
import { Banner, Button } from "@singz/ui";
import {
    endpointChoices,
    endpointWiresRemoved,
    endpointLayout,
} from "./model.js";
export default function EndpointEditor({
    state,
    device,
    node,
    kind,
    writable,
    send,
    transport,
    refresh,
}) {
    const [selector, setSelector] = useState(node.selector);
    const [channels, setChannels] = useState(node.channels);
    const [confirmation, setConfirmation] = useState(null);
    const [extraApp, setExtraApp] = useState(null);
    const [error, setError] = useState("");
    useEffect(() => {
        setSelector(node.selector);
        setChannels(node.channels);
        setConfirmation(null);
    }, [node.id, node.selector, node.channels]);
    const choices = endpointChoices(state, device, node, kind, extraApp);
    const choice = choices.find((item) => item.selector === selector);
    const adjustable = kind === "monitor" || node.kind === "hardware";
    const count = adjustable
        ? channels
        : node.kind === "app"
          ? 2
          : choice?.channels || node.channels;
    const removed = endpointWiresRemoved(node, kind, count);
    const layout = endpointLayout(node);
    const proposal = JSON.stringify([layout, selector, count]);
    const confirming = confirmation === proposal;
    const changed = selector !== node.selector || count !== node.channels;
    const apply = async () => {
        if (removed && !confirming) {
            setConfirmation(proposal);
            return;
        }
        if (
            await send({
                type: `${kind}-endpoint`,
                device: device.id,
                id: node.id,
                selector,
                channels: count,
                expectedLayout: layout,
            })
        )
            setConfirmation(null);
    };
    return (
        <fieldset className="lz-route-fieldset" disabled={!writable}>
            <legend>
                {kind === "monitor" ? "Listening device" : "Source selection"}
            </legend>
            <p className="modal-body">
                Gain, mute and valid connections stay saved when you change the
                selection.
            </p>
            {state.inventory.devicesError &&
                (kind === "monitor" || node.kind === "hardware") && (
                    <Banner tone="warning" heading="Device list unavailable">
                        {state.inventory.devicesError}
                    </Banner>
                )}
            <label className="lz-field">
                {kind === "monitor"
                    ? t("Output device")
                    : node.kind === "app"
                      ? t("Application")
                      : node.kind === "virtual"
                        ? t("Virtual device")
                        : t("Input device")}
                <select
                    aria-label={
                        kind === "monitor"
                            ? "Monitor output device"
                            : node.kind === "app"
                              ? "Source application"
                              : node.kind === "virtual"
                                ? "Source virtual device"
                                : "Source input device"
                    }
                    value={selector}
                    onChange={(event) => {
                        setSelector(event.target.value);
                        setConfirmation(null);
                    }}
                >
                    {choices.map((item) => (
                        <option
                            key={item.selector}
                            value={item.selector}
                            disabled={item.blocked}
                        >
                            {item.name}
                            {item.blocked
                                ? " · Would create a routing loop"
                                : ""}
                        </option>
                    ))}
                </select>
            </label>
            {node.kind === "app" && kind === "source" && (
                <Button
                    size="sm"
                    onClick={async () => {
                        setError("");
                        try {
                            const app = await transport({
                                version: 1,
                                operation: "choose-application",
                            });
                            if (app) {
                                setExtraApp(app);
                                setSelector(app.bundle);
                                setConfirmation(null);
                                await refresh();
                            }
                        } catch {
                            setError("The application could not be selected.");
                        }
                    }}
                >
                    {t("Choose another application\u2026")}
                </Button>
            )}
            {adjustable ? (
                <label className="lz-field">
                    {kind === "monitor"
                        ? "Monitor channels"
                        : t("Input channels")}
                    <select
                        aria-label={
                            kind === "monitor"
                                ? "Monitor channel count"
                                : "Source input channel count"
                        }
                        value={channels}
                        onChange={(event) => {
                            setChannels(+event.target.value);
                            setConfirmation(null);
                        }}
                    >
                        {Array.from(
                            {
                                length: 64,
                            },
                            (_, i) => i + 1,
                        ).map((value) => (
                            <option key={value} value={value}>
                                {value} channel{value === 1 ? "" : "s"}
                            </option>
                        ))}
                    </select>
                </label>
            ) : (
                <p className="modal-body">
                    {count} channels
                    {node.kind === "app"
                        ? " · stereo capture"
                        : " · follows the selected device"}
                </p>
            )}
            {choice?.channels && adjustable && channels > choice.channels && (
                <p className="modal-body">
                    This device currently has {choice.channels} channels. Extra
                    saved channels wait for a device with enough channels.
                </p>
            )}
            {choice?.unavailable && (
                <p className="modal-body">
                    This saved selection is unavailable. LoopZ keeps retrying
                    it.
                </p>
            )}
            {error && (
                <Banner tone="danger" heading="Selection unavailable">
                    {error}
                </Banner>
            )}
            {confirming && removed > 0 ? (
                <Banner
                    tone="warning"
                    heading={`Remove ${removed} connection${removed === 1 ? "" : "s"}?`}
                    footer={
                        <div className="lz-confirm-actions">
                            <Button
                                size="sm"
                                onClick={() => {
                                    setSelector(node.selector);
                                    setChannels(node.channels);
                                    setConfirmation(null);
                                }}
                            >
                                Keep current selection
                            </Button>
                            <Button
                                size="sm"
                                variant="danger"
                                disabled={!choice || choice.blocked}
                                onClick={apply}
                            >
                                Change and remove connections
                            </Button>
                        </div>
                    }
                >
                    The new channel layout removes connections on channels that
                    no longer exist.
                </Banner>
            ) : (
                <Button
                    size="sm"
                    disabled={!changed || !choice || choice.blocked}
                    onClick={apply}
                >
                    {kind === "monitor"
                        ? "Apply monitor selection"
                        : "Apply source selection"}
                </Button>
            )}
        </fieldset>
    );
}
