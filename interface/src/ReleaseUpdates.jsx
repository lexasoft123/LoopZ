import React, { useEffect, useState } from "react";
import { Button } from "@singz/ui";
import { Switch } from "./components/Switch.jsx";
import { t } from "./i18n.js";

export function useReleaseUpdates(transport) {
    const [updates, setUpdates] = useState(null);
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState(false);
    useEffect(() => {
        let active = true;
        let timer;
        const poll = async () => {
            try {
                const next = await transport({
                    version: 1,
                    operation: "updates",
                });
                if (!next?.ok || typeof next.state !== "string") throw Error();
                if (active) {
                    setUpdates((previous) =>
                        JSON.stringify(previous) === JSON.stringify(next)
                            ? previous
                            : next,
                    );
                    setError(false);
                }
            } catch {
                if (active) setError(true);
            }
            if (active) timer = setTimeout(poll, 5000);
        };
        poll();
        return () => {
            active = false;
            clearTimeout(timer);
        };
    }, [transport]);
    const action = async (operation, extra = {}) => {
        if (busy) return;
        setBusy(true);
        try {
            const next = await transport({ version: 1, operation, ...extra });
            if (!next?.ok) throw Error();
            setUpdates((previous) =>
                JSON.stringify(previous) === JSON.stringify(next)
                    ? previous
                    : next,
            );
            setError(false);
        } catch {
            setError(true);
        } finally {
            setBusy(false);
        }
    };
    return { updates, busy, error, action };
}
export function ReleaseUpdates({ value }) {
    const { updates, busy, error, action } = value;
    const messages = {
        checking: "Checking for updates…",
        current: "You have the latest version.",
        available: "A newer version is available.",
        unpublished: "The first release is not published yet.",
        error: "Update check failed. Try again later.",
    };
    return (
        <>
            <div className="lz-setting-row">
                <div>
                    <strong>
                        {t("Updates")}
                        {updates?.currentVersion
                            ? ` · ${updates.currentVersion}`
                            : ""}
                    </strong>
                    <p role="status">
                        {error
                            ? t(
                                  "Update checks are unavailable in this environment.",
                              )
                            : t(
                                  messages[updates?.state] ||
                                      "App downloads are separate from driver installation.",
                              )}
                    </p>
                </div>
                <Button
                    size="sm"
                    disabled={busy || updates?.state === "checking"}
                    onClick={() =>
                        action(
                            updates?.state === "available"
                                ? "open-update"
                                : "check-updates",
                        )
                    }
                >
                    {t(
                        updates?.state === "available"
                            ? "Download update"
                            : "Check for updates",
                    )}
                </Button>
            </div>
            <div className="lz-setting-row">
                <strong>{t("Automatically check for updates")}</strong>
                <Switch
                    checked={updates?.automatic !== false}
                    disabled={busy || !updates}
                    label={t("Automatically check for updates")}
                    onChange={(enabled) =>
                        action("automatic-updates", { enabled })
                    }
                />
            </div>
        </>
    );
}
