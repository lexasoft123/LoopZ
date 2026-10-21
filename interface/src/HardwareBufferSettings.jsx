import { t } from "./i18n.js";
import { bufferLatencyLabel } from "./hardware-buffer-model.js";
import React, { useEffect, useState } from "react";
const sizes = [16, 32, 64, 128, 256, 512, 1024, 2048, 4096];
export function bufferChoices(device) {
    const minimum = device?.minimumBufferFrames;
    const maximum = device?.maximumBufferFrames;
    if (
        !Number.isInteger(minimum) ||
        !Number.isInteger(maximum) ||
        minimum < 1 ||
        maximum < minimum
    )
        return [];
    return sizes.filter((frames) => frames >= minimum && frames <= maximum);
}
export default function HardwareBufferSettings({
    inventory,
    transport,
    refresh,
}) {
    const targets = (inventory.routingBufferDevices || []).map((uid) => {
        const device = (inventory.devices || []).find(
            (item) => item.uid === uid,
        );
        const preference = (inventory.bufferPreferences || []).find(
            (item) => item.uid === uid,
        );
        return {
            uid,
            device,
            preference,
        };
    });
    const connected = targets.filter((target) => target.device);
    const choices = sizes.filter((size) =>
        connected.every(({ device }) => bufferChoices(device).includes(size)),
    );
    const actual = [
        ...new Set(connected.map(({ device }) => device.bufferFrames)),
    ];
    const saved = inventory.routingBufferFrames || 0;
    const [busy, setBusy] = useState(false);
    const [accepted, setAccepted] = useState(null);
    const [error, setError] = useState("");
    useEffect(() => setAccepted(null), [saved]);
    const frames = accepted ?? (saved || (actual.length === 1 ? actual[0] : 0));
    const available =
        inventory.routingBufferSettings === true &&
        inventory.bufferSettingsAvailable !== false;
    const change = async (value) => {
        if (
            busy ||
            !available ||
            inventory.devicesFresh !== true ||
            !choices.includes(value)
        )
            return;
        setBusy(true);
        setError("");
        try {
            const result = await transport({
                version: 1,
                operation: "device-buffer",
                buffer: {
                    frames: value,
                },
            });
            if (!result?.ok)
                throw new Error(
                    result?.detail ||
                        "The hardware buffer could not be changed.",
                );
            setAccepted(value);
        } catch (failure) {
            setError(
                failure.message || "The hardware buffer could not be changed.",
            );
        } finally {
            try {
                await refresh();
            } finally {
                setBusy(false);
            }
        }
    };
    return (
        <section
            aria-label={t("Hardware buffer")}
            className="lz-hardware-buffer"
        >
            <strong>{t("Hardware buffer")}</strong>
            <p>
                {t(
                    "Applies immediately to all hardware inputs and monitors used by your mixes. Smaller buffers reduce audio latency and use more CPU. Changing this can briefly interrupt other apps using these devices.",
                )}
            </p>
            <label className="lz-field">
                {t("Buffer size")}
                <select
                    aria-label={t("Hardware buffer size")}
                    value={frames}
                    disabled={
                        busy ||
                        !available ||
                        inventory.devicesFresh !== true ||
                        !!inventory.bufferSettingsError ||
                        !choices.length
                    }
                    onChange={(event) => change(Number(event.target.value))}
                >
                    {!frames && (
                        <option value={0} disabled>
                            {actual.length > 1
                                ? t("Mixed buffer sizes")
                                : t("Choose a buffer size")}
                        </option>
                    )}
                    {!!frames && !choices.includes(frames) && (
                        <option value={frames} disabled>
                            {frames} frames (saved)
                        </option>
                    )}
                    {choices.map((size) => (
                        <option key={size} value={size}>
                            {bufferLatencyLabel(
                                size,
                                connected.map(({ device }) => device),
                            )}
                        </option>
                    ))}
                </select>
            </label>
            <p>
                {t(
                    "Target latency is for one hardware buffer at the devices\u2019 current sample rates. Total sound delay also includes device and routing processing.",
                )}
            </p>
            {busy && (
                <p role="status">{t("Applying to all used devices\u2026")}</p>
            )}
            <ul>
                {targets.map(({ uid, device, preference }) => (
                    <li key={uid}>
                        {device?.name || uid}:{" "}
                        {saved && preference?.applied
                            ? `${saved} frames`
                            : device
                              ? `${device.bufferFrames} frames`
                              : t("Unavailable")}
                        {saved > 0 &&
                            !preference?.applied &&
                            ` · ${preference?.error || t("Waiting to apply saved buffer")}`}
                    </li>
                ))}
            </ul>
            {!targets.length && (
                <p>
                    {t(
                        "Your choice will apply when you add a hardware input or monitor.",
                    )}
                </p>
            )}
            {inventory.bufferSettingsPersistence && (
                <p>
                    {t(
                        "One saved choice restores when the Engine restarts, a device reconnects, or you add another input or monitor.",
                    )}
                </p>
            )}
            {inventory.bufferSettingsError && (
                <p role="alert">
                    Saved audio settings need recovery before they can be
                    changed.
                </p>
            )}
            {!available && (
                <p>The Engine must be connected and support buffer settings.</p>
            )}
            {error && <p role="alert">{error}</p>}
        </section>
    );
}
