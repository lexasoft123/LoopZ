// Locale resolution follows SingZ's shared/i18n/rules.ts. UI copy is LoopZ's.
import copy from "./locales/copy.json" with { type: "json" };
export const languages = ["system", "en", "ru", "zh-CN"];
export function fromTag(tag) {
    const parts = String(tag).toLowerCase().replaceAll("_", "-").split("-");
    if (parts[0] === "en" || parts[0] === "ru") return parts[0];
    if (parts[0] !== "zh" || parts.includes("hant")) return null;
    if (parts.includes("hans")) return "zh-CN";
    const region = parts.slice(1).find((part) => part.length === 2);
    return !region || ["cn", "sg"].includes(region) ? "zh-CN" : null;
}
export function resolveLocale(language, tags = []) {
    if (languages.includes(language) && language !== "system") return language;
    return tags.map(fromTag).find(Boolean) || "en";
}
let language = "system";
try {
    const saved = globalThis.localStorage?.getItem("loopz-language");
    if (languages.includes(saved)) language = saved;
} catch {}
const systemTags = () => globalThis.navigator?.languages || ["en"];
let locale = resolveLocale(language, systemTags());
const listeners = new Set();
export function getLanguage() {
    return language;
}
export function getLocale() {
    return locale;
}
export function systemLocale() {
    return resolveLocale("system", systemTags());
}
export function subscribeLocale(listener) {
    listeners.add(listener);
    return () => listeners.delete(listener);
}
function notify() {
    locale = resolveLocale(language, systemTags());
    if (globalThis.document) document.documentElement.lang = locale;
    for (const listener of listeners) listener();
}
export function setLanguage(value) {
    if (!languages.includes(value)) return false;
    language = value;
    try {
        globalThis.localStorage?.setItem("loopz-language", value);
    } catch {}
    notify();
    return true;
}
globalThis.addEventListener?.("languagechange", notify);
export function t(key, variables) {
    const text =
        locale === "en" ? key : copy[key]?.[locale === "ru" ? 0 : 1] || key;
    return variables
        ? text.replace(/\{(\w+)\}/g, (match, name) =>
              Object.hasOwn(variables, name) ? String(variables[name]) : match,
          )
        : text;
}
