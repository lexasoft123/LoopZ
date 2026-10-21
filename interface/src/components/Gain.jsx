import React, { useRef, useState } from "react";
import { gainText } from "../model.js";
const gainKeys = new Set([
    "ArrowLeft",
    "ArrowRight",
    "ArrowUp",
    "ArrowDown",
    "Home",
    "End",
    "PageUp",
    "PageDown",
]);
const gainPosition = (value) => Math.max(-60, Math.min(0, Math.round(value)));

export function Gain({ value, onChange, label }) {
    const input = useRef(null);
    const [draft, setDraft] = useState(value);
    const latest = useRef(value);
    const committed = useRef(gainPosition(value));
    const onCommit = useRef(onChange);
    const adjustingKey = useRef(false);
    React.useLayoutEffect(() => {
        onCommit.current = onChange;
    }, [onChange]);
    React.useLayoutEffect(() => {
        latest.current = value;
        committed.current = gainPosition(value);
        setDraft(value);
    }, [value]);
    const commit = React.useCallback(async (next) => {
        if (!Number.isFinite(next) || committed.current === next) return;
        committed.current = next;
        setDraft(next);
        const ok = await onCommit.current(next);
        if (ok === false) {
            setDraft(latest.current);
            committed.current = gainPosition(latest.current);
        }
    }, []);
    React.useLayoutEffect(() => {
        const control = input.current;
        // React maps range onChange to input events and may suppress the later
        // native change event. Listen to the committed event directly so an
        // accessibility action does not need a pointer/key release or blur.
        const changed = () => {
            if (!adjustingKey.current) void commit(+control.value);
        };
        control.addEventListener("change", changed);
        return () => control.removeEventListener("change", changed);
    }, [commit]);
    return (
        <div className="lz-gain">
            <input
                ref={input}
                className="slider"
                type="range"
                min="-60"
                max="0"
                step="1"
                value={draft}
                onChange={(event) => setDraft(+event.target.value)}
                onPointerUp={(event) => commit(+event.currentTarget.value)}
                onKeyDown={(event) => {
                    if (gainKeys.has(event.key)) adjustingKey.current = true;
                }}
                onKeyUp={(event) => {
                    if (gainKeys.has(event.key)) {
                        adjustingKey.current = false;
                        void commit(+event.currentTarget.value);
                    }
                }}
                onBlur={(event) => {
                    adjustingKey.current = false;
                    void commit(+event.currentTarget.value);
                }}
                aria-label={label}
                aria-valuetext={gainText(draft)}
            />
            <output>{gainText(draft)}</output>
        </div>
    );
}
