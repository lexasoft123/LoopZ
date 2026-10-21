import { project } from "./model.js";
import { t } from "./i18n.js";

// A display-only document. It never enters EngineStore or the native transport.
// Fixed example levels make the signal path visible without opening any audio.
export function createTourDemoState(theme) {
    const device = "tour-demo-mix";
    const microphone = "tour-demo-microphone";
    const headphones = "tour-demo-headphones";
    const stereo = [
        { sourceChannel: 0, outputChannel: 0, gain: 1 },
        { sourceChannel: 1, outputChannel: 1, gain: 1 },
    ];
    const meter = (peak) => ({ valid: true, peak });
    const envelope = {
        version: 1,
        connection: { connected: true, ready: true },
        inventory: {
            applications: [],
            devices: [
                {
                    uid: microphone,
                    name: t("Demo microphone"),
                    inputChannels: 1,
                    outputChannels: 0,
                },
                {
                    uid: headphones,
                    name: t("Demo headphones"),
                    inputChannels: 0,
                    outputChannels: 2,
                },
            ],
        },
        snapshot: {
            version: 1,
            revision: { epoch: "tour-demo", generation: "1" },
            history: { canUndo: false, canRedo: false },
            restoration: {
                phase: "restored",
                activeGeneration: "1",
                desiredGeneration: "1",
            },
            document: {
                version: 2,
                devices: [
                    {
                        id: device,
                        name: t("Demo Mix"),
                        sampleRate: 48000,
                        routing: {
                            outputChannels: 2,
                            gain: 1,
                            enabled: true,
                            muted: false,
                            safetyLimiter: true,
                            sources: [
                                {
                                    id: "demo-mic",
                                    channels: 1,
                                    gain: 1,
                                    enabled: true,
                                    muted: false,
                                    connections: [
                                        {
                                            sourceChannel: 0,
                                            outputChannel: 0,
                                            gain: 1,
                                        },
                                        {
                                            sourceChannel: 0,
                                            outputChannel: 1,
                                            gain: 1,
                                        },
                                    ],
                                },
                                {
                                    id: "demo-pass",
                                    channels: 2,
                                    gain: 1,
                                    enabled: true,
                                    muted: false,
                                    connections: stereo,
                                },
                            ],
                        },
                        bindings: [
                            {
                                id: "demo-mic",
                                kind: "input-device",
                                selector: microphone,
                                muteOriginal: false,
                            },
                            {
                                id: "demo-pass",
                                kind: "pass-through",
                                selector: "",
                                muteOriginal: false,
                            },
                        ],
                        monitors: [
                            {
                                id: "demo-monitor",
                                deviceSelector: headphones,
                                channels: 2,
                                gain: 0.7,
                                enabled: true,
                                muted: false,
                                connections: stereo,
                            },
                        ],
                    },
                ],
            },
            runtime: {
                // These describe the example only, never real permission status.
                permissions: { microphone: "authorized" },
                metersFresh: true,
                restored: true,
                devices: [
                    {
                        id: device,
                        active: true,
                        meter: meter([0.58, 0.53]),
                        sources: [
                            {
                                id: "demo-mic",
                                online: true,
                                meter: meter([0.45]),
                            },
                            {
                                id: "demo-pass",
                                online: true,
                                meter: meter([0.22, 0.18]),
                            },
                        ],
                    },
                ],
                sources: [
                    {
                        kind: "input-device",
                        selector: microphone,
                        demanded: true,
                        connected: true,
                        complete: true,
                    },
                ],
                monitors: [
                    {
                        device,
                        monitor: "demo-monitor",
                        connected: true,
                        complete: true,
                        meter: meter([0.38, 0.35]),
                    },
                ],
            },
        },
    };
    return {
        ...project(
            envelope,
            { theme, selected: device },
            { meterExpired: false },
        ),
        demo: true,
    };
}
