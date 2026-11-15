# Decision: where the LoopZ Engine bundle is installed

Status: accepted for implementation (slices 1-2); slice 3 (migration) gated on a real-Mac consent test.

## Question
Keep `LoopZ Engine.app` in `/Applications`, or install it where users cannot casually move or delete it, without losing consent, saved mixes or defaults, and without restarting Core Audio?

## Inputs
- `audio_maintenance.mm` installs the Engine as root but hands the tree to the console user, so it is user-deletable without a password. A real Mac ended with the Engine missing, the app lacking its payload, and the Engine launchd job in `spawn scheduled` (respawn loop on a missing program) with no visible error.
- The routing broker pins the Engine by identifier + team OU + Developer ID marker (`pin()`), not path.
- macOS privacy consent for the Engine is stored by bundle id with a code requirement (TCC rows: `kTCCServiceMicrophone`, `kTCCServiceAudioCapture`, client `com.lexasoft.loopz.engine`, client_type 0, csreq present, no path). Evidence read on a developer Mac. Survival across a path change must still be proven by a real move test.
- Launch is path-dependent: the user job `~/Library/Application Support/LoopZ/Session/engine.plist` carries the absolute Engine path and existing job definitions must match exactly (`docs/editor-control.md`).
- Maintenance already does `launchctl bootout`, waits up to 8 s, re-bootstraps and compares routing status, with adjacent rollback bundles.

## Options
A `/Applications` (status quo). B `/Library/Application Support/LoopZ/LoopZ Engine.app`, root:wheel 0755. C per-user Application Support. D inside `LoopZ.app`.
C: user-deletable, one copy per account, root helper writing into home directories. D: every interface update replaces the running Engine's files, breaking "interface-only update leaves the Engine running"; deleting the app removes the Engine.

## Decision
Option B, bundle id, signing and launchd label unchanged. The Engine becomes a helper-installed component like the broker. Fall back to A plus hardening only if the consent test fails.

## Slices
1. No move: single Engine-path constant (native and `desktop_install.TARGETS`); make the Engine root-owned; accurate recovery states (see spec).
2. Dual-path read: probe, session host and lease, microphone setup accept the new path first, then the legacy path; a job definition with the legacy path is "known old", not "unknown, preserved".
3. Migration inside a user-initiated Update/Repair audio transaction, never silently: snapshot, stage copy, verify signature + requirement + `--runtime-capabilities`, no quarantine xattr, pause via existing bootout/wait (abort with nothing changed if it will not stop), atomic rename, publish new job plist (keep old as backup), bootstrap and verify routing status + broker + consent granted with no prompt, then rename old bundle to a rollback name and delete only after verification; resumable journal. Rollback restores old plist and path; if consent reads not-determined, roll back and do not prompt.

## Risks
Consent survival (system audio especially) unproven; two same-id bundles briefly coexist; developer unprivileged pair-update scripts assume `/Applications`; other accounts keep the legacy job path until their next launch; a root-owned Engine must not need to self-write.

The native Engine has no bundle-relative write path: routing and audio-settings files (and their backups) use the supplied user document path, diagnostics go to stdout/stderr, and it creates no bundle-local cache, bytecode, log or crash files (`engine/routing_main.mm`, `engine_runtime.cpp`, `configuration_controller.cpp`, `route_store.cpp`, `device_settings.mm`).
