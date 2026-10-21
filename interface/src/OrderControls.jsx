import { t } from "./i18n.js";
import React, { useRef } from "react";
import { Button } from "@singz/ui";
export default function OrderControls({
    state,
    device,
    node = device,
    kind,
    writable,
    send,
}) {
    const group = useRef(null),
        up = useRef(null),
        down = useRef(null);
    const items =
        kind === "device"
            ? state.devices
            : kind === "source"
              ? device.sources
              : device.monitors;
    const index = items.findIndex((item) => item.id === node.id);
    const label = {
        device: t("Device"),
        source: t("Source"),
        monitor: t("Monitor"),
    }[kind];
    const move = async (offset, origin) => {
        const focused = document.activeElement === origin;
        await send({
            type: "move",
            kind,
            device: device.id,
            id: node.id,
            offset,
            expectedOrder: items.map((item) => item.id),
        });
        if (focused)
            requestAnimationFrame(() => {
                if (
                    !group.current ||
                    (document.activeElement !== document.body &&
                        document.activeElement !== origin)
                )
                    return;
                const preferred = offset < 0 ? up.current : down.current;
                const alternate = offset < 0 ? down.current : up.current;
                const target =
                    preferred && !preferred.disabled
                        ? preferred
                        : alternate && !alternate.disabled
                          ? alternate
                          : group.current;
                target.focus({
                    preventScroll: true,
                });
            });
    };
    return (
        <div
            className="lz-order-controls"
            ref={group}
            tabIndex={-1}
            role="group"
            aria-label={`${label} order`}
        >
            <div>
                <strong>{label} order</strong>
                <p role="status" aria-live="polite">
                    {t("Position") + " "}
                    {index + 1} of {items.length}
                </p>
            </div>
            <div className="lz-order-actions">
                <Button
                    ref={up}
                    size="sm"
                    disabled={!writable || index <= 0}
                    aria-label={`Move ${node.name} up`}
                    onClick={(event) => move(-1, event.currentTarget)}
                >
                    {t("Move up")}
                </Button>
                <Button
                    ref={down}
                    size="sm"
                    disabled={
                        !writable || index < 0 || index + 1 >= items.length
                    }
                    aria-label={`Move ${node.name} down`}
                    onClick={(event) => move(1, event.currentTarget)}
                >
                    {t("Move down")}
                </Button>
            </div>
        </div>
    );
}
