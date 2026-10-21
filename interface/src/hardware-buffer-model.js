import { t } from "./i18n.js";
export function bufferLatencyLabel(frames, devices) {
    const rates = devices
        .map((device) => device.rate)
        .filter((rate) => Number.isFinite(rate) && rate > 0);
    const milliseconds = [
        ...new Set(rates.map((rate) => ((frames * 1000) / rate).toFixed(2))),
    ].sort((a, b) => Number(a) - Number(b));
    const duration = milliseconds.length
        ? `${milliseconds[0]}${milliseconds.length > 1 ? `–${milliseconds.at(-1)}` : ""} ms target/buffer`
        : "";
    const profile = {
        32: t("Low latency"),
        64: t("Balanced"),
        128: t("Lower CPU"),
    }[frames];
    return [`${frames} frames`, duration, profile].filter(Boolean).join(" · ");
}
