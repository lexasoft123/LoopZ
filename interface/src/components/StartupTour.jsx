import React, { useLayoutEffect, useRef, useState } from "react";
import { Button } from "@singz/ui";
import { t } from "../i18n.js";
import { tourPlacement, tourSteps } from "../tour.js";

export default function StartupTour({
    hasDevice,
    hasPassThrough,
    deviceName,
    onFinish,
}) {
    const dialog = useRef(null);
    const card = useRef(null);
    const [index, setIndex] = useState(0);
    const [layout, setLayout] = useState({ rect: null, placement: {} });
    // Freeze the route through the tour. Background audio snapshots must not
    // reset steps or cause extra layout measurements while meters update.
    const [steps] = useState(() => tourSteps(hasDevice, hasPassThrough));
    const step = steps[index];
    const body = t(step.body, { name: deviceName });
    const title = t(step.title);
    const maskID = "loopz-tour-spotlight";

    useLayoutEffect(() => {
        const previous = document.activeElement;
        const element = dialog.current;
        element.showModal();
        return () => {
            element.close();
            const target =
                previous?.isConnected && previous !== document.body
                    ? previous
                    : document.querySelector('[data-tour="settings"]');
            target?.focus();
        };
    }, []);

    useLayoutEffect(() => {
        document
            .getElementById("lz-tour-title")
            ?.focus({ preventScroll: true });
        const target = step.target
            ? [...document.querySelectorAll(step.target)].find((element) => {
                  const bounds = element.getBoundingClientRect();
                  return bounds.width > 0 && bounds.height > 0;
              })
            : null;
        target?.scrollIntoView({
            block: "nearest",
            inline: "nearest",
            behavior: "auto",
        });
        // Batch layout events without polling. Animation frames may be suspended
        // while a native WebKit window is not the key window.
        let measurement;
        const measure = () => {
            clearTimeout(measurement);
            measurement = setTimeout(() => {
                const viewport = {
                    width: window.innerWidth,
                    height: window.innerHeight,
                };
                const bounds = target?.getBoundingClientRect();
                const rect =
                    bounds &&
                    bounds.width > 0 &&
                    bounds.height > 0 &&
                    bounds.right > 0 &&
                    bounds.bottom > 0 &&
                    bounds.left < viewport.width &&
                    bounds.top < viewport.height
                        ? {
                              left: Math.max(4, bounds.left - 6),
                              top: Math.max(4, bounds.top - 6),
                              right: Math.min(
                                  viewport.width - 4,
                                  bounds.right + 6,
                              ),
                              bottom: Math.min(
                                  viewport.height - 4,
                                  bounds.bottom + 6,
                              ),
                          }
                        : null;
                const placement = tourPlacement(rect, viewport, {
                    height: card.current?.offsetHeight || 280,
                });
                setLayout({ rect, placement });
            });
        };
        const observer = new ResizeObserver(measure);
        if (target) observer.observe(target);
        observer.observe(card.current);
        window.addEventListener("resize", measure);
        window.addEventListener("scroll", measure, true);
        measure();
        return () => {
            clearTimeout(measurement);
            observer.disconnect();
            window.removeEventListener("resize", measure);
            window.removeEventListener("scroll", measure, true);
        };
    }, [step, title, body]);

    const rect = layout.rect;
    return (
        <dialog
            ref={dialog}
            className="lz-tour"
            role="dialog"
            aria-labelledby="lz-tour-title"
            aria-describedby="lz-tour-body"
            onKeyDown={(event) => {
                if (event.key !== "Tab") return;
                const buttons = [
                    ...dialog.current.querySelectorAll("button:not(:disabled)"),
                ];
                const current = document.activeElement;
                const first = buttons[0],
                    last = buttons.at(-1);
                if (
                    event.shiftKey &&
                    (current === first || !buttons.includes(current))
                ) {
                    event.preventDefault();
                    last?.focus();
                } else if (!event.shiftKey && current === last) {
                    event.preventDefault();
                    first?.focus();
                }
            }}
            onCancel={(event) => {
                event.preventDefault();
                onFinish();
            }}
        >
            <svg
                className="lz-tour-shade"
                width="100%"
                height="100%"
                aria-hidden="true"
            >
                <defs>
                    <mask id={maskID}>
                        <rect width="100%" height="100%" fill="white" />
                        {rect && (
                            <rect
                                x={rect.left}
                                y={rect.top}
                                width={rect.right - rect.left}
                                height={rect.bottom - rect.top}
                                rx="10"
                                fill="black"
                            />
                        )}
                    </mask>
                </defs>
                <rect
                    width="100%"
                    height="100%"
                    fill="rgba(0,0,0,.65)"
                    mask={`url(#${maskID})`}
                />
                {rect && (
                    <rect
                        x={rect.left}
                        y={rect.top}
                        width={rect.right - rect.left}
                        height={rect.bottom - rect.top}
                        rx="10"
                        fill="none"
                        stroke="var(--sz-accent)"
                        strokeWidth="2"
                    />
                )}
            </svg>
            <section
                ref={card}
                className="lz-tour-card"
                style={layout.placement}
            >
                <div className="lz-tour-progress">
                    {t("Quick tour")} · {index + 1} / {steps.length}
                </div>
                <h2 id="lz-tour-title" tabIndex={-1}>
                    {title}
                </h2>
                <p id="lz-tour-body">{body}</p>
                <div className="lz-tour-actions">
                    <Button size="sm" onClick={onFinish}>
                        {t("Skip tour")}
                    </Button>
                    <div>
                        {index > 0 && (
                            <Button
                                size="sm"
                                onClick={() => setIndex(index - 1)}
                            >
                                {t("Back")}
                            </Button>
                        )}
                        <Button
                            size="sm"
                            variant="primary"
                            autoFocus
                            onClick={() =>
                                index === steps.length - 1
                                    ? onFinish()
                                    : setIndex(index + 1)
                            }
                        >
                            {t(index === steps.length - 1 ? "Got it" : "Next")}
                        </Button>
                    </div>
                </div>
            </section>
        </dialog>
    );
}
