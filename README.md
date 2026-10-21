<p align="center"><img src="branding/icon-128.png" width="96" alt="LoopZ icon"></p>

# LoopZ

[![Release](https://img.shields.io/github/v/release/lexasoft123/LoopZ?include_prereleases&label=Download)](https://github.com/lexasoft123/LoopZ/releases)
[![Desktop checks](https://github.com/lexasoft123/LoopZ/actions/workflows/checks.yml/badge.svg)](https://github.com/lexasoft123/LoopZ/actions/workflows/checks.yml)
[![Portable core](https://github.com/lexasoft123/LoopZ/actions/workflows/core.yml/badge.svg)](https://github.com/lexasoft123/LoopZ/actions/workflows/core.yml)

Virtual audio routing for macOS. Combine microphones, hardware inputs and sound
from apps into a virtual device, then choose that device as the microphone in
your recording, streaming or calling app. Connect channels visually and listen
through any output device.

![LoopZ audio routing workspace](docs/screenshot.png)

Built with SingZ's **zcore**, **zdsp** and **singz-ui**. Audio runs locally in a
native background Engine; closing the window leaves your routing running.

## Download and use

Download a DMG from [GitHub Releases](https://github.com/lexasoft123/LoopZ/releases).
The first release targets **Apple Silicon, macOS 14.2 or newer**. Windows and
Linux can build the portable core; desktop audio backends for those platforms
are future work.

1. Drag **LoopZ** from the DMG into **Applications** and open it.
2. On a fresh installation, open **Settings → Install audio components** and
   authorize macOS once. The signed driver and background service are installed
   separately from the desktop app.
3. Allow microphone access to capture physical inputs, and macOS audio-recording
   access to capture apps, when requested.
4. Create a virtual device, add sources, connect their channels to outputs, and
   choose **LoopZ: <your device name>** in your recording app.
   Add a monitor to hear it locally.

Users do **not** need Python, Node.js, Xcode or a separate installer package.
Use **Settings → Audio components** to install, update or repair the driver and
background service. Maintenance restores saved mixes and preserves audio defaults.
If macOS asks, allow LoopZ’s helper in **Login Items & Extensions**.
When first-time activation is needed, **Finish audio setup** briefly interrupts
audio in other apps while preserving your audio selections and saved mixes.

## What it does

- **Virtual devices** — independent mixes with up to 64 channels, per-device
  on/off controls, gain, mute and a safety limiter.
- **App and hardware sources** — capture selected applications, microphones and
  multichannel interfaces; route individual channels into the mix.
- **Three-column routing** — sources → output channels → monitors. Drag between
  sockets, or use the Connections view and keyboard controls.
- **Monitoring** — send a mix to physical output channels while exposing it to
  other applications as a recording device.
- **Saved routing and recovery** — retain connections when devices or apps
  disconnect and retry the desired graph automatically.
- **Live meters** — separate source and output levels, with visual interpolation
  between Engine readings.
- **One hardware buffer setting** — applied immediately to all used hardware
  inputs and monitors, with a target duration shown beside each size. The setting
  persists across reconnects. Smaller buffers increase CPU demand and can affect
  other applications using the same hardware.
- **Languages and appearance** — English, Russian and Simplified Chinese controls,
  a system-language option, and the SingZ light/dark palettes. Some diagnostic
  messages currently remain in English.
- **Update checks** — automatic checks every six hours, with a manual check in
  Settings. Like SingZ on macOS, updates open the release download page;
  installation is explicit. Driver installation is a separate action.

## Preview limitations

The first release is a preview. Very small hardware buffers are not yet a
universal glitch-free setting: recent live checks found remaining missing source
frames. The latency shown in Settings describes **one hardware buffer**, not the
complete microphone-to-speaker delay. See [measured latency](docs/latency-comparison.md)
for the actual experiments and their limits.

Fresh-install acceptance and application compatibility checks are tracked in
[the roadmap](docs/product-roadmap.md). The repository's tests do not by themselves
prove a clean-Mac installation or every third-party recording app.

## Develop

Requires CMake 3.24+, a C++20 compiler, Python 3, Node.js, and Xcode 26 or newer on macOS.
The pinned SingZ core snapshot and UI archive are included; a sibling SingZ
checkout is not required.

```sh
npm ci --ignore-scripts --prefix interface
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure
npm test --prefix interface
```

Build the portable core on macOS, Windows or Linux:

```sh
cmake -S . -B build/core -DLOOPZ_BUILD_MACOS_BACKEND=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release --parallel 4
ctest --test-dir build/core -C Release --output-on-failure
```

Native code lives in `native/core` and `native/platform/macos`; the React interface
lives in `interface`. Build and release scripts use Python **on the development
machine and CI runner only**. Native formatting uses clang-format and Black;
frontend formatting uses the locked Prettier version.

## Releases

Like SingZ, releases are made from `v*` tags with versioned notes committed first.
CI builds and tests the app. Releases are currently signed locally through a
disposable keychain, submitted to Apple, stapled, and published with checksums.
Hosted signing is optional and disabled by default; see
[release setup](docs/releasing.md). Forks can run checks without Apple credentials.

[Architecture](docs/architecture.md) · [Installation and migration](docs/install.md) ·
[Editor contract](docs/editor-control.md) · [Validation](docs/validation.md) ·
[Report a bug](https://github.com/lexasoft123/LoopZ/issues)

## Credits and licensing

LoopZ uses pinned SingZ zcore/zdsp sources and the MIT-licensed singz-ui package.
Their provenance is recorded in `vendor/singz.lock.json` and
`interface/packages/provenance.json`. Apple's driver sample notice remains in
[native/platform/macos/driver/APPLE-LICENSE.txt](native/platform/macos/driver/APPLE-LICENSE.txt).
BlackHole was studied as a timing and installation reference; its GPL implementation
is not included. No additional license for LoopZ's own source is granted in this
preview; copyright remains with its author.
