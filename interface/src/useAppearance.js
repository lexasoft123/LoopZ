import { useLayoutEffect, useSyncExternalStore } from "react";
import { resolveAppearance } from "./appearance.js";

const system = globalThis.matchMedia?.("(prefers-color-scheme: dark)");
const isDark = () => system?.matches ?? false;
const subscribe = (changed) => {
    system?.addEventListener("change", changed);
    return () => system?.removeEventListener("change", changed);
};

export default function useAppearance(choice) {
    const dark = useSyncExternalStore(subscribe, isDark);
    const palette = resolveAppearance(choice, dark);
    useLayoutEffect(() => {
        document.documentElement.dataset.szPalette = palette;
        document.documentElement.style.colorScheme =
            palette === "atelier" ? "light" : "dark";
    }, [palette]);
    return palette;
}
