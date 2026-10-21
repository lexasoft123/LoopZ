import { t } from "./i18n.js";
const components = [
    ["engine", "Audio engine"],
    ["driver", "Audio driver"],
    ["service", "Audio service"],
];
const states = new Set(["installed", "missing", "invalid", "unknown"]);
export function installationComponents(state) {
    const setup = state.installation;
    if (!setup) return [];
    return components.map(([id, name]) => {
        const component = setup.components?.[id];
        const status =
            setup.checked !== true
                ? "unchecked"
                : states.has(component?.state)
                  ? component.state
                  : "unknown";
        const labels = {
            unchecked: t("Checking"),
            installed: t("Installed"),
            missing: t("Not installed"),
            invalid: t("Needs repair"),
            unknown: t("Could not check"),
        };
        return {
            id,
            name,
            status,
            label: labels[status],
            detail:
                status === "installed" || status === "unchecked"
                    ? ""
                    : typeof component?.detail === "string" && component.detail
                      ? component.detail
                      : "The setup check is incomplete. Retry the setup check.",
        };
    });
}
export function installationCondition(state) {
    const components = installationComponents(state);
    if (!components.length || components.some((c) => c.status === "unchecked"))
        return null;
    for (const status of ["invalid", "missing", "unknown"]) {
        const issue = components.find((c) => c.status === status);
        if (issue)
            return {
                kind: status,
                heading:
                    status === "missing"
                        ? "Finish LoopZ setup"
                        : status === "invalid"
                          ? "LoopZ setup needs attention"
                          : "Could not check LoopZ setup",
                detail: issue.detail,
            };
    }
    if (
        state.connected &&
        state.snapshot?.restoration?.phase === "waiting-for-driver"
    ) {
        return {
            kind: "driver-connection",
            heading: "Audio driver is not connected",
            detail: "The audio components are installed. Retry the audio connection in Settings. Your saved graph stays intact.",
        };
    }
    return null;
}
