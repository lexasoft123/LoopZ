import React, { useState } from "react";
import { Button } from "@singz/ui";
import { installationComponents } from "./installation.js";
import { t } from "./i18n.js";
export function AudioSetup({ state, transport, refresh }) {
    const [busy, setBusy] = useState(false);
    const [detail, setDetail] = useState("");
    const components = installationComponents(state);
    if (
        !components.length ||
        !components.every((component) => component.status === "missing")
    )
        return null;
    const install = async () => {
        if (busy) return;
        setBusy(true);
        setDetail("");
        try {
            const result = await transport({
                version: 1,
                operation: "audio-setup",
            });
            if (!result?.ok)
                throw Error(
                    result?.detail || t("Audio setup could not finish."),
                );
            setDetail(t("Audio components installed. Reconnecting…"));
            await transport({ version: 1, operation: "retry-session" });
            await refresh();
        } catch (error) {
            setDetail(error.message);
        } finally {
            setBusy(false);
        }
    };
    return (
        <div className="lz-setting-row">
            <div>
                <strong>{t("Set up audio")}</strong>
                <p>
                    {t(
                        "Install the driver and background service once. macOS will ask for administrator authorization.",
                    )}
                </p>
                {detail && <p role="status">{detail}</p>}
            </div>
            <Button disabled={busy} onClick={install}>
                {t(
                    busy
                        ? "Installing audio components…"
                        : "Install audio components",
                )}
            </Button>
        </div>
    );
}
