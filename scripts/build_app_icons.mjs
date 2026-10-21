// SingZ UI's app-icon recipe, kept verbatim in branding/{glass.js,cut-icons.cjs}.
import { execFileSync } from "node:child_process";
import {
    existsSync,
    copyFileSync,
    readFileSync,
    writeFileSync,
    mkdtempSync,
    rmSync,
} from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const electron =
    process.argv[2] ||
    process.env.LOOPZ_ICON_ELECTRON ||
    resolve(
        root,
        "../SingZ/node_modules/electron/dist/Electron.app/Contents/MacOS/Electron",
    );
if (!existsSync(electron))
    throw new Error(
        "Pass the Electron executable path as the first argument, or set LOOPZ_ICON_ELECTRON.",
    );
if (process.platform !== "darwin")
    throw new Error("ICNS packaging requires macOS iconutil.");
const branding = root + "/branding";
execFileSync(
    electron,
    [branding + "/cut-icons.cjs", branding + "/forge.html", branding],
    { stdio: "inherit", timeout: 45000 },
);
const work = mkdtempSync(root + "/artifacts/icon-extract-");
try {
    const set = work + "/LoopZ.iconset";
    execFileSync("/usr/bin/iconutil", [
        "-c",
        "iconset",
        branding + "/icon.icns",
        "-o",
        set,
    ]);
    for (const size of [16, 32, 64, 128, 256]) {
        const name = size === 64 ? "icon_32x32@2x" : `icon_${size}x${size}`;
        copyFileSync(`${set}/${name}.png`, `${branding}/icon-${size}.png`);
    }
    // Review the exact shipped representations, including the small variant.
    let x = 36;
    const drawings = [256, 128, 64, 32, 16].map((size) => {
        const source = readFileSync(`${branding}/icon-${size}.png`).toString(
            "base64",
        );
        const markup = `<image href="data:image/png;base64,${source}" x="${x}" y="${320 - size}" width="${size}" height="${size}"/><text x="${x + size / 2}" y="350" text-anchor="middle" fill="#aaa394" font-family="system-ui" font-size="12">${size} px</text>`;
        x += size + 28;
        return markup;
    });
    writeFileSync(
        branding + "/contact-sheet.svg",
        `<svg xmlns="http://www.w3.org/2000/svg" width="720" height="390" viewBox="0 0 720 390"><rect width="720" height="390" rx="20" fill="#100e0b"/><text x="36" y="38" fill="#f1eadf" font-family="system-ui" font-size="16">LoopZ · SingZ UI liquid glass</text>${drawings.join("")}</svg>`,
    );
} finally {
    rmSync(work, { recursive: true });
}
