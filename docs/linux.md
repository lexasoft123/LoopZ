# Linux development preview

See the [acceptance record](linux-acceptance.md) for tested scope and limits.

Linux targets x86-64 and ARM64, including Orange Pi 5 Plus. This port is a
preview until the installed-package and hardware checks in
[the design](linux-engine-design.md) have passed. A portable-core build alone
is not application or audio qualification.

## Build the service or application

Use a separate checkout and build directory. Debian 13 dependencies include
`cmake`, a C++20 compiler, `python3`, `pkg-config`, `libasound2-dev`, `libudev-dev`
and `dpkg-dev` for Debian packages. The desktop additionally needs
`libwebkit2gtk-4.1-dev`, Node.js and the pinned interface dependencies. Engine-only
builds do not require GTK, WebKitGTK or Node.js. Initialize the pinned dependency:

```sh
git submodule update --init -- vendor/zcore
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DLOOPZ_BUILD_MACOS_BACKEND=OFF \
  -DLOOPZ_BUILD_ENGINE=ON
cmake --build build/linux --parallel 4
ctest --test-dir build/linux --output-on-failure --parallel 2
```

For the desktop, run `npm ci --ignore-scripts --prefix interface`, then
`npm run build --prefix interface`, and configure with
`-DLOOPZ_BUILD_LINUX_DESKTOP=ON` before building. Run `npm test --prefix interface`
and `npm run format:check --prefix interface`. Inspect the actual WebKitGTK
window on each target; browser tests are separate evidence. Follow native build
locks and compile limits on a shared build stand. Never share mutable build or
node_modules directories with another checkout.

## Freeze, build and package

The packaging script never builds or starts services. It stages `cmake --install`
under a fresh temporary DESTDIR and splits Engine/CLI/service from desktop/assets.
Configure the install prefix as `/usr`, including when testing tar artifacts.
Package on Linux for the target architecture; cross builds need a properly
configured target sysroot and dependency set.

Freeze source before the final build. Receipt/output paths must be ignored
artifacts outside tracked source so generating them does not alter the snapshot:

```sh
mkdir -p artifacts/linux-package
python3 scripts/package_linux.py \
  --write-source-receipt artifacts/linux-package/source.json
cmake --build build/linux --parallel 4
ctest --test-dir build/linux --output-on-failure --parallel 2
python3 scripts/package_linux.py --build build/linux \
  --source-receipt artifacts/linux-package/source.json \
  --output artifacts/linux-package/packages --format both
```

Use a fresh empty package output directory for each run; existing artifacts are
never overwritten. The receipt records HEAD, the full clean zCore revision, tracked/deleted/untracked
nonignored source bytes and executable bits, source snapshot digest, build
configuration and actual installed ELF architecture/hash. Packages include the
receipt and output includes SHA256SUMS. Packaging rejects source changes since
the pre-build receipt, a CMake build belonging to another source checkout, a
cached zCore root pointing elsewhere, and mixed/unsupported ELF architectures. This records
source stability and payload identity; it is not reproducible-build attestation.
Preserve build logs and the exact tested packages alongside the receipts.

`loopz-engine` contains no desktop runtime dependency. Debian packages derive
versioned runtime dependencies separately from each split's actual ELF files
using `dpkg-shlibdeps` from `dpkg-dev`, including the desktop's direct native
library dependencies. The desktop additionally requires the exact Engine package
version. Dependency scanning runs on the native target architecture against its
installed symbol/package metadata and fails explicitly if resolution is missing;
there is no hardcoded fallback. A temporary Debian control fixture under staging
supports scanning without a Debian source package in the repository. Receipts
record the resulting Depends field. Cross-built binaries must be packaged on
the matching native target environment; the script rejects an architecture
mismatch for Debian dependency scanning. Dependencies reflect that build target,
not a claim of compatibility with other distributions or older library versions.
Tar files contain a `/usr` tree and require compatible runtime libraries too;
tar-only packaging does not derive Debian dependency metadata. No maintainer
script auto-enables, restarts or migrates a
running audio service. Verify `dpkg-deb --info` and `dpkg-deb --contents` before
installing. This script does not publish a release.

## Installation and service lifetime

Coordinate an exclusive window before replacing any active audio owner. Do not
run this Engine beside a Box process that already owns the same ALSA devices.
Install the Engine package with `sudo apt install ./loopz-engine_*.deb`; install
the matching desktop package if needed. Installation places the user unit at
`/usr/lib/systemd/user/loopz-engine.service` but does not enable it. Explicitly
start it when ready:

```sh
systemctl --user daemon-reload
systemctl --user enable --now loopz-engine.service
systemctl --user status loopz-engine.service
```

The service runs as the current user. Device access and scheduling policy are
host configuration; do not run the Engine as root or weaken permissions merely
to hide a configuration failure. Inspect service logs with
`journalctl --user -u loopz-engine.service`. The editor is a control client;
closing it does not disable saved microphone monitoring. Explicit input-off or
route/device edits govern hardware lifetime. Consult CLI `--help` and the
versioned protocol document for the actual operation schema.

## Migration and rollback

Before updating, retain the currently working package, its SHA256SUMS and a
backup of the current user's LoopZ state directory identified by the service
configuration. Never overwrite unrelated SingZ/Box state or default devices.
Record active routes and device identities; do not assume card numbers or old
USB UIDs remain stable across boot/mode changes.

In an agreed maintenance window, stop only this LoopZ user service, install the
new package, reload the unit, start and verify canonical desired/active state
and audio counters. If activation fails, preserve logs and the new state backup,
stop this service, reinstall the retained matching Engine/desktop packages,
restore the prior state backup if the older schema cannot read it, reload and
start. Verify device/channel identity and routing before enabling audible paths.
Package removal deliberately leaves user routing state; remove it only on an
explicit request. Disabling the service uses
`systemctl --user disable --now loopz-engine.service`.

## Evidence limits

No existing SingZ image or historical Pi receipt qualifies this new Engine.
Report automated, installed, hardware and external-client checks separately.
Use loopback before a coordinated physical FLOW/HDMI test; never open competing
ALSA streams during the active Box session. No firmware, kernel, mixer or reboot
change is part of package installation. The current infrastructure choice is
FLOW analog MAIN to a separate amplifier and HDMI video; analog connection and
latency remain unverified. See the design for historical timing limitations.
