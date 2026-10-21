# LoopZ app identity

The mark is a continuous signal route folded into a Z. `mark.js` is the shared
geometry for the Dock forge and the header's inline vector component.

The Dock icon follows SingZ UI's `recipes/app-icon/README.md` from the local
`singz-ui` repository (package version 1.9.5). `glass.js` and `cut-icons.cjs` are
verbatim copies of that recipe. The dark superellipse, amber lighting passes,
and glass layer stay unchanged; only the application mark is customized.
The full drawing has two dark routing terminals. At 16–32 pixels those recesses
are removed and the path is bolder so the silhouette stays legible.

Regenerate on macOS:

```sh
node scripts/build_app_icons.mjs /path/to/Electron.app/Contents/MacOS/Electron
```

The executable can also be supplied with `LOOPZ_ICON_ELECTRON`. In this shared
workspace the script defaults to SingZ's existing Electron installation. Electron
is a design-time renderer only; LoopZ remains a native AppKit/WebKit application.
The initial WebKit render did not support the recipe's canvas blur correctly,
so the recipe's own Electron renderer produces the static deliverables.

Commit the generated `icon.icns`, `icon.ico`, `icon.png`, small PNGs and contact
sheet with the forge sources. Normal app builds only copy the committed ICNS;
they do not launch an icon renderer. The header is a static, theme-aware vector,
with no blur, canvas, animation or additional network request.
