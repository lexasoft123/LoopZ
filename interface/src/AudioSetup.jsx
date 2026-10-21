import React, { useState } from "react";
import { Button } from "@singz/ui";
import { installationComponents } from "./installation.js";
import { t } from "./i18n.js";

export function AudioSetup({ state, transport, refresh }) {
    const [busy, setBusy] = useState(false);
    const [detail, setDetail] = useState("");
    const [approvalRequired, setApprovalRequired] = useState(false);
    const [pendingActivation, setPendingActivation] = useState(false);
    const components = installationComponents(state);
    if (!components.length || state.installation?.checked !== true) return null;
    const fresh = components.every(
        (component) => component.status === "missing",
    );
    const update = state.installation?.updateAvailable === true;
    const needsActivation =
        pendingActivation || state.installation?.activationRequired === true;
    const action = needsActivation
        ? "activate"
        : fresh
          ? "install"
          : update
            ? "update"
            : "repair";
    const available = state.installation?.maintenanceAvailable !== false;
    const labels = {
        install: "Install audio components",
        update: "Update audio components",
        repair: "Repair audio",
        activate: "Finish audio setup",
    };
    const maintain = async (choice) => {
        if (busy) return;
        setBusy(true);
        setDetail("");
        try {
            const result = await transport({
                version: 1,
                operation: "audio-setup",
                action: choice,
            });
            setApprovalRequired(result?.code === "approval-required");
            if (!result?.ok)
                throw Error(
                    result?.detail || t("Audio maintenance could not finish."),
                );
            setPendingActivation(result.activationRequired === true);
            setDetail(
                t(
                    result.activationRequired
                        ? "Audio components installed. Choose Finish audio setup to activate the driver."
                        : "Audio components verified. Reconnecting your saved mixes…",
                ),
            );
            await transport({ version: 1, operation: "retry-session" });
            await refresh();
        } catch (error) {
            setDetail(t(error.message));
        } finally {
            setBusy(false);
        }
    };
    return (
        <div className="lz-setting-row">
            <div>
                <strong>
                    {t(fresh ? "Set up audio" : "Audio components")}
                </strong>
                <p>
                    {t(
                        needsActivation
                            ? "Activating the driver briefly interrupts audio in other apps. Your saved mixes and audio selections will be restored."
                            : fresh
                              ? "Install the driver and background service once. macOS will ask for administrator authorization."
                              : "Maintenance briefly stops LoopZ routing, then restores your saved mixes. Close apps using LoopZ audio before continuing. macOS will ask for administrator authorization.",
                    )}
                </p>
                {!available && (
                    <p>
                        {t(
                            "Move the complete LoopZ download to Applications to maintain audio components.",
                        )}
                    </p>
                )}
                {detail && <p role="status">{detail}</p>}
            </div>
            {approvalRequired && (
                <Button
                    onClick={() =>
                        transport({
                            version: 1,
                            operation: "audio-maintenance-settings",
                        })
                    }
                >
                    {t("Open audio approval settings")}
                </Button>
            )}
            <Button
                disabled={busy || !available}
                onClick={() => maintain(action)}
            >
                {t(busy ? "Maintaining audio components…" : labels[action])}
            </Button>
            {update && (
                <Button
                    disabled={busy || !available}
                    onClick={() => maintain("repair")}
                >
                    {t("Repair audio")}
                </Button>
            )}
        </div>
    );
}
