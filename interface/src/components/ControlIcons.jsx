import React from "react";
import { Icon } from "@singz/ui/icons";
// The kit exports a sliders Settings icon, but no gear/wheel icon.
export function GearIcon() {
    return (
        <svg
            className="lz-control-icon"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            strokeWidth="1.7"
            strokeLinecap="round"
            strokeLinejoin="round"
            aria-hidden="true"
        >
            <path
                transform="translate(1 0)"
                d="m9 3-.6 2.3-2 .9-2.1-.6-2 3.5L4 10.8v2.4l-1.7 1.7 2 3.5 2.3-.6 1.9 1L9 21h4l.6-2.3 2-.9 2.1.6 2-3.5-1.7-1.7v-2.4l1.7-1.7-2-3.5-2.3.6-1.9-1L13 3Z"
            />
            <circle cx="12" cy="12" r="3.2" />
        </svg>
    );
}
export function SpeakerState({ muted }) {
    return (
        <span className="lz-mute-icon" aria-hidden="true">
            <Icon name="speaker" size={15} />
            {muted && (
                <svg
                    className="lz-mute-slash"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    strokeWidth="2"
                    strokeLinecap="round"
                >
                    <path d="m4 3 16 18" />
                </svg>
            )}
        </span>
    );
}
