import React, { useEffect, useRef, useState } from "react";
import { Button } from "@singz/ui";
import { installationComponents } from "./installation.js";
import { t } from "./i18n.js";

export function AudioSetup({ state, transport, refresh }) {
    const [busy, setBusy] = useState(false);
    const [detail, setDetail] = useState("");
    const [approvalRequired, setApprovalRequired] = useState(false);
    const [pendingActivation, setPendingActivation] = useState(false);
    const waiter = useRef(0);
    const mounted = useRef(true);
    useEffect(() => {
        mounted.current = true;
        return () => {
            mounted.current = false;
            waiter.current += 1;
        };
    }, []);
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
    // After macOS asks for Login Items approval, watch the helper registration
    // and continue the same action once it is enabled (SMAppService status 1).
    const continueAfterApproval = async (choice) => {
        const ticket = ++waiter.current;
        for (let i = 0; i < 150; i++) {
            await new Promise((resolve) => setTimeout(resolve, 2000));
            if (!mounted.current || waiter.current !== ticket) return;
            let status;
            try {
                status = await transport({
                    version: 1,
                    operation: "audio-setup",
                    action: "status",
                });
            } catch {
                continue;
            }
            if (status?.registrationStatus === 1) {
                setApprovalRequired(false);
                await maintain(choice);
                return;
            }
        }
    };
    const maintain = async (choice) => {
        if (busy) return;
        waiter.current += 1;
        setBusy(true);
        setDetail("");
        try {
            const result = await transport({
                version: 1,
                operation: "audio-setup",
                action: choice,
            });
            const approval = result?.code === "approval-required";
            setApprovalRequired(approval);
            if (approval && mounted.current) void continueAfterApproval(choice);
            if (!result?.ok)
                throw Error(
                    result?.detail || t("Audio maintenance could not finish."),
                );
            setPendingActivation(result.activationRequired === true);
            setDetail(
                t(
                    result.migrationPending
                        ? "Engine migration is pending consent verification. Your saved mixes keep using the current Engine."
                        : result.activationRequired
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
    // Keep reason codes/messages in sync with installation_setup.mm.
    const unavailableMessages = {
        "wrong-location":
            "Move LoopZ to Applications to maintain audio components.",
        "missing-helper":
            "This copy of LoopZ is missing its setup helper. Download the full LoopZ release and replace this copy.",
        "missing-payload":
            "This copy of LoopZ is incomplete. Download the full LoopZ release and replace this copy.",
        "separate-driver":
            "This separate routing driver layout cannot be maintained in-app. Contact support before changing audio components.",
    };
    const waiting = state.snapshot?.restoration?.phase === "waiting-for-driver";
    return (
        <div className="lz-setting-row lz-setting-row-stacked">
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
                            unavailableMessages[
                                state.installation?.maintenanceUnavailableReason
                            ] ||
                                "This copy of LoopZ is incomplete. Download the full LoopZ release and replace this copy.",
                        )}
                    </p>
                )}
                {state.installation?.migrationPending && !detail && (
                    <p role="status">
                        {t(
                            "Engine migration is pending consent verification. Your saved mixes keep using the current Engine.",
                        )}
                    </p>
                )}
                {detail && <p role="status">{detail}</p>}
            </div>
            <div className="lz-setting-actions">
                {waiting && (
                    <Button
                        disabled={busy}
                        onClick={async () => {
                            await transport({
                                version: 1,
                                operation: "retry-session",
                            });
                            await refresh();
                        }}
                    >
                        {t("Retry audio connection")}
                    </Button>
                )}
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
        </div>
    );
}
