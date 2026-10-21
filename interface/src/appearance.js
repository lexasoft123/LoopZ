export const appearanceOptions = [
    { value: "system", label: "Match system" },
    { value: "atelier", label: "Atelier" },
    { value: "night-studio", label: "Night Studio" },
];

export const isAppearance = (value) =>
    appearanceOptions.some((option) => option.value === value);

export const normalizeAppearance = (value) =>
    isAppearance(value) ? value : "system";

export const resolveAppearance = (value, dark) => {
    const choice = normalizeAppearance(value);
    return choice === "system" ? (dark ? "night-studio" : "atelier") : choice;
};
