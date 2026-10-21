import { build } from "esbuild";
import { mkdir, writeFile, readFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { resolve } from "node:path";

const output = resolve(process.argv[2] || "dist");
await mkdir(output, { recursive: true });
const result = await build({
    entryPoints: ["src/main.jsx"],
    bundle: true,
    outfile: `${output}/loopz.js`,
    format: "iife",
    platform: "browser",
    target: ["safari17"],
    jsx: "automatic",
    minify: false,
    metafile: true,
    define: { "process.env.NODE_ENV": '"production"' },
    legalComments: "eof",
});
await writeFile(
    `${output}/index.html`,
    `<!doctype html>
<html lang="en" data-sz-palette="night-studio"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light dark">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self' data:; connect-src 'none'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'">
<title>LoopZ</title><link rel="stylesheet" href="loopz.css"></head>
<body><div id="root"></div><script src="loopz.js"></script></body></html>\n`,
);
const modules = [];
const contribution = new Map();
for (const output of Object.values(result.metafile.outputs)) {
    for (const [name, info] of Object.entries(output.inputs))
        contribution.set(
            name,
            (contribution.get(name) || 0) + info.bytesInOutput,
        );
}
for (const name of Object.keys(result.metafile.inputs).filter(
    (p) => p.includes("@singz/ui/") && contribution.get(p) > 0,
)) {
    modules.push({
        path: name,
        bytesInOutput: contribution.get(name),
        sha256: createHash("sha256")
            .update(await readFile(name))
            .digest("hex"),
    });
}
await writeFile(
    `${output}/build.json`,
    JSON.stringify(
        { package: "@singz/ui", version: "1.9.5", modules },
        null,
        2,
    ) + "\n",
);
