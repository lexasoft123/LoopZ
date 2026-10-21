import { t } from "../i18n.js";
import React from "react";
import { Button, Chip } from "@singz/ui";
import { Switch } from "./Switch.jsx";
import { GearIcon, SpeakerState } from "./ControlIcons.jsx";
import { sampleRateLabel } from "../sample-rates.js";
import { Gain } from "./Gain.jsx";
export function OutputControls({
    device: d,
    send,
    open,
    state,
    standalone = false,
}) {
    return (
        <fieldset
            disabled={!state.canEdit || state.saving}
            className={`lz-output-controls ${standalone ? "standalone" : ""}`}
            data-od-id="output-controls"
            aria-label="Device output controls"
        >
            <div className="lz-output-control-heading">
                <strong>Device output</strong>
                <span>{t("All pairs")}</span>
            </div>
            <div className="lz-output-gain-row">
                <Chip
                    active={d.muted}
                    aria-label="Mute device"
                    title={
                        d.muted ? "Unmute device output" : "Mute device output"
                    }
                    onClick={() =>
                        send({
                            type: "device",
                            patch: {
                                muted: !d.muted,
                            },
                        })
                    }
                >
                    <SpeakerState muted={d.muted} />
                </Chip>
                <Gain
                    value={d.gain}
                    label={t("Device gain")}
                    onChange={(gain) =>
                        send({
                            type: "device",
                            patch: {
                                gain,
                            },
                        })
                    }
                />
            </div>
            <div className="lz-output-setting">
                <label htmlFor="sample-rate">{t("Sample rate")}</label>
                <span className="lz-rate-control">
                    <select
                        id="sample-rate"
                        aria-label={t("Sample rate")}
                        className="lz-rate"
                        value={d.rate}
                        onChange={(e) =>
                            send({
                                type: "device",
                                patch: {
                                    rate: +e.target.value,
                                },
                            })
                        }
                    >
                        {!state.sampleRates.includes(d.rate) && (
                            <option value={d.rate} disabled>
                                {sampleRateLabel(d.rate)} (unavailable)
                            </option>
                        )}
                        {state.sampleRates.map((rate) => (
                            <option key={rate} value={rate}>
                                {sampleRateLabel(rate)}
                            </option>
                        ))}
                    </select>
                    <span className="lz-rate-chevron" aria-hidden="true" />
                </span>
            </div>
            <div className="lz-output-setting">
                <span>{t("Safety limiter")}</span>
                <Switch
                    checked={d.limiter}
                    label={t("Safety limiter")}
                    onChange={(limiter) =>
                        send({
                            type: "device",
                            patch: {
                                limiter,
                            },
                        })
                    }
                    data-od-id="limiter-switch"
                />
            </div>
            <div className="lz-output-setting lz-output-config">
                <span>{d.channels} channels</span>
                <Button
                    size="sm"
                    icon
                    aria-label={t("Configure output channels")}
                    title={t("Configure output channels")}
                    data-od-id="configure-channels"
                    onClick={(event) =>
                        open(
                            {
                                kind: "channels",
                            },
                            event.currentTarget,
                        )
                    }
                >
                    <GearIcon />
                </Button>
            </div>
        </fieldset>
    );
}
