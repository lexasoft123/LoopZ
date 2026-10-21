import React from "react";
import { SIGNAL_PATH } from "../../../branding/mark.js";

export function LoopZMark({ size = 24 }) {
    return (
        <svg
            className="lz-brand-mark"
            width={size}
            height={size}
            viewBox="0 0 64 64"
            fill="none"
            aria-hidden="true"
            focusable="false"
        >
            <path
                d={SIGNAL_PATH}
                stroke="currentColor"
                strokeWidth="7.5"
                strokeLinecap="round"
                strokeLinejoin="round"
            />
        </svg>
    );
}
