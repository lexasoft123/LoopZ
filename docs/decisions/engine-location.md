# Decision: where the LoopZ Engine bundle is installed

Status: slices 1–3 implemented; consent test pending on a real Mac. Application-capture graph migration remains disabled by default.

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
3. Migration inside a user-initiated Update/Repair audio transaction, never silently: snapshot, stage copy, verify signature + requirement + `--runtime-capabilities`, no quarantine xattr, pause via existing bootout/wait (abort with nothing changed if it will not stop), atomic rename, publish new job plist (keep old as backup), bootstrap and verify routing status + broker + consent granted with no prompt, then rename old bundle to a rollback name and delete only after verification; resumable journal. Rollback restores old plist and path; if a positively granted baseline becomes not-determined or denied, roll back and do not prompt.

## Risks
Consent survival (system audio especially) unproven; two same-id bundles briefly coexist; developer scripts must select the actual installed path; other accounts may retain legacy user jobs; a root-owned Engine must not need to self-write.

The native Engine has no bundle-relative write path: routing and audio-settings files (and their backups) use the supplied user document path, diagnostics go to stdout/stderr, and it creates no bundle-local cache, bytecode, log or crash files (`engine/routing_main.mm`, `engine_runtime.cpp`, `configuration_controller.cpp`, `route_store.cpp`, `device_settings.mm`).

## Slice 3 implementation and consent gate

Only an explicit Install/Update/Repair audio request to LoopZSetup moves the Engine.
The target is a real root:wheel directory under `/Library/Application Support/LoopZ`.
The helper verifies a private signed snapshot and staged Engine (publisher requirement,
read-only runtime capabilities and absence of quarantine), stops the exact user job,
and journals the Engine, other maintenance components and user-job plist together.
The prior plist and Engine remain recoverable until routing, broker and microphone
consent checks finish. The legacy bundle is then renamed into the rollback workspace;
commit removes it. A retry rolls an unfinished transaction back before starting again.
The saved routes, login preference and signing/launchd identities are unchanged.

`kMigrateApplicationCaptureGraphs` in `engine_location.hpp` defaults to false.
A saved application-capture graph with a real legacy bundle stays on that target
and reports a localized pending state, while maintenance still updates or repairs
the legacy Engine, driver and broker. It does not rewrite the job or retire the
legacy bundle. Incomplete real legacy bundle directories are repairable in place;
links are never admitted as bundles. The helper, setup probe and session host use
one shared predicate for this policy. The setup probe compares the selected legacy
Engine with the payload, so a completed in-place update clears the update action.
Fresh installs and other graphs use the new target. Native entry-point fixture
injection covers both gate values; there is no GUI or environment override. This is the explicit consent-related deviation from unconditional slice 3.
Microphone status is queried read-only from the Engine before and after publication;
only a positively authorized baseline establishes a grant that must survive.
A failed query, damaged Engine, denied or not-determined baseline means no baseline;
Repair remains available, including not-determined before and after maintenance.
Queries launched with asuser from the root daemon may have different TCC attribution,
so their absence of a grant does not establish consent loss. Loss of an authorized
baseline aborts and restores the old job and path without calling permission setup.
When the gate is enabled, application sources must also reconnect after bootstrap;
a capture failure rolls back with a distinct consent error when reported. An
inactive saved application-capture session cannot establish reconnection: migration
fails closed and retains the legacy Engine. There is no supported read-only system-audio consent
query in this implementation: microphone status does not prove system-audio consent.

Before enabling the gate, the coordinator will inspect the microphone and AudioCapture
TCC rows read-only on a real Mac before and after the move, verify that consent remains
granted without any prompt, and exercise application capture and rollback. No private
TCC API or database access is performed by the product or fixture tests. Installed,
hardware and real system-audio consent acceptance remain pending.

Retirement checks other local account jobs under `/Users/*/Library/Application Support/LoopZ/Session/engine.plist` before planning and immediately before retirement. A reference to the legacy executable, or unreadable/unknown account state, retains the legacy bundle; the current account may still move to the new Engine. Accounts with homes outside `/Users` are not exhaustively enumerated: automatic retirement is limited to the standard local-home layout and needs operator review on managed/custom-home Macs. No other account's job or routes are rewritten.

