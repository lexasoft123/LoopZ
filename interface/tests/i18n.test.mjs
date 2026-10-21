import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import {
    fromTag,
    resolveLocale,
    setLanguage,
    getLocale,
    t,
} from "../src/i18n.js";

test("SingZ locale rules respect system order and traditional Chinese", () => {
    assert.equal(resolveLocale("system", ["fr-FR", "ru-RU"]), "ru");
    assert.equal(resolveLocale("system", ["zh-TW", "en-US"]), "en");
    assert.equal(fromTag("zh-Hans-CN"), "zh-CN");
    assert.equal(fromTag("zh-Hant-CN"), null);
    assert.equal(resolveLocale("ru", ["en-US"]), "ru");
});
test("all offered languages have complete dictionaries and can switch live", () => {
    const copy = JSON.parse(
        readFileSync(new URL("../src/locales/copy.json", import.meta.url)),
    );
    for (const [key, entries] of Object.entries(copy)) {
        assert.equal(entries.length, 2, key);
        assert(
            entries.every((entry) => typeof entry === "string" && entry.trim()),
            key,
        );
    }
    setLanguage("ru");
    assert.equal(t("Settings"), "Настройки");
    setLanguage("zh-CN");
    assert.equal(t("Settings"), "设置");
    assert.equal(setLanguage("not-a-language"), false);
    assert.equal(getLocale(), "zh-CN");
    assert.equal(t("User-named guitar device"), "User-named guitar device");
    setLanguage("en");
    assert.equal(t("Settings"), "Settings");
});
