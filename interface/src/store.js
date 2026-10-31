import { commandsFor, project } from "./model.js";
import { isAppearance, normalizeAppearance } from "./appearance.js";
import { startVisiblePolling } from "./visibility.js";

export class EngineStore {
    constructor(transport, preferences = {}, savePreferences = () => {}) {
        this.transport = transport;
        if (
            !preferences ||
            typeof preferences !== "object" ||
            Array.isArray(preferences)
        )
            preferences = {};
        this.preferences = {
            selected:
                typeof preferences.selected === "string"
                    ? preferences.selected
                    : null,
            theme: normalizeAppearance(preferences.theme),
            appNames:
                preferences.appNames && typeof preferences.appNames === "object"
                    ? preferences.appNames
                    : {},
            endpointNames:
                preferences.endpointNames &&
                typeof preferences.endpointNames === "object"
                    ? preferences.endpointNames
                    : {},
        };
        this.savePreferences = savePreferences;
        this.flags = {
            saving: false,
            uncertain: false,
            error: "",
            meterExpired: true,
        };
        this.envelope = null;
        this.listeners = new Set();
        this.visible = true;
        this.visibilityGeneration = 0;
        this.state = project(null, this.preferences, this.flags);
    }
    getSnapshot = () => this.state;
    subscribe = (listener) => {
        this.listeners.add(listener);
        return () => this.listeners.delete(listener);
    };
    publish() {
        if (!this.visible) return;
        const revision = this.envelope?.snapshot?.revision;
        const key = JSON.stringify([
            revision,
            this.envelope?.inventory,
            this.preferences.appNames,
            this.preferences.endpointNames,
        ]);
        this.state = project(
            this.envelope,
            this.preferences,
            this.flags,
            key === this.projectionKey ? this.state.devices : null,
        );
        this.projectionKey = key;
        for (const listener of this.listeners) listener();
    }
    accept(envelope) {
        if (
            !envelope ||
            envelope.version !== 1 ||
            typeof envelope.connection?.ready !== "boolean"
        )
            throw new Error("The audio service returned an invalid response.");
        if (
            envelope.snapshot &&
            (envelope.snapshot.version !== 1 ||
                typeof envelope.snapshot.revision?.epoch !== "string" ||
                typeof envelope.snapshot.revision?.generation !== "string" ||
                !/^(0|[1-9][0-9]*)$/.test(
                    envelope.snapshot.revision?.generation,
                ))
        )
            throw new Error("The routing revision could not be read.");
        const key = JSON.stringify([
            envelope.snapshot?.revision,
            envelope.inventory,
            this.preferences.appNames,
            this.preferences.endpointNames,
        ]);
        project(
            envelope,
            this.preferences,
            this.flags,
            key === this.projectionKey ? this.state.devices : null,
        );
        this.envelope = envelope;
        let metadataChanged = false;
        for (const [list, key, identity] of [
            [envelope.inventory?.applications, "appNames", "bundle"],
            [envelope.inventory?.devices, "endpointNames", "uid"],
        ]) {
            const entries = Object.entries(this.preferences[key]);
            const names = new Map(entries);
            for (const item of list || []) {
                if (
                    typeof item[identity] === "string" &&
                    typeof item.name === "string" &&
                    names.get(item[identity]) !== item.name
                ) {
                    names.set(item[identity], item.name);
                    metadataChanged = true;
                }
            }
            this.preferences[key] = Object.fromEntries([...names].slice(-4096));
        }
        if (metadataChanged) this.savePreferences(this.preferences);
        this.flags.meterExpired = false;
        clearTimeout(this.expiry);
        if (this.visible)
            this.expiry = setTimeout(() => {
                this.flags.meterExpired = true;
                this.publish();
            }, 250);
        this.publish();
    }
    async refresh(isCurrent = () => this.visible) {
        if (!this.visible) return false;
        if (this.reading) return this.reading;
        const generation = this.visibilityGeneration;
        const current = () =>
            this.visible &&
            generation === this.visibilityGeneration &&
            isCurrent();
        this.reading = (async () => {
            try {
                const envelope = await this.transport({
                    version: 1,
                    operation: "state",
                });
                if (!current()) return false;
                this.accept(envelope);
                return true;
            } catch (error) {
                if (!current()) return false;
                this.envelope = this.envelope
                    ? {
                          ...this.envelope,
                          connection: { connected: false, ready: false },
                      }
                    : null;
                this.flags.error = error.message;
                this.flags.meterExpired = true;
                this.publish();
                return true;
            }
        })();
        try {
            return await this.reading;
        } finally {
            this.reading = null;
        }
    }
    start(observe) {
        this.stopPolling?.();
        this.stopPolling = startVisiblePolling(
            (isCurrent) => this.refresh(isCurrent),
            {
                interval: 100,
                observe,
                changed: (visible) => {
                    ++this.visibilityGeneration;
                    this.visible = visible;
                    clearTimeout(this.expiry);
                    this.flags.meterExpired = true;
                    if (visible) this.publish();
                },
            },
        );
        return this.stopPolling;
    }
    send = async (action) => {
        if (action.type === "theme" && !isAppearance(action.value))
            return false;
        if (["select", "theme", "clear-error"].includes(action.type)) {
            if (action.type === "select") this.preferences.selected = action.id;
            if (action.type === "theme") this.preferences.theme = action.value;
            this.flags.error = "";
            this.savePreferences(this.preferences);
            this.publish();
            return true;
        }
        if (action.type === "acknowledge") {
            await this.refresh();
            if (!this.state.transportReady) return false;
            this.flags.uncertain = false;
            this.flags.error = "";
            this.publish();
            return true;
        }
        if (
            !(
                this.state.canEdit ||
                (action.type === "create" && this.state.canCreate)
            ) ||
            this.flags.saving
        ) {
            this.flags.error = this.flags.uncertain
                ? "Review the current routing before making another change."
                : "Wait for the audio service to reconnect before editing.";
            this.publish();
            return false;
        }
        this.flags.saving = true;
        this.flags.error = "";
        this.publish();
        let submitted = false;
        let definitiveReply = false;
        try {
            const history = ["undo", "redo"].includes(action.type);
            if (
                history &&
                (!this.state.history.supported ||
                    !this.state.history[
                        action.type === "undo" ? "canUndo" : "canRedo"
                    ])
            )
                throw new Error(
                    `There is no routing change to ${action.type}.`,
                );
            const message =
                action.type === "recover"
                    ? { version: 1, operation: "retry" }
                    : {
                          version: 1,
                          operation: this.state.snapshot?.document
                              ? "edit"
                              : "initialize",
                          request: {
                              version: 1,
                              expected: { ...this.state.snapshot.revision },
                              ...(history
                                  ? { history: action.type }
                                  : {
                                        commands: commandsFor(
                                            this.state,
                                            action,
                                        ),
                                    }),
                          },
                      };
            if (
                action.type === "move" &&
                action.kind === "device" &&
                this.preferences.selected !== this.state.selected
            ) {
                // Pin the displayed identity before a concurrent state refresh
                // can observe the reordered list ahead of its edit reply.
                this.preferences.selected = this.state.selected;
                this.savePreferences(this.preferences);
            }
            submitted = true;
            const result = await this.transport(message);
            definitiveReply = true;
            if (!result.ok) {
                this.flags.uncertain = ["uncertain", "failed"].includes(
                    result.code,
                );
                throw new Error(
                    this.flags.uncertain
                        ? "This change may already be saved. Review the latest routing before making another change."
                        : result.code === "conflict"
                          ? "Routing changed. Review the latest graph and try again."
                          : result.detail || "The change could not be saved.",
                );
            }
            if (action.type === "create") {
                this.preferences.selected = action.id;
                this.savePreferences(this.preferences);
            }
            await this.refresh();
            return true;
        } catch (error) {
            if (submitted && !definitiveReply) this.flags.uncertain = true;
            this.flags.error = this.flags.uncertain
                ? "This change may already be saved. Review the latest routing before making another change."
                : error.message;
            await this.refresh();
            return false;
        } finally {
            this.flags.saving = false;
            this.publish();
        }
    };
}

export function nativeTransport(message) {
    const bridge = globalThis.webkit?.messageHandlers?.loopz;
    if (!bridge)
        return Promise.reject(
            new Error(
                "Open LoopZ in its desktop application to connect to audio.",
            ),
        );
    return bridge.postMessage(message);
}