The staged capability probe runs as the console user from a verified, byte-identical root-owned copy under the private signed snapshot; it never executes as root and the transaction journal directory stays mode 0700. Recovery context serialization is checked and completion is cleared durably before rollback. A recreated retirement path is retained in a root-owned `.preserved-legacy-*` sibling so it cannot prevent restoring Engine, job and driver. When migration must wait for driver activation, rollback retains `activation-required.plist` and returns `activationRequired`, so Settings offers Finish audio setup instead of a permanent repair loop.

## Re-review: account selection and recovery snapshots

The current account's user-job program selects its Engine path. A new-path job
stays on the new path even while other accounts retain legacy, or if a legacy
bundle reappears later. Pending migration applies to a legacy-path job, or to a
missing job with a real legacy bundle; mere global bundle presence cannot redirect
a migrated account. The native host, setup probe and maintenance policy share
this rule. Developer script inventories and preparation commands use the same
account-aware selection (console account when invoked by root), then existing
real bundles when that account has no job. Unknown job paths fail closed.

Rollback is restoration of prior files and job, not a second test of the lost
grant: it clears consent and capture checks, bootstraps the restored job without
requiring capture reconnection, and independently attempts audio-default restoration
and resolved-context cleanup. A recovery context with no journal is stale and is
never reused; a new request snapshots current routes, runtime, catalog and consent,
so later user choices cannot be reverted by an old snapshot. Capture reconnection
and permission-string checks apply only while moving application capture from the
legacy path, not to ordinary Repair of an already migrated account.

Pending driver activation stops the replacement broker before rollback and starts
the restored broker afterward. A pending-activation transaction that keeps its
installed files verifies routing status before commit. A gate flip from enabled
to disabled during an interrupted move first recovers the recorded transaction,
then repairs in place under the new policy; native entry-point fixtures cover this.

Remaining optional edge limits: reads of other accounts' jobs can traverse user
directory symlinks (the final plist rejects symlinks), so a hostile/changing home
layout is not certified by this acceptance work. Cross-volume rename (EXDEV) is
not emulated with a non-atomic copy: it aborts and rolls back, retaining recovery
state if rollback cannot finish. If the user deletes the Session directory after
a crash, maintenance does not recreate it as root; restore its safe ownership and
directory structure before retrying the retained journal. These cases need separate
operator acceptance and do not justify silently dropping recovery state.

## Third review: launch admission and stopped jobs

Account job lookup is advisory and nonthrowing during desktop construction.
Unreadable, linked, multiply linked (including a link/unlink publication window),
empty, oversized or nondictionary plists become unknown metadata. The normal
host validator reports the problem through supervised status and can retry after
the file is valid; it does not terminate the launch path or replace the file.
The setup probe marks an uncertain job as unknown while keeping eligible
maintenance actions available. Private fixtures exercise the shared account
launch factory, not a separate parser-only approximation.

An existing canonical job file is restarted by explicit Repair even if no job is
currently loaded. This also repairs a stopped session after rollback cleaned its
file journal but bootstrap subsequently failed or the helper crashed. Current
routes/defaults/consent are still freshly sampled; a stale no-journal context is
not reused. Once bootstrapped, the session is tracked as loaded and must be
stopped before any later rollback. With no job at all, there is no prior capture
session to reconnect: gate-enabled maintenance can install/move the Engine
without inventing a capture-verification prerequisite or silently creating a job.

Developer target inventories hold a lazy path-like Engine target. Importing them
and derived target dicts never reads the console user's job; the first actual
path use resolves and freezes the account-aware path for that operation. Unknown
worktree jobs still fail guarded installer operations, while imports and isolated
fixtures remain usable without reading another user's private directory.

Hardware evidence remains open: pending-activation routing verification can time
out while the old driver image is still loaded. The activation-required marker
survives that failure, and the real Mac must verify the Finish audio setup flow
and saved routing restoration with that actual driver state. Fixture successes
do not certify this hardware transition and no live activation was performed.
