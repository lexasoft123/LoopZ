import React from "react";
// Local kit extension: @singz/ui 1.9.5 has no web Switch export.
// Uses the shipped control material and semantic colors without restyling kit primitives.
export function Switch({
    checked,
    onChange,
    label,
    disabled = false,
    ...props
}) {
    return (
        <button
            type="button"
            role="switch"
            aria-checked={checked}
            aria-label={label}
            disabled={disabled}
            title={`${label}: ${checked ? "On" : "Off"}`}
            className="lz-switch"
            onClick={() => onChange(!checked)}
            {...props}
        >
            <span className="lz-switch-track" aria-hidden="true">
                <span className="lz-switch-thumb" />
            </span>
        </button>
    );
}
