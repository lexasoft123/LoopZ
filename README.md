# LoopZ

LoopZ is a macOS audio router built on SingZ's zcore, zdsp and design system.
This repository currently implements the approved feasibility phase, before
the complete routing interface or multiple virtual devices.

## Build and check

Requires macOS 14.2 or newer, a recent Xcode SDK with Core Audio taps, CMake,
Python 3 and a C++20 compiler. No sibling checkout or network download is used.

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

`native/core` and `native/include/loopz` contain platform-independent DSP,
capture history, timeline math and marker analysis. All Core Audio, Mach, XPC,
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

The native snapshot is pinned in `vendor/singz.lock.json`. `cmake/singz` builds
only device and graph components; media codecs, ML models and SingZ's product
ownership layers are excluded. Apple's driver sample license is retained in
`native/platform/macos/driver/APPLE-LICENSE.txt`. The `@singz/ui` dependency is reserved for
the interface phase; this feasibility build has no routing GUI.

See [the architecture contract](docs/architecture.md),
[the validation gates](docs/validation.md) and
[installation instructions](docs/install.md). Current observations and remaining
gates are recorded in [the feasibility results](docs/feasibility-results.md).
See [measured latency against Loopback](docs/latency-comparison.md) for the
experimental app-only settings, comparison results and measurement limits.

BlackHole was inspected as a timing and installer reference; see [the source review](docs/blackhole-review.md). Its GPL implementation is not included in LoopZ.
