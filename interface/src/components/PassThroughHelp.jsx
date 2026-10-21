import React from "react";
import { t } from "../i18n.js";

export const passThroughCopy = [
    "Pass-through receives sound from apps that select this LoopZ device as their speaker or audio output.",
    "Use it when you want to send an app’s output into your mix. You can also capture an app directly by adding it as an application source.",
    "Only using a microphone or instrument? You don’t need Pass-through and can remove it.",
];

export default function PassThroughHelp({ expanded = false }) {
    const explanation = (
        <>
            {passThroughCopy.map((line) => (
                <p key={line}>{t(line)}</p>
            ))}
            <p className="lz-pass-example">
                {t(
                    "Example: Music app → LoopZ device output → Pass-through → your mix → Zoom microphone.",
                )}
            </p>
        </>
    );
    return expanded ? (
        <aside className="lz-pass-help" aria-label={t("About Pass-through")}>
            {explanation}
        </aside>
    ) : (
        <details className="lz-pass-help">
            <summary>{t("What is Pass-through?")}</summary>
            {explanation}
        </details>
    );
}
