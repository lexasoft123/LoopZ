import React, { useSyncExternalStore } from "react";
import { LanguageSwitcher } from "@singz/ui";
import {
    getLanguage,
    getLocale,
    setLanguage,
    subscribeLocale,
    systemLocale,
    t,
} from "../i18n.js";

const options = [
    { value: "en", label: "English", code: "EN", flag: "🇬🇧" },
    { value: "ru", label: "Русский", code: "RU", flag: "🇷🇺" },
    { value: "zh-CN", label: "简体中文", code: "ZH", flag: "🇨🇳" },
];
export function Language({ compact = false }) {
    useSyncExternalStore(subscribeLocale, getLocale);
    const language = useSyncExternalStore(subscribeLocale, getLanguage);
    return (
        <LanguageSwitcher
            options={options}
            value={language}
            onChange={setLanguage}
            compact={compact}
            aria-label={t("Language")}
            system={{
                label: t("Follow system"),
                resolves: systemLocale(),
                badge: t("Automatic"),
            }}
        />
    );
}
