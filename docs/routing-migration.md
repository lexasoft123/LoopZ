# Existing-slot routing migration

The first fresh-slot routing activation failed on the tested Mac. The new root
broker and signed Engine connected, but the routing driver did not register or
attach within the activation window. The installer restored the original files,
kept Core Audio and default devices unchanged, and passed the independent output
continuity check. The authoritative result is copied to
`artifacts/routing-activation-failure.json`. A file copy and a successful broker
handshake do not establish HAL activation.

The separate migration entry point upgrades the already-discovered
`LoopZAudio.driver` slot. It uses the full routing implementation while preserving
the old bundle identifier, executable name and CFPlugIn factory identity. The
new bundle declares the routing broker service and contains the sealed broker
pin. Both driver variants run the same native lifecycle, multichannel IO,
configuration, timing and rollback fixture. The existing-slot migration now
passed on the tested Mac: Apple loaded the new image in a replacement isolated
host, published the validation device and withdrew it cleanly. Core Audio kept
its PID/start time, default devices stayed unchanged and the physical-output
continuity guard passed. Evidence is
`artifacts/routing-migration-installed-proof.json`. This establishes the tested
existing-slot path; fresh-slot discovery remains unresolved.

The old stereo endpoint is absent after migration because the new driver
publishes devices from the desired document. An empty catalog after withdrawing
the validation device is expected. The historical result field
`legacyInstallationPreserved` describes protected legacy broker/checker files;
the driver and Engine were intentionally replaced.

## Preparation and authorization

Build the macOS targets, then stage a distinct package:

```sh
python3 scripts/stage_signed.py --routing -- \
  --build build/editor-macos --legacy-slot --output <new-package-directory>
```

The result has kind `loopz-routing-migration`. The normal routing installer
rejects it. The migration verifier rejects ordinary routing packages and checks
the exact legacy slot identifier, executable and factory dictionary. The
package retains the artifact name `LoopZRoutingAudio.driver`; only this explicit
entry point maps it to `/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver`.

Run a read-only check as the console user:

```sh
/usr/bin/python3 -E -s -S scripts/install_routing_migration.py \
  --package <new-package-directory> --manifest-sha <reviewed-hash> \
  --replace-engine --check
```

The check reports that root host inspection remains required. The privileged
operation must be approved separately because it replaces the resident legacy
driver and force-stops its verified isolated host. It does not signal the main
Core Audio service, any host containing another HAL component, or another
application. There is no global-reset fallback. The original package and its
administrator command are not a migration command and should not be retried.

## Transaction and rollback

Before any promotion, require an idle, alive legacy endpoint, no default or
active aggregate depending on it, no running Engine, and an empty new routing
catalog. Refuse an existing separate routing-driver slot. Authenticate the
resident driver through the unchanged root-owned legacy bridge checker, then
verify its Apple signature, `_coreaudiod` UID, kernel process generation and
mapped driver device/inode. Refuse mappings from any other HAL bundle.

Keep adjacent rollback files until activation and output continuity pass. After
promotion, authenticate the same original peer again and verify that it still
maps the exact backup inode. Recheck idle state and Core Audio identity. Signal
only that process, with its kernel generation checked immediately before the
single-PID signal. PID checking and signaling are not atomic; the remaining race
is the same one documented by the existing isolated-host experiment.

Start a bounded, capture-free validation graph. Require an authenticated native
commit, exact public device channels/rate/input-latency metadata, a distinct
isolated host mapping the installed new binary, and clean device withdrawal.
Keep default devices, legacy broker/checker files and physical output continuity
unchanged. No saved user document, physical capture or monitor is opened.

On failure, capture helper/catalog diagnostics before cleanup. Withdraw the
validation graph and stop its Engine. Inspect any replacement host while the new
image is still on disk, restore all original files, and signal only an inspected
replacement if necessary. Require the original legacy endpoint, timing, mapped
image and authenticated bridge attachment to return. A disk-only rollback cannot
be reported as successful. If no replacement host can be established, never
signal a process by name or restart Core Audio; verify restoration or report
repair required. Preserve recoverable backups when repair is incomplete.

This migration is specific to an existing isolated LoopZ installation. Fresh
installation without a pre-discovered HAL slot remains an open acceptance gate,
as do active-graph updates and portability across macOS versions. The desktop
and login/background integration have their own acceptance gates.
