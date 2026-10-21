export const tourStorageKey = "loopz-tour-v1";

export function tourSeen(storage) {
    try {
        return (
            (storage || globalThis.localStorage).getItem(tourStorageKey) ===
            "seen"
        );
    } catch {
        return false;
    }
}

export function rememberTour(storage) {
    try {
        (storage || globalThis.localStorage).setItem(tourStorageKey, "seen");
    } catch {
        // The current window still remembers dismissal if storage is unavailable.
    }
}

export function tourSteps() {
    const steps = [
        {
            title: "Your sound, in one place",
            body: "Explore this demo mix with a microphone, Pass-through and headphones. The example shows how sound flows into a virtual device and out to other apps. No audio is captured or played. Your own project stays unchanged.",
        },
        {
            target: '[data-tour="new-device"], [data-tour="devices-menu"]',
            title: "Create a virtual device",
            body: "Open the device list and choose New virtual device. Name your mix, for example Podcast or Guitar. Other apps will see “LoopZ: Podcast” or “LoopZ: Guitar”. Each device has its own sources and connections.",
        },
    ];
    steps.push(
        {
            target: '[data-od-id="sources"] .lz-column-heading',
            title: "Choose what goes in",
            body: "Use + in Sources to add an app, microphone, audio interface or another virtual device. The meters show whether each channel is receiving sound.",
        },
        {
            target: '[data-tour="pass-through"] .lz-node-heading',
            title: "When to use Pass-through",
            body: "Pass-through receives audio from apps that choose your LoopZ device as their speaker or output. For a microphone or instrument alone, you can remove it. You can also add an app directly as a source.",
        },
        {
            target: '[data-od-id="outputs"] .lz-first-pair .lz-output-rows',
            title: "Connect your channels",
            body: "Drag a circle beside a source channel to a circle beside an output channel. Connected sounds are mixed together. Channels 1 and 2 normally carry left and right stereo sound.",
        },
        {
            target: '[data-od-id="monitors"] .lz-column-heading',
            title: "Hear your mix, if you want",
            body: "Add headphones or speakers under Monitors to listen to the mix. Monitoring is optional: other apps still receive your virtual device without a monitor. Use headphones with a microphone to avoid feedback.",
        },
        {
            target: '[data-od-id="outputs"] .lz-column-heading',
            title: "Use your mix in another app",
            body: "In Zoom, a recorder or a streaming app, choose “LoopZ: {name}” as the microphone or audio input. LoopZ sends the connected output channels to that app.",
        },
    );
    steps.push({
        target: '[data-tour="settings"]',
        title: "Help is always here",
        body: "Settings contains audio setup, buffer size and language. Replay this demo here whenever you need a refresher. Choose Got it to return to your own workspace.",
    });
    return steps;
}

// Place the callout beside the highlighted control when there is room. On small
// windows it stays inside the viewport and can scroll independently.
export function tourPlacement(rect, viewport, card) {
    const gap = 16;
    const width = Math.min(380, viewport.width - gap * 2);
    const height = Math.min(card.height, viewport.height - gap * 2);
    const clamp = (value, maximum) => Math.max(gap, Math.min(value, maximum));
    if (!rect)
        return {
            left: Math.max(gap, (viewport.width - width) / 2),
            top: Math.max(gap, (viewport.height - height) / 2),
            width,
        };
    let left = rect.left;
    let top = rect.bottom + gap;
    if (top + height > viewport.height - gap) {
        if (rect.top - height - gap >= gap) top = rect.top - height - gap;
        else if (rect.right + width + gap <= viewport.width - gap) {
            left = rect.right + gap;
            top = rect.top;
        } else {
            left = rect.left - width - gap;
            top = rect.top;
        }
    }
    return {
        left: clamp(left, viewport.width - width - gap),
        top: clamp(top, viewport.height - height - gap),
        width,
    };
}
