// Socket identifiers are stable UI keys, independent of signal availability.
export function parseSocket(key) {
    if (!key) return null;
    const parts = key.split(":");
    const kind = parts.shift(),
        channel = parts.pop(),
        id = parts.join(":");
    return { key, kind, id, channel: Number(channel) };
}
export function connectionAction(a, b) {
    if (!a || !b) return null;
    const pair = [a, b];
    const source = pair.find((s) => s.kind === "source");
    const output = pair.find((s) => s.kind === "output-input");
    if (source && output)
        return {
            type: "route",
            kind: "source",
            id: source.id,
            channel: source.channel,
            output: output.channel,
        };
    const monitor = pair.find((s) => s.kind === "monitor-input");
    const send = pair.find((s) => s.kind === "output-send");
    if (monitor && send)
        return {
            type: "route",
            kind: "monitor",
            id: monitor.id,
            channel: monitor.channel,
            output: send.channel,
        };
    return null;
}
