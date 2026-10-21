export function engineFixture() {
    return {
        version: 1,
        connection: { connected: true, ready: true },
        inventory: {
            applications: [{ bundle: "example.player", name: "Player" }],
            devices: [
                {
                    uid: "input",
                    name: "Microphone",
                    inputChannels: 1,
                    outputChannels: 0,
                },
                {
                    uid: "phones",
                    name: "Headphones",
                    inputChannels: 0,
                    outputChannels: 2,
                },
            ],
        },
        snapshot: {
            version: 1,
            revision: { epoch: "engine-one", generation: "9007199254740993" },
            canCreateDocument: false,
            restoration: {
                phase: "restored",
                desiredGeneration: "9007199254740993",
                activeGeneration: "9007199254740993",
            },
            document: {
                version: 2,
                devices: [
                    {
                        id: "mix",
                        name: "Mix",
                        sampleRate: 48000,
                        formatRequestReceipt: "native:8",
                        routing: {
                            outputChannels: 2,
                            gain: 1,
                            enabled: true,
                            muted: false,
                            safetyLimiter: true,
                            sources: [
                                {
                                    id: "app",
                                    channels: 2,
                                    gain: 0.5,
                                    enabled: true,
                                    muted: false,
                                    connections: [
                                        {
                                            sourceChannel: 0,
                                            outputChannel: 0,
                                            gain: 0.25,
                                        },
                                        {
                                            sourceChannel: 1,
                                            outputChannel: 1,
                                            gain: 0.75,
                                        },
                                    ],
                                },
                            ],
                        },
                        bindings: [
                            {
                                id: "app",
                                kind: "application",
                                selector: "example.player",
                                muteOriginal: true,
                            },
                        ],
                        monitors: [
                            {
                                id: "monitor",
                                deviceSelector: "phones",
                                channels: 2,
                                gain: 0.5,
                                enabled: true,
                                muted: false,
                                connections: [
                                    {
                                        sourceChannel: 1,
                                        outputChannel: 0,
                                        gain: 0.2,
                                    },
                                ],
                            },
                        ],
                    },
                ],
            },
            runtime: {
                metersFresh: true,
                restored: true,
                devices: [
                    {
                        id: "mix",
                        active: true,
                        meter: { valid: true, peak: [0.2, 0.3] },
                        sources: [
                            {
                                id: "app",
                                online: true,
                                meter: { valid: true, peak: [0.7, 0.6] },
                            },
                        ],
                    },
                ],
                sources: [
                    {
                        kind: "application",
                        selector: "example.player",
                        connected: true,
                        complete: true,
                    },
                ],
                monitors: [
                    {
                        device: "mix",
                        monitor: "monitor",
                        connected: true,
                        complete: true,
                        meter: { valid: true, peak: [0.12, 0.09] },
                    },
                ],
            },
        },
    };
}
