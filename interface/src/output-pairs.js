// Review data only. The portable reducer owns the actual channel/wire edit.
export function outputPairRemoval(state, deviceId, start) {
    const selected = state.devices.find((device) => device.id === deviceId);
    if (
        !selected ||
        selected.channels <= 2 ||
        !Number.isInteger(start) ||
        start < 1 ||
        start % 2 !== 1 ||
        start >= selected.channels
    )
        throw new Error(
            "Choose an existing output pair. At least one pair must remain.",
        );
    const first = start - 1;
    const removed = (channel) => channel === first || channel === first + 1;
    const records = [];
    const dependents = new Set();
    let removedConnections = 0;
    let shiftedConnections = 0;
    const inspect = (device, node, input, output, kind) => {
        records.push([
            device.id,
            kind,
            node.id,
            node.channels,
            node.kind,
            node.selector,
            node.connections.map((wire) => [
                wire.sourceChannel,
                wire.outputChannel,
                wire.gain,
            ]),
        ]);
        for (const wire of node.connections) {
            if (
                (input && removed(wire.sourceChannel)) ||
                (output && removed(wire.outputChannel))
            )
                removedConnections++;
            else if (
                (input && wire.sourceChannel > first + 1) ||
                (output && wire.outputChannel > first + 1)
            )
                shiftedConnections++;
        }
    };
    for (const device of state.devices) {
        const own = device.id === deviceId;
        for (const source of device.sources) {
            const dependent =
                source.kind === "virtual" && source.deviceId === deviceId;
            const input = (own && source.kind === "pass") || dependent;
            if (dependent) dependents.add(device.id);
            if (own || input) inspect(device, source, input, own, "source");
        }
        if (own)
            for (const monitor of device.monitors)
                inspect(device, monitor, true, false, "monitor");
    }
    return {
        start,
        channels: selected.channels - 2,
        removedConnections,
        shiftedConnections,
        dependentDevices: dependents.size,
        basis: JSON.stringify([deviceId, selected.channels, start, records]),
    };
}
