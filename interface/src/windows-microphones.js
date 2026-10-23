// Metadata acceptance alone does not prove that a microphone can carry audio.
export function windowsMicrophoneStatus(state) {
    const setup = state.microphoneSetup;
    if (!state.connected || !state.transportReady || state.meterExpired)
        return { message: "Waiting for the audio engine.", detail: "" };
    if (!setup?.enabled)
        return {
            message: "Install or repair microphones to enable automatic setup.",
            detail: setup?.error || "",
        };
    if (!setup.authoritative)
        return {
            message: state.devices.length
                ? "Save a mix change to resume microphone setup after recovery."
                : "Create a mix to set up its microphone.",
            detail: "",
        };
    const devices = Object.values(setup.devices || {});
    if (devices.some((device) => device.phase === "reboot_required"))
        return {
            message: "Restart Windows to finish microphone setup.",
            detail: "",
        };
    const detail =
        setup.error || devices.find((device) => device.error)?.error || "";
    if (detail || devices.some((device) => device.phase === "error"))
        return {
            message:
                "Microphone setup needs attention. Try installing or repairing microphones.",
            detail,
        };
    const current = setup.generation === state.snapshot?.revision?.generation;
    const runtime = state.snapshot?.runtime?.devices || [];
    const ready =
        current &&
        setup.accepted &&
        devices.every(
            (device) => device.phase === "ready" && !device.removing,
        ) &&
        state.devices.every(
            (device) =>
                setup.devices?.[device.id]?.phase === "ready" &&
                runtime.some(
                    (row) => row.id === device.id && row.microphone?.connected,
                ),
        );
    return {
        message: ready
            ? "Microphones are ready."
            : "Setting up microphones. Your mixes are saved.",
        detail: "",
    };
}
