# LoopZ

LoopZ is a macOS audio router built on SingZ's zcore, zdsp and design system.
The full routing driver and Engine now run in the existing LoopZ installation.
Actual 2-, 8- and 64-channel IO, virtual chaining, gain/mute and a rate transition
have passed short checks. Shared capture, monitoring and graph recovery also
have isolated native and authenticated service tests.
The native desktop connects the SingZ interface to authenticated Engine edits,
real meters and permission-free inventory. Existing-slot activation passed
without restarting Core Audio. A signed background-operation candidate now
passes automatic Engine crash recovery and full four-device audio restoration.
Deployment/login, sustained low-latency and fresh-install acceptance remain in progress.
The active scope and remaining acceptance evidence are tracked in
[the full product roadmap](docs/product-roadmap.md).

## Build and check

Requires macOS 14.2 or newer, a recent Xcode SDK with Core Audio taps, CMake,
Python 3, Node.js and a C++20 compiler. The native SingZ snapshot and private UI
archive are included; no sibling checkout is required. Install the locked desktop
dependencies before configuring the macOS build (add `--offline` when cached):

```sh
cd interface
npm ci --ignore-scripts
cd ..
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
"build/LoopZ Engine.app/Contents/MacOS/LoopZ Engine" --list
```

The DSP core can be built independently, with a C++20 compiler and Python 3:

```sh
cmake -S . -B build/core -DLOOPZ_BUILD_MACOS_BACKEND=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release --parallel 4
ctest --test-dir build/core -C Release --output-on-failure
```

`native/core` and `native/include/loopz` contain platform-independent routing,
the multi-device document, capture history, timeline math and marker analysis.
The macOS `loopz-config` tool validates and atomically saves that document. It
does not activate audio itself: the installed document runtime watches saves
and publishes the desired devices. Starting that Engine is still explicit. See
[saved configuration commands](docs/install.md#saved-routing-configuration).
All Core Audio, Mach, XPC,
driver, helper and device-inventory code lives in `native/platform/macos`.
Its headers are available only to backend targets. The core does not enable
Objective-C++ or link Apple audio frameworks. Windows/Linux runtime backends
are future work; a three-OS CI workflow checks the core build when published.
Current local validation is on macOS, with the backend both enabled and disabled.

Source formatting uses clang-format 22.1.8 and Black 25.1.0. Install the optional
formatting tools once, then use the shared formatting command:

```sh
python3 -m venv build/format-tools
build/format-tools/bin/python -m pip install -r scripts/requirements-format.txt
python3 scripts/format_code.py          # format native code and Python scripts
python3 scripts/format_code.py --check  # check without editing
```

Install clang-format separately, or set `CLANG_FORMAT` to its executable. CMake
also exposes `format` and `format-check` targets. Formatting covers LoopZ's own
sources; the pinned vendor snapshot is excluded.

The native snapshot is pinned in `vendor/singz.lock.json` and remains unmodified.
An explicit build overlay in `cmake/singz/graph_containment.cmake` pins both input
and output hashes while reducing the runner's sample-containment overhead; the
generated runner remains covered by the SDK realtime source check.
`cmake/singz` builds
only device and graph components; media codecs, ML models and SingZ's product
ownership layers are excluded. Apple's driver sample license is retained in
`native/platform/macos/driver/APPLE-LICENSE.txt`. The desktop source in `interface/`
imports actual `@singz/ui` components and the complete kit stylesheet from pinned
version 1.9.5. Its graph and meters come from the Engine. The earlier design
prototype in `artifacts/interface-design` remains a separate demonstration.
See [the editor contract](docs/editor-control.md) for signed staging, first-run
creation and remaining installed UI acceptance. Frontend formatting uses the
locked Prettier version through `npm run format:check` in `interface/`.

See [the architecture contract](docs/architecture.md),
[the validation gates](docs/validation.md) and
[installation instructions](docs/install.md). Current observations and remaining
gates are recorded in [the feasibility results](docs/feasibility-results.md).
See [measured latency against Loopback](docs/latency-comparison.md) for the
experimental app-only settings, comparison results and measurement limits.

BlackHole was inspected as a timing and installer reference; see [the source review](docs/blackhole-review.md). Its GPL implementation is not included in LoopZ.

The development engine also accepts `--routing-document <path>` for the new
multi-device runtime. It watches the saved document, independently schedules
audio and reconciles capture/monitor ownership through the authenticated
protocol-2 service. Missing configuration or trusted service metadata keeps it
waiting without opening capture or implicitly deleting devices. SIGUSR1 retries;
SIGINT/SIGTERM stop cleanly. This mode and its service/preview driver are not yet
the installed default. See [the full acceptance roadmap](docs/product-roadmap.md)
and [runtime architecture](docs/architecture.md#persistent-document-runtime).

The preview driver accepts client requests for 44.1/48 kHz through the same
authenticated configuration path. Document version 2 stores a durable request
receipt so a lost acknowledgement or helper restart cannot replay an old choice
over a newer edit; version 1 files still load. See
[client sample-rate requests](docs/architecture.md#client-sample-rate-requests)
for activation and validation limits.
