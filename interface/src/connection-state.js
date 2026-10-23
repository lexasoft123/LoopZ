import { t } from "./i18n.js";
const nativeKinds = {
    app: "application",
    hardware: "input-device",
};
const condition = (kind, label, detail = "", action = null) => ({
    kind,
    label: t(label),
    detail,
    action,
    available: ["available", "idle", "disabled"].includes(kind),
});
export function microphoneAccess(permission) {
    switch (permission) {
        case "authorized":
            return "Access granted to LoopZ Engine.";
        case "denied":
            return "Microphone access is turned off for LoopZ Engine. Enable it in Privacy & Security. Saved inputs reconnect automatically.";
        case "restricted":
            return "Microphone access is restricted on this Mac. Review the Mac’s privacy restrictions before reconnecting inputs.";
        case "not-determined":
            return "Microphone access has not been granted yet. Allow LoopZ Engine’s microphone request to connect saved inputs.";
        default:
            return "The current microphone permission could not be checked. Review LoopZ Engine’s access in Privacy & Security.";
    }
}
function unavailableState(state) {
    if (!state.connected || !state.transportReady || state.meterExpired)
        return condition(
            "unknown",
            "Status unavailable",
            "Waiting for current engine status. Saved connections stay in place.",
        );
    if (state.service === "driver")
        return condition(
            "waiting",
            "Waiting for audio service",
            "LoopZ restores saved connections when the audio service returns.",
        );
    if (state.pending)
        return condition(
            "waiting",
            "Applying changes",
            "Waiting for the saved graph to become active.",
        );
    return null;
}
function virtualCondition(state, source, context) {
    const target = state.devices.find(
        (device) => device.id === source.deviceId,
    );
    if (!target)
        return condition(
            "waiting",
            "Waiting for source device",
            "The saved virtual device is unavailable. Its connections stay saved.",
        );
    const show = (device) => ({
        kind: "select-device",
        label: t("Show device"),
        device,
    });
    if (!target.enabled)
        return condition(
            "waiting",
            "Source device disabled",
            `Enable ${target.name} to restore this source.`,
            show(target.id),
        );
    // Native virtual input stays online while a failed physical/app branch
    // supplies silence. Follow saved signal edges to expose that partial loss,
    // without interpreting quiet samples or a monitor failure as input loss.
    const walk = context || {
        active: new Set(),
        checked: new Map(),
    };
    if (walk.active.has(target.id))
        return condition(
            "unknown",
            "Source routing unavailable",
            "Review the source device’s saved connections.",
            show(target.id),
        );
    if (walk.checked.has(target.id)) return walk.checked.get(target.id);
    walk.active.add(target.id);
    let issue = null;
    for (const input of target.sources) {
        if (
            !input.enabled ||
            !input.connections.some((wire) => wire.gain !== 0)
        )
            continue;
        const upstream = sourceCondition(state, input, walk);
        if (!upstream.available) {
            issue = upstream;
            break;
        }
    }
    walk.active.delete(target.id);
    const result = issue
        ? condition(
              "partial",
              "Upstream source unavailable",
              `Some audio feeding ${target.name} is unavailable. Connected audio continues, and saved connections stay in place.`,
              issue.action?.kind === "select-device"
                  ? issue.action
                  : show(target.id),
          )
        : condition("available", t("Available"));
    walk.checked.set(target.id, result);
    return result;
}
export function sourceCondition(state, source, context) {
    if (!source.enabled) return condition("disabled", t("Disabled"));
    const unavailable = unavailableState(state);
    if (unavailable) return unavailable;
    if (source.kind === "virtual")
        return virtualCondition(state, source, context);
    if (source.kind === "pass") return condition("available", t("Available"));
    const status = state.snapshot?.runtime?.sources?.find(
        (item) =>
            item.kind === nativeKinds[source.kind] &&
            item.selector === source.selector,
    );
    if (status?.demanded === false)
        return condition("idle", t("Ready when used"));
    // Permission is current metadata. It takes precedence over a capture's last
    // successful status while the control owner is closing a revoked stream.
    if (
        source.kind === "hardware" &&
        ["denied", "restricted", "not-determined"].includes(
            state.permissions?.microphone,
        )
    )
        return condition(
            "permission",
            state.permissions?.microphone === "restricted"
                ? "Microphone access restricted"
                : "Microphone access required",
            microphoneAccess(state.permissions?.microphone),
            {
                kind: "settings",
                label: t("Review access"),
            },
        );
    if (status?.connected && status.complete)
        return condition("available", t("Available"));
    if (status?.connected && source.kind === "app")
        return condition(
            "partial",
            "Application audio incomplete",
            "Some application processes are not connected. LoopZ keeps trying while connected audio continues.",
            {
                kind: "retry",
                label: t("Retry now"),
            },
        );
    if (status?.connected)
        return condition(
            "partial",
            "Some channels unavailable",
            "The input has fewer available channels than this source needs. Available channels keep working; the remaining connections stay saved.",
            {
                kind: "source-options",
                label: "Review source",
            },
        );
    return condition(
        "waiting",
        source.kind === "app"
            ? "Waiting for application audio"
            : "Waiting for input device",
        source.kind === "app"
            ? "Open the application and review any audio-recording permission request. LoopZ reconnects automatically."
            : "Reconnect or select the input device. LoopZ keeps trying and retains its saved connections.",
        status?.demanded
            ? {
                  kind: "retry",
                  label: t("Retry now"),
              }
            : null,
    );
}
export function monitorCondition(state, monitor) {
    if (!monitor.enabled) return condition("disabled", t("Monitor disabled"));
    const unavailable = unavailableState(state);
    if (unavailable) return unavailable;
    const status = state.snapshot?.runtime?.monitors?.find(
        (item) => item.device === monitor.owner && item.monitor === monitor.id,
    );
    return status?.connected && status.complete
        ? condition("available", t("Available"))
        : condition(
              "waiting",
              "Waiting for output device",
              "Reconnect or select the output device. LoopZ restores listening automatically and retains its connections.",
          );
}
