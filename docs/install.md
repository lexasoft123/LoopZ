# Install and update LoopZ

## Latest account-owned app update

The installed app pair includes the capacity-transition and silent-meter optimizations, earlier
preview/original-playback fixes and backup-recovery guidance. It was updated as
the console account without administrator authorization. The driver, broker,
login choice, Core Audio process, audio defaults, microphone grant and original
saved files are retained. Exact installed hashes and live signal acceptance are
recorded in `artifacts/silent-meter-delivery-proof.json` and
`artifacts/silent-meter-repeat-delivery-proof.json`: two continuous 30-second,
64-channel recordings had no missing samples while the graph grew to 32 devices
of 64 channels and shrank again. The runs add 3 and 0 publication deadline misses,
compared with the parent's 1,065, with no new misses during either full-graph hold.
Sustained worst-case performance margin is still open.

The Apple notarization below covers a separate, older release archive. It does
not establish notarization of this newer installed pair.

A later producer-retention experiment did not pass actual edited-input visibility
continuity and was rolled back. The installed pair is again manifest
`ca8b00a79a2fa0b5679a692a92a6749cc7d25dc038e6bc21a248394cebccf6ba`, with exact
pre-experiment app bytes, original files, empty fixture history, canonical session,
permissions, driver/broker, audio defaults and 512-frame hardware buffers verified.
The experimental renderer edits are absent from working code. Evidence:
`artifacts/visibility-rollback-proof.json`.

## Saved routing recovery in the installed app

LoopZ automatically loads the last valid backup when its primary routing file
cannot be read. The desktop warns that this recovered routing may be older and
asks you to review devices and connections before making changes. Loading a
backup does not overwrite the damaged primary. Repairing the primary clears
the warning automatically. An unsupported newer document is preserved and is
not silently replaced by an older backup.

If no readable document is available while routing is already running, LoopZ
keeps its last valid graph and rejects edits until a valid file becomes
available. Error guidance does not claim an active graph exists on a cold
start. Actual installed virtual-signal recovery and the signed interface-only
update are recorded in `artifacts/document-recovery-delivery-proof.json`.
The update needs no administrator access and leaves the Engine/driver running.
Installed cold-start checks now also cover two unreadable files, a newer primary
with a valid older backup, and a damaged primary with a newer backup. Each keeps
the saved bytes intact and refuses both edit and initialization requests. A valid
primary restores normal operation in the same process. An incompatible backup
still prevents saving over that backup after primary repair.

With both files absent, no device is silently created. An explicit authenticated
initialization creates the first source-free two-channel input device. Its saved
definition and public UID survive an actual Engine restart; a second initialization
is rejected. The fixture then deletes its device and restores the original files
and canonical session. Evidence: `artifacts/cold-start-delivery-proof.json`.
These checks use the current installed app pair; no app or protected component
is replaced. Existing private WebKit error-guidance checks still match the shipped
interface. This is user-document startup acceptance, not fresh system installation,
login/reboot or a substitute for offline recovery copies.

## Installed setup diagnostics, 20 October

The current signed app pair includes read-only native setup inspection.
`LoopZ --installation-status` reports Engine, routing driver and service checks
using fixed production paths, publisher requirements and protected-file rules.
The desktop displays missing, invalid or unknown components separately from
driver connection status and refreshes the checks through Retry. Signature
checks run outside meter polling; the interface cannot select executable paths
or installer commands. Legacy-only driver files do not count as an installed
routing slot or falsely conflict with a new routing slot.

This update is installed without sudo; driver and broker bytes remain unchanged.
Evidence: `artifacts/setup-publication-delivery-proof.json`. Initial native
installation and fresh-slot activation remain open. The notarization archive
described below predates this update; only those exact approved bytes were
submitted, without substituting newer app bytes.

## Distribution signing and accepted notarization

Local development signing and distribution signing are now explicit paths.
`stage_desktop.py --distribution --build build/editor-release` requires an
optimized Release build, signs the login helper and both apps with secure
Apple timestamps, and does not reuse the installed development signatures.
`stage_driver_update.py --distribution --desktop-package <package>` requires
timestamped signatures on that desktop pair before timestamping the driver.
Ordinary local staging keeps its existing signature-reuse behavior for
interface-only updates.

`distribution_signing.py` verifies every Mach-O architecture against the fixed
publisher requirement, Hardened Runtime and exact component entitlements.
A local `Signed Time` is not accepted as an Apple timestamp. Debug entitlements,
a weak second slice, wrong identities and missing timestamps fail admission.
Passing this check does not imply notarization or Gatekeeper acceptance.

`notarize_release.py --package <package> --manifest-sha <digest>` prepares a
private immutable archive of the verified existing-slot desktop/driver update.
It strips filesystem metadata, rejects extra paths, symlinks, duplicate files
and changed bytes, and compares every archived file to the reviewed payload.
The private receipt binds its complete SHA-256 and file count. The current
candidate contains 17 files in a 962,993-byte archive; exact identity and
68 passing local checks are recorded in
`artifacts/distribution-candidate-proof.json`.

Submission is separate and requires authorization to send the specific archive
to Apple's notary service. The local credential wrapper from SingZ can supply
API authentication without printing credential values. With authorization,
`notarize_release.py --receipt <receipt> --archive-sha <reviewed-archive-sha> --submit`
revalidates the archive and records an attempt before
upload and persists Apple's submission ID. Later `--receipt <receipt> --check`
calls query only that same ID. A timeout never starts another submission; an
ambiguous upload without an ID requires reconciliation rather than a retry.
This tool neither installs files nor staples or changes the reviewed package.

An initial upload attempt was blocked by automatic approval review for missing
external-transfer authorization. The user subsequently explicitly approved
this exact archive, SHA-256
`314d00ac0aae3bac0a4a7b3d36f7206cc5be9dcfacade81d84829c944328fcad`.
Apple accepted submission `0127c7fc-23f3-4c43-81c0-e6e40ffd1528`; its log matches
that archive hash, reports no issues, and contains tickets for both apps, the
login helper and the driver. Receipt, Apple log and validation evidence are in
`artifacts/notary-release-vlqkl64d/{receipt,apple-log,acceptance-proof}.json`.

Tickets were stapled to separate local copies of both apps and the driver;
all three pass ticket validation and strict signature verification. Both apps
pass Gatekeeper with `source=Notarized Developer ID`. The driver's execute-type
Gatekeeper assessment returns exit 3, stating that the code is valid but is not
an app; this is not a successful driver installation assessment. Apple describes
[ticket stapling and archive handling](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow).

The original approved archive and package remain byte-for-byte unchanged.
The stapled copies have a separate file inventory; their bundled original
update manifests must not be used to install those changed copies. No installed
app, driver, broker or routing state was replaced. This older existing-slot
candidate is not a complete initial installer. Root broker packaging, fresh-slot
activation, a complete distribution container, current-build notarization and
fresh-machine offline installation remain open.

Before submission, a read-only Gatekeeper assessment rejected the installed
driver with `source=Unnotarized Developer ID`. That historical result does not
establish the cause of the earlier fresh-slot activation timeout.
Apple's [notarization issue guidance](https://developer.apple.com/documentation/security/resolving-common-notarization-issues)
requires secure timestamps, and its
[distribution guidance](https://developer.apple.com/documentation/xcode/packaging-mac-software-for-distribution)
describes signing and notarizing delivery containers.

## Automatic recovery of interrupted application updates

Both user update paths now record app identities before their first file move.
They keep the original apps until the new signed files, routing restoration and
preservation checks pass, then durably commit before removing backups. Interface
updates keep the Engine running. A pending user journal blocks another update,
driver maintenance or removal, including a second check after lock acquisition.

Both ordinary user update paths now arm a temporary account LaunchAgent before
app promotion. The independent owner acknowledges the transaction, login session
and, for paired updates, its running guard before the Engine can stop. It uses a
frozen, content-hashed Python module closure in a private account directory;
replacing the apps or changing repository files does not replace the running
repair implementation. No signing configuration, credentials or unrelated
modules are copied. The job takes over after the updater releases its lock or
dies and removes its own job, permit, readiness record and managed workspace.

Within the same login, recovery restores the full saved graph. At a later login,
the tested policy repairs files without starting audio when normal startup was
off, unless the user already started the canonical session. The temporary job
does not register, unregister or enable LoopZ's normal login item. Actual
logout/login and reboot acceptance are still required; the policy has private
fixtures and the same-login path has installed acceptance.

Run `python3 scripts/manage_app_update.py --recover` as the console account,
without sudo or package options. Recovery verifies fixed app locations and the
chosen signed outcome. An uncommitted update restores the original app inodes;
a committed one retains the validated new apps and finishes cleanup. Paired
recovery loads the current saved mix and buffer choices, never a journal copy of
an older mix. Temporarily unavailable hardware stays pending for normal runtime
retry. Interface-only recovery never stops or restores an Engine session.

Installed automatic acceptance deliberately killed the updater after moving the original
interface app aside, before candidate promotion, while a three-device graph and
public recording clients were active. No explicit recovery command was invoked.
The full graph returned automatically, with 2.041 seconds of measured silence;
cleanup and the receipt completed 4.030 seconds after the updater exited. The
same public clients, device/stream identities and clock origins survived, with
no baseline/post-recovery silent samples or timestamp errors. The mic-to-monitor
route again measured 3.875 ms at 32 frames. Cleanup restored the empty original document,
its backup, undo history and saved 512-frame setting. Apps remain user-owned;
Core Audio, driver/broker, login choice and audio defaults were preserved.

The normal installed interface update also passes with the recovery owner armed:
its 7.005-second coordinated interval has zero silent samples or timestamp errors,
with the same Engine bytes, inode and process. It leaves no pending record.
Private tests cover every app move, interrupted rollback and
partial cleanup, runtime restoration failures, newer saved intent, fixed launchd
definitions, signature refusal and maintenance admission. Evidence is recorded in
`artifacts/automatic-app-recovery-delivery-proof.json`. The preceding explicit
recovery checkpoint remains in `artifacts/user-app-recovery-delivery-proof.json`.

Immediate cooperative guard shutdown and workspace reclamation now pass installed
acceptance. Signature validation is performed while routing is still running and
reused only while complete file hashes, inode identities, permissions, flags and
extended metadata match. Changed code or metadata requires a fresh check. The
guard continues observing physical IO through graph restoration, then stops before
workspace removal. Earlier slow runs and the rejected metadata-adapter attempt
are retained as separate evidence rather than reported as the final result.

Signed distribution packaging, a fresh-machine interpreter strategy, native GUI
admission during early recovery setup, actual logout/login/reboot/power-loss
acceptance and enabled-login replacement remain open. The current development
workflow uses the installed system Python and current signed app pair.

## Interface-only updates while routing continues

`scripts/manage_editor_update.py --package <signed-pair> --manifest-sha <digest>`
updates only `LoopZ.app` under the console account. It requires the package's
Engine to match the installed Engine exactly. It closes/reopens only the
verified desktop process and never stops, replaces, bootstraps or restarts an
Engine session. A nonempty saved mix and active public clients are allowed.
The one-time ownership migration has passed. Both installed application bundles
and all their contents now belong to UID 501; the backend remains root-owned.

The file transaction retains the Engine's complete contents, directory inode,
owner/mode and observed launchd process generations. Driver/broker state, saved
routing files, audio defaults and the public device catalog must stay unchanged.
Validation checks the signed interface and existing runtime capabilities. It
opens no audio stream and publishes no test device. Its passive preservation
report explicitly does not claim a waveform or sample-continuity measurement.

Login registration may remain enabled or awaiting approval if both its signed
helper and launch-agent definition remain byte-identical. The updater observes
and verifies the same enabled/registered/approval flags after replacement; it
never registers, unregisters or approves a login item. Changed registered login
helpers require a separate update path. Native enabled-login replacement remains
an acceptance gate; the current installed login choice stays off.

Private filesystem tests cover active-graph preservation, signature-validation
rollback, refusal to change the Engine, process-generation changes, malformed
metadata and login preservation. System Python passes the same checks. A
read-only observation verifies the existing installed Engine guard. Actual
installed interface replacement now passes with active audio clients: a fresh
package retains the Engine's bytes, inode and PID, public device/stream identities
and clock origin. A coordinated clean baseline and the entire 4.624-second update
interval contain zero silent samples or timestamp errors. An earlier whole-capture
attempt included silence without isolating startup from the update and remains
failed evidence. See `artifacts/editor-only-update-proof.json` and
`artifacts/active-app-update-delivery-proof.json`.

Fresh signing changes unchanged Engine/login CMS envelopes. `stage_desktop.py`
now reuses an installed component only after strict publisher verification,
matching full SHA-256 CodeDirectories for every architecture and identical
non-executable resources. It copies the complete sealed component into the
private package and rechecks source/copy hashes. A resource, architecture or code
change prevents reuse. The login helper is retained before signing its parent
GUI bundle. This makes ordinary GUI builds use the interface-only path while
retaining the exact-byte Engine/login requirements at deployment.

## Ordinary application updates and existing-owner migration

The signed desktop and per-user Engine are application components. They can be
owned and updated by the console account; the HAL driver, routing broker,
configuration and root service jobs remain protected system components.
`install_desktop.py` now assigns only the two application bundles to that
account. `install_driver_update.py` applies the same policy while keeping the
driver root-owned. Signature requirements, entitlements and installed paths
remain unchanged.

Existing root-owned apps need one administrator migration. The prepared command
in `artifacts/user-app-migration-command.txt` targets the signed latency/buffer
update. `manage_app_update.py --migrate` first verifies the signed package and
checks the actual authenticated empty graph, process identities and canonical
session job. It requests administrator authorization before stopping anything,
then closes only the account's verified desktop and empty Engine session.
After the guarded app-only installer finishes or rolls back, it restores the
same session definition, waits for the authenticated empty graph to restore,
and reopens the desktop if it was open. It does not restart Core Audio or alter
any driver, broker, audio default, login choice or saved routing document.

After migration, use `manage_app_update.py` without `--migrate` for ordinary app
updates. It delegates to `update_active_apps.py` as the console account; no sudo is
invoked. Package digests, publisher identity, complete signed-file contents,
atomic app replacement, physical-output continuity and rollback are verified.
A private user receipt is written under `~/Library/Application Support/LoopZ/Updates`.
The user updater takes the same root-owned installer lock through a read-only
descriptor, preventing overlap with system install/removal. The migration makes
this empty advisory-lock file readable; it remains root-owned and non-writable
by other accounts. Pending root maintenance journals block ordinary updates.

The managed updater now accepts a nonempty saved graph and existing public
clients. It prepares and flushes replacement files while the current Engine
processes audio, then pauses only the verified canonical Engine for promotion.
It restarts the same saved job and requires a new authenticated epoch, the exact
saved document, previously ready sources/monitors, saved buffer choices and
permission state. Already-disconnected routes retain their pending intent.
An unchanged Engine selects the interface-only path automatically. Registered
login state is allowed only when its helper/job stay byte-identical; actual
enabled-login replacement remains an acceptance gate.

Caught preparation, promotion, signature and restoration failures roll back the
files and restart the previous apps. Native active acceptance preserved three
devices, a saved 32-frame buffer and the same public IO clients. The full command
took 5.33 seconds; Engine pause through acknowledgement was 1.43 seconds, with
1.26 seconds of measured audio silence. GUI-only updates require no such pause.
Driver/broker state, audio defaults, login choice and routing files were preserved.
The lower-level idle/root migration installer retains its separate restrictions.
Installed automatic same-login SIGKILL recovery is verified above. Later-login
policy is fixture-tested; actual reboot/power-loss acceptance is not yet certified.

Maintenance understands the ownership split. Only explicit app roles may use a
regular account owner, and every nested file must share its component's owner.
Uninstall journals/quarantines and privileged components retain the root owner.
Descriptor-scoped uninstall, durable recovery and coordinated backend-update
checks cover mixed ownership; backend updates preserve an app's account owner.
`artifacts/user-application-update-proof.json` records temporary-file tests and
the verified installed migration. `artifacts/user-app-migration-verified.json`
binds the success receipt, all 11 signed payload hashes, recursive ownership,
restored authenticated session and unchanged audio defaults/Core Audio process.

## Current app-only update

The latest app-only update is now installed with manifest
`e7aeffbb2c647b7e6c9c1ee9545ecb69e813a86cb801788b44058fc1aaa52871`.
`artifacts/active-app-update-delivery-proof.json` binds the active user update
receipt and all 11 installed app payload files. It includes the direct hardware
monitor path, bounded input-clock correction and Settings buffer control.
The earlier `327280…629c4` administrator migration changed ownership successfully;
the subsequent buffer fix was installed without sudo.

Buffer discovery and Apply now execute in the Engine's process through the
authenticated editor connection. A live two-process observation showed a
32-frame setter while a second process still read 512. The original interface
process setter therefore did not configure the Engine's IO period. Settings
now reads the Engine's inventory, sends one bounded expected-current request,
and disables writes when an older/disconnected Engine cannot own the setting.
Actual monitor status includes its negotiated `bufferFrames`. Buffer choices now
persist in `routes.json.audio-settings.json`, beside the canonical document.
Only production editor-service sessions enable this store; installer validation
and private runtimes with no settings path never apply a user's stored choices.
The Engine loads preferences before acquiring the graph's physical endpoints,
retries unavailable/changed devices and retains exact UID selections. It does
not silently impose 32 frames on other applications.

Accepted, freshly validated settings are saved atomically as mode 0600 before
effects or acknowledgement. A last-valid backup permits recovery; newer schema
versions and unsafe paths are not overwritten. Save failure leaves the running
buffer unchanged. Device loss after a successful save returns saved/pending
intent and retries restoration. Settings distinguishes that state from the
actual running value. The app updater protects both settings files with the
saved mix.

Actual installed testing replaced only the verified account-owned Engine process
with three saved devices active: a 16-channel physical mic/monitor route and two
stereo virtual devices in a dependency chain. The complete document and the
saved 32-frame buffer restored in approximately 588 ms. The same public IO clients,
device/stream identities and clock origins survived. The deliberate crash
produced approximately 464 ms of silence; this is recovery, not uninterrupted
processing through a dead Engine. Stable pre/post signal checks pass, and the
mic/internal-return path measures 3.875 ms before and after. The temporary graph
was removed and the 512-frame choice saved again. Physical unplug/wake acceptance
remains open; simulated disconnect, discovery failure, changed limits and retry
are covered by native tests.
The driver, broker, Core Audio
identity, audio defaults, login choice and saved routing document were preserved.
The previous installed Quad Cortex preview check updates eight source-meter channels
without a public audio client. The desktop uses Inputs–Outputs–Monitors labels,
and newly added sources remain unwired until the user connects them.

The app and Engine are now user-owned. Ordinary application updates use the
user updater; driver/broker changes retain separate administrator installation.
Installed active-audio paired and interface replacement and explicit updater
SIGKILL recovery now pass, including automatic same-login recovery. Enabled-login
replacement and actual later-login/reboot acceptance remain open.
The local Loopback reference is
a user-owned application and a root-owned ARK driver containing the independently
versioned `arkaudiod` background application. Its separate backend setup is
documented at https://rogueamoeba.com/support/knowledgebase/?product=Loopback&showArticle=Misc-ARK-Plugin-Audio-Capture-Details.

### Earlier continuity-check repair

The installed app/driver payload still matches the successful `06b03b…a1981`
deployment, verified again across all 15 payload files. The later app-only
attempt for desktop manifest
`5c4e5602c2e61f9c7ecf47f0b0bb7397ad2f9f9d2807650336795cadf47f18dc`
failed before application preparation: the administrator-launched physical
continuity checker did not become ready. The root result records this failure,
not a successful application update. No app, driver, broker, defaults or user
document was changed by that attempt.

The app-only installer now starts its output-only checker in the console user's
launch session, matching the Engine's audio context. Only its report directory
is user-owned; the stop request stays in the protected administrator workspace.
Reports must be bounded regular files with the expected owner and device UID,
and completion must confirm positive frames, zero timestamp errors and an
explicit successful stop. The updated path passed a real Antelope continuity
check with 0.22-second startup; its read-only installation preflight also passed.
This repair is not deployed until a new root success report and installed hashes
verify the application pair. The signed app package itself is unchanged.

Use `artifacts/working-app-update.command` for the repaired app-only deployment.
It preserves the installed driver and broker. Historical staging and installation
records below retain their original package identifiers and source hashes; the
new installer revision is recorded separately.

## Verified existing-driver deployment

The reviewed package with manifest SHA-256
`267c8ffca51696d9cacc942fde13bc921ea413669241df24772e7586ffa3f7a4`
has now been installed. The root-owned success report records replacement of
the two apps and driver, a verified LoopZ-only host transition, successful
capture-free device publication/withdrawal and actual 512-frame callback
telemetry. All 15 installed payload files independently match the manifest and
retain root ownership without group/world write access. Core Audio remained
PID 29793 with its original start time; defaults and the routing broker were
preserved. The physical-output guard observed 392704 frames without timestamp
errors over 8.18 seconds. Existing microphone authorization is still available
without requesting permission.

The installed package predates the later workspace helper-ownership, recovery
notification and endpoint/order editing changes. The staging proof remains a
historical record; deployment and subsequent timing evidence are recorded
separately. Active-graph/enabled-login update, uninstall and broader live audio
acceptance remain open.

## Existing routing-driver and desktop update

### Coordinated apps, driver and broker candidate

`stage_suite_update.py` now builds a separate signed coordinated payload from a
verified app/driver package and the current broker build. Its strict outer
manifest binds the complete nested manifest and broker bytes. Five code identities
(including the nested login helper) are checked against the fixed publisher and
entitlement policy. The driver keeps its existing slot, factory and sealed
broker pin. Staging probes the signed Engine and broker only with the capture-free
`--runtime-capabilities` argument, then verifies all signatures/hashes again before
publishing. Those reports describe compiled support; they do not establish a
live negotiated session.
Capability children also drop exported signing passwords and disposable-keychain
variables from their environment.

The current candidate is recorded in `artifacts/suite-update-final-stage.json` and
`artifacts/suite-update-staging-proof.json`. It includes the latest workspace
interface, pair-removal command, higher-rate policy and broker negotiation. Its
exact signed driver passes private factory checks, and the signed desktop's
interface seal verifies. Strict policy and private four-target rollback tests
pass under both available Python versions. No installed file or service changes.

**There is no coordinated installation entry point yet.** The next required work
is a guarded broker lifecycle transaction: admit all active GUI sessions and
pending recovery journals, audit the exact running root broker and LoopZ-only
driver generations, preserve the root job/pin policy, promote all four fixed
targets, reload only those verified LoopZ services, validate real negotiated
high-rate publication under a continuity guard and restore both files and runtime
on failure. Abrupt-loss recovery also needs a durable coordinator. Staging/file
rehearsals do not certify any of these runtime gates.

The coordinated file layer now has a separate protected journal and replacement
transaction (`suite_journal.py`, `suite_files.py`). The caller must hold the shared
installation lock and supply a verified private source and the four fixed policy
targets. Original and replacement inventories, parent directory identities and
private workspace identities are synchronized before active file renames. Atomic
no-overwrite moves preserve original inodes. Recovery checks every component
before changing any, restores interrupted promotions and retains the replacement
images for subsequent runtime inspection. Closing a transaction does not delete
recovery data or choose a runtime outcome.

A durable commit may only follow verified new runtime activation; rollback data
cannot be discarded merely because files match. Likewise, restoring files leaves
the journal pending until the caller verifies the original runtime. Cleanup can
resume after an interruption inside a directory removal. A changed destination,
parent, recovery record or cleanup root blocks overwriting/deletion. Unknown
workspace identity is retained for inspection. These are process-crash and
filesystem failure tests, not a physical power-loss guarantee.

Private tests terminate child updater processes at preparation, promotion,
rollback, commit and cleanup boundaries, including partial recursive deletion.
A second rehearsal copies the actual installed and signed candidate payloads
into disposable folders and checks all four replacements and original-inode
rollback. `artifacts/suite-file-recovery-proof.json` binds the evidence. Updates
refuse pending uninstall records, and the workspace uninstaller refuses pending
coordinated updates before host lookup and again after acquiring the shared lock.
The twelve frozen legacy installers still predate this admission rule; they are
preserved for the pending reviewed command and must not be used during a future
coordinated transaction.

This file layer is not a root installation/recovery command. The broker audit and
lifecycle primitives below must be integrated with isolated-driver restoration,
complete session admission, actual high-rate publication and continuity
validation by a coordinated runtime owner. No administrator command or new
deployment is provided by these private tests.

### Broker runtime identity and transition status

`process_code.py` reads the kernel PID, effective/real/saved UID and precise start
generation. It uses public Security framework dynamic `SecCode` validation to
check the running process against a publisher requirement. Checking the current
file at the executable's old pathname is insufficient after atomic replacement.
The implementation uses the local macOS SDK's public APIs and does not execute
the inspected component or request privacy consent.

`broker_host.py` binds that identity to the fixed launchd domain/label, protected
daemon definition, exact arguments, explicit environment, managed Mach endpoint
and resident executable device/inode. The definition must retain the existing
fixed Engine trust pin and contain no extra fields or boolean/integer lookalikes.
All source, job and process identities are rechecked after inspection. Only the
canonical executable and its recorded old/new sibling slot are accepted as image
paths. An old broker may remain resident after its file is renamed; finding a
new executable at the canonical path does not certify new code is running.

`broker_lifecycle.py` requires an admission callback from the eventual transaction
owner. It unloads only the audited fixed job and waits for that process generation
to end; it uses no PID signals. Startup verifies the canonical signed image and
fixed plist, then bootstraps only that job. Startup proxies are observed until
the expected executable is running. A completed startup can be resumed without
another bootstrap. Timeout/nonzero command results are followed by observation
of the same job; they never trigger a blind command retry. Changed generations,
inaccessible identity or untrusted resident images retain the need for recovery.
Public launchctl operations cannot atomically compare a process generation with
job removal, so the runtime owner must hold the shared installation lock and
other administrator operations must honor that lock.

`tests/editor/broker_process_tests.py` performs an explicit private GUI-job
rehearsal with a native fixture that never opens audio. Its ad-hoc hardened
runtime signature and private identifier are fixture policy, separate from the
production Developer ID requirement. The rehearsal checks dynamic signature
rejection, resident old code after four-file promotion, exact replacement startup
and original-file/runtime restoration. Its abrupt-exit mode terminates only the
updater worker immediately after launchd accepts startup; a reconstructed owner
reuses the running replacement, then restores all four original file inodes and
the original broker image in a fresh process. No public bridge is registered.

`artifacts/broker-runtime-proof.json` binds those native rehearsals and the private
policy/failure-order regressions. The existing real broker job and binary/plist
bytes remain unchanged. A normal user's read-only root-job query works, but the
kernel rejects that user's detailed inspection of the running root broker. Root
identity/lifecycle acceptance therefore remains part of the eventual guarded
administrator transaction. Isolated driver runtime rollback, durable complete
runtime context, integrated GUI/session admission and real routing/continuity
validation remain required before a coordinated root installer is available.

### Isolated driver recovery status

The new driver runtime layer resolves original/replacement bundle inodes from
the fixed four-component file transaction. `driver_runtime.py` verifies the
sealed bundle and publisher, the running Apple isolated-helper signing identity,
kernel generation and known mapped executable inodes. Another HAL component,
another HAL root, an unknown/deleted image or changing process inventory blocks
the audit. Startup with no resident known image is observed without signalling.
No global Core Audio process is a signal candidate.

`driver_record.py` keeps a protected `.suite-driver-state.json` companion record
bound to the file-transaction token. It records every known image seen in each
host generation and persists a signal intent before dispatch. A host that loaded
the replacement cannot later certify original-code restoration just by unloading
that image. `driver_lifecycle.py` requires a fresh clean generation for that case.
The same updater does not repeat a signal after an ambiguous result. A fresh
recovery actor can re-audit and retry only after the prior actor's exact kernel
generation has ended. An inaccessible actor identity is not treated as exit.
Each new request still requires a complete image/role audit, admission and the
final single-PID generation check. Public PID signals are not atomically bound
to mapping inspection; the full owner must hold the shared installation lock.

The existing file-journal schema now shares protected descriptor-based record
storage with this companion; its namespace and disposition semantics remain
unchanged. New file preparation and the workspace uninstaller also refuse an
orphan driver record. Closing either record never chooses a runtime outcome or
deletes recovery data.

Native private GUI fixtures load two separately signed dylib versions, then
exercise four-file promotion, exact host replacement and original-inode/runtime
restoration. Worker processes exit before signalling, after signalling and after
recording the outcome. Another case loads both images, records the history,
unloads the replacement and verifies that the old-only mapping still requires a
fresh host. The fixtures have private identifiers and user ownership; they never
open the actual HAL, Core Audio devices or a public audio bridge. The production
Apple-helper policy and Developer ID bundle verification remain separate.
`artifacts/driver-runtime-proof.json` records the tests and limits.

The remaining coordinated owner must persist complete broker and protected-state
context before promoting files, combine all-GUI/default/graph/login admission
with hardware continuity, order both service transitions and validate actual
negotiated routing before commit. Recovery must bind both records, restore the
original broker/driver runtime and retain files on uncertainty. Terminal decisions
and cleanup ordering must also handle an orphan companion after interrupted
finalization. The driver companion alone does not solve those full-transaction
gates. Production Apple-host acceptance and a reviewed administrator entry point
remain required; no system driver was signalled by these private tests.

The existing pending package, command and twelve frozen installer modules are
unchanged. Do not pass the nested `update` package to the old driver updater to
perform this coordinated upgrade: that updater deliberately preserves the old
broker. No new administrator command is supplied until the coordinated lifecycle
and recovery path is concrete and reviewed.

### Pending driver-only recovery update

A newer combined recovery candidate is prepared with manifest
`06b03b0744aee6d309b065727d6a79f156b730421fb49bd60b60e0b8386a1981`.
It includes per-client IO reference accounting, clock preservation for
playback-only visibility changes and the current signed desktop/Engine pair.
The exact signed driver passed the private native driver suite; normal,
address/undefined and thread-sanitized checks cover both driver factories.
Four packaging/signing/rollback groups and read-only deployment/runtime
preflights passed. It is **not installed**; the authoritative installed report
still has the earlier `267c8ff…` manifest above.

Use `artifacts/combined-recovery-update-command.txt` for this candidate. It
supersedes the pending shared-client command while leaving that earlier command,
package and source archive intact. The same twelve reviewed installer modules
remain unchanged. The new package/source hashes, complete command and baseline
installed state are recorded in `artifacts/combined-recovery-update-proof.json`.
macOS requires administrator authorization for installation. After a matching
successful root-owned report, repeat the original shared-client baseline,
active-input visibility case and full mixed-source recovery sequence; the
signed/native checks do not prove those installed gates.

The newer paired updater is separate from the initial stereo migration. It
requires the existing routing implementation in `LoopZAudio.driver`, an empty
routing catalog, stopped desktop/Engine jobs and disabled login registration.
It refuses default-device dependencies, an incomplete catalog, another HAL slot,
foreign/unprotected installed code or a changed review manifest.

`stage_driver_update.py --desktop-package <signed-desktop-package>` combines a
verified Release desktop pair with the existing-slot driver build. It uses the
same disposable SingZ signing keychain, preserves the driver factory/broker pin,
and checks the pin through the real native loader. Staging changes no installed
files. `install_driver_update.py --check` is the read-only console-user preflight.
The exact reviewed administrator command is in
`artifacts/driver-update-command.txt`; its package and source hashes are recorded
in `artifacts/driver-update-proof.json`. Those installer sources remain frozen
while that administrator step is pending.

Installation atomically promotes the two apps and driver, with private root
preparation and retained backups. Before reloading, it verifies the precise
kernel generation and mapped image of the Apple-signed, LoopZ-only driver host.
It never signals `coreaudiod` or reloads the routing broker. A physical-output
continuity guard runs throughout. Capture-free publication, an output-only
callback-timing check and withdrawal must pass before success. The user routing
document, audio defaults and login choice are preserved.

Rollback checks actual mapped state even if macOS reloaded a host automatically.
A host retaining both driver images cannot prove restoration. Only an audited
LoopZ replacement may be signalled; restored file hashes, a strictly verified
old-image host and a fresh authenticated publication/withdrawal are required.
Unverified runtime restoration is reported as repair required with recovery
files retained. This path has automated failure-injection and read-only native
preflight evidence, followed by the successful deployment described above.
Lower-queue timing and active-graph/enabled-login update/uninstall acceptance
remain separate gates.

## Original stereo prototype

Build and pass the automated checks first. Stage ad-hoc signed local artifacts:

```sh
python3 scripts/prepare_install.py
```

The staging step does not install or run anything. Its default helper arguments
explicitly select a 120-second synthetic stereo test signal. This signal goes
only into the virtual input; no physical output is enabled.

Review `artifacts/feasibility/manifest.json`, then install the prepared artifacts
with administrator authorization:

```sh
sudo python3 scripts/install_local.py
```

The installer verifies signatures and complete package hashes, copies a private
verified snapshot, and prepares replacement files alongside their destinations.
An exclusive installer lock prevents concurrent replacement. It keeps originals
until activation and continuity checks pass; failure attempts file restoration
and broker recovery independently, then verifies restored hashes and runtime
state. Any incomplete recovery is explicitly marked as requiring repair. It never invokes a system audio reset.

Existing installations use a hot update:

```sh
python3 scripts/install_local.py --replace --check
sudo python3 scripts/install_local.py --replace
```

`--update-engine` is an alias for the hot update. `--update-tools` also requires
an identical helper and broker configuration. The complete installed driver
bundle must match the package: incompatible driver changes are rejected before
any file or service changes. The loaded driver is preserved, including its
inode. Updating the processing helper and DSP does not require driver reload.

Stop LoopZ's source/recording clients and engine before updating. Other audio
clients may remain active. During the update, a separate AUHAL client writes
zeros to the current hardware output and checks its sample-counter continuity.
`--guard-uid UID` selects another physical output for this check when needed.
No microphone permission or capture is required for installation validation.

Success additionally requires the virtual device to be alive, an authenticated
driver attachment to the matching broker protocol, unchanged default input,
output and system-output UIDs, and unchanged Core Audio service PID/start time.
The resulting evidence is saved to
`/Library/Application Support/LoopZ/install-result.json`.

Fresh installation only succeeds if the device actually appears and attaches
within 20 seconds without a reset. Otherwise it fails and rolls back its files. Filesystem rollback cannot
guarantee that an already-loaded plug-in has been unpublished; residual
activation and changes in runtime/default-device state are reported explicitly.
Fresh activation into an already-running audio server remains unverified.
The standard installer rejects live replacement of a changed driver. A separate,
guarded isolated-host experiment succeeded on the tested macOS release; see
[the activation evidence](latency-comparison.md). It is not a supported general
installer path. Copying a driver to disk does not establish activation.

Start the helper in the GUI login session using the generated launch agent:

```sh
launchctl bootstrap "gui/$(id -u)" artifacts/feasibility/com.lexasoft.loopz.engine.plist
/Library/Application\ Support/LoopZ/loopz-probe --seconds 5 --expect-tone
```

Run two probes at once to exercise independent clients. The synthetic test
expects 440 Hz in the left channel and 880 Hz in the right channel. After
stopping the helper, run the probe with `--expect-silence`.

For real sources, prepare the helper arguments explicitly using the application
bundle ID and microphone UID from the inventory command:

```sh
python3 scripts/prepare_install.py --engine-args-json '["--app","com.example.player","--mic","default","--seconds","60"]'
```

Restaging changes the pinned helper identity when the binary changes; reinstall
the matching broker configuration. Consent is requested by the installed
helper. `--mute-app` opts into muting the source's original playback.
`--monitor-muted UID` exercises a physical output callback silently.
`--no-pass-through` disables the public playback bus to isolate application
capture during validation. These flags are prototype diagnostics.

To stop and unregister the manually started helper:

```sh
launchctl bootout "gui/$(id -u)/com.lexasoft.loopz.engine"
```

The old `uninstall_local.py` entry now performs the guarded read-only review
described below by default. Its former unconditional prototype deletion has
been removed. A prototype-only installation must be migrated or repaired before
the modern routing uninstaller can establish its required service/tool state.

## Guarded removal preparation

The workspace uninstaller inventories the desktop/Engine pair, either supported
routing-driver slot, both current and legacy broker definitions, routing tools,
trust configuration and known system reports. It preserves user routing
documents/preferences, unknown support files and the shared installer lock.
Review the installed state without changing it:

```sh
/usr/bin/python3 -E -s -S scripts/uninstall_loopz.py --preflight
```

The compatibility entry `scripts/uninstall_local.py` has the same safe default.
Read-only review checks protected file ownership, full file fingerprints,
publisher identities for modern code, valid legacy tool signatures, stopped
desktop/Engine jobs, login registration, aggregate dependencies, audio defaults
and canonical saved-document fingerprints. It neither opens audio IO nor
signals/removes anything. A successful result is
`readyForAdministratorPreflight`, not a passed removal. The installed read-only
review found 19 present components in the current existing-slot installation.

Actual removal requires administrator execution, explicit `--remove` and an
exact `--expect-plan-sha` from a fresh review. There is no arbitrary target-path
override. Login startup must be off and the editor, Engine and dependent routes
must already be stopped; the uninstaller does not silently change those choices.
The root path serializes with other installers and repeats the plan and admission
checks before mutation. Its copied checker is usable only after private-directory,
content and signature checks. A continuity guard runs on the unchanged default
output while verified services stop and original components move into private
quarantines on their existing filesystems.

Only the isolated Apple-signed driver host with the expected mapped LoopZ
inode/kernel process generation can receive a signal. Another HAL image or an
ambiguous/replaced process aborts the transaction; `coreaudiod` is never signalled.
Driver withdrawal, unchanged defaults/Core Audio/documents and the continuity
guard must pass before quarantined files are purged. Before that boundary,
failures attempt atomic no-clobber file restoration and restore only previously
loaded brokers. Incomplete restoration retains originals and reports repair
paths. After purge begins, a cleanup failure remains explicitly committed and
retains cleanup paths; it is never presented as a successful rollback.

`artifacts/uninstall-preparation-proof.json` records temporary-file/service
rehearsals, both tested Python interpreters, driver-identity rejection fixtures,
manual review, and actual read-only preflight evidence. This is prepared workspace
code, not an installed uninstall/reinstall pass. Real driver unload/restore,
multi-user login lifecycle, abrupt-process-loss recovery, prototype-only and
damaged-installation teardown remain release gates. The pending signed recovery
package and its twelve frozen installer modules remain unchanged.

### Interrupted uninstall records

The removal path now persists a bounded, private `.uninstall-state.json` in
system support before service changes. It records the fixed plan, original
file identities/content fingerprints, parent identities, quarantine token and
private diagnostic workspace. Quarantine identities are synchronized before
their first file move. Journal writes publish atomically, synchronize the
directory and request macOS `F_FULLFSYNC` before and after publication. Hardware
power-loss behavior has not been certified.

The durable `removing` decision permits exact original-file restoration using
atomic no-clobber publication. A durable `purging` decision permits cleanup of
recorded originals, including bundles partially deleted before interruption.
Missing entries in a committed quarantine are allowed; edited or foreign data,
replaced directories, unsafe links and destination collisions block recovery.
New destinations are preserved. An uncertain commit write retains all originals
and reports `committed: null`; it does not proceed to purge or assert rollback.

Default review and explicit removal both detect pending recovery before looking
for installed apps/tools, which may already be quarantined. Workspaces remain
available while a record is pending. Verified normal completion/rollback clears
the journal only after file disposition is checked again. File-only recovery
retains the journal and explicitly requires runtime/workspace finalization.

`artifacts/uninstall-journal-proof.json` binds both Python interpreter runs and
nine actual SIGKILL checkpoints in private child processes: initial record,
quarantine record, first/all moves, commit-before-delete, partial bundle purge,
finished purge, rollback move and terminal restore record. These tests never
operate on installed components or audio services. The administrator coordinator
and scoped diagnostic/workspace shutdown below are now implemented and rehearsed.
Real service/driver restoration, power-loss acceptance and multi-user handling
remain work.
The frozen existing installers do not yet consult the pending-record marker;
that integration must follow completion of their currently reviewed deployment.
No installed uninstall or recovery operation has been run.

### Administrator recovery coordinator

An existing interrupted uninstall can now be resumed with the fixed journal:

```sh
sudo /usr/bin/python3 -E -s -S scripts/uninstall_loopz.py --recover
```

This action acquires the installer lock and follows the recorded decision; it
does not start a new uninstall or accept replacement paths. With no pending
record it returns `nothingToRecover` without querying installed tools.

Before the deletion commit, the coordinator validates all file placements,
the original console account and the recorded service/login context. It selects
the signed checker from its verified original or quarantine location. The old
private diagnostic receives only its scoped stop file; kernel executable-path
inspection must establish that it has exited before workspace cleanup. Foreign
files, unsafe entries, replaced directories and live diagnostics retain the
record for repair. A new workspace is recorded durably before a new verified
checker copy or continuity guard is used.

The guard surrounds original-file restoration, publisher validation, recovery
of only previously loaded brokers and read-only verification of the restored
driver host. The restoration observer has no driver-signal authority. Saved
documents, defaults and the current Core Audio identity are compared against
the new recovery baseline, so choices made since the initial review are not
overwritten and a reboot does not require the previous server PID. Workspace
cleanup and a final file-disposition check precede journal removal. Normal
uninstall completion uses the same workspace-before-journal ordering.

After the commit, recovery finishes only recorded quarantine cleanup and the
inactive diagnostic workspace. It constructs no runtime host, can run when the
installed apps/tools are absent, and reports any recreated targets as preserved.
Failures and repeated interruptions retain a retryable record. A restored
installation, a recovered committed removal and required repair have distinct
results.

`artifacts/uninstall-recovery-proof.json` binds coordinator/failure rehearsals,
three additional real private-child SIGKILL checkpoints during recovery, a native
CPU-only diagnostic that exits cooperatively, and a read-only rehearsal against
the installed signed checker. The native rehearsal performs no audio IO, service
changes, driver reload or installed-file edits. Actual system uninstall/recovery,
physical continuity, multi-user/damaged-installation and power-loss acceptance
remain unverified. The twelve pending installer modules and signed package remain
unchanged; their pending-journal admission integration is still deferred.

## Stable macOS engine signing

`scripts/stage_signed.py` adapts SingZ's macOS signing workflow to the native
helper. It imports an existing Developer ID Application `.p12` into a disposable
keychain, unlocks that keychain and configures its imported key for Apple's
signing tools. The keychain password is generated for that run; the certificate
export password is used only for import. The temporary keychain is placed first
in the existing search list, which is necessary to avoid resolving a duplicate
identity through a locked login keychain. Cleanup deletes the temporary keychain
and its search-list entry after success, failure or handled interruption.

Create `.local/macos-signing.json` with local file references:

```json
{
  "certificateFile": "/absolute/path/developer-id.p12",
  "passwordStore": "/absolute/path/secrets.enc.yaml"
}
```

Then stage with:

```sh
python3 scripts/stage_signed.py
```

The SOPS store uses SingZ's `mac_p12_password` field. Decryption stays in memory;
only that password is consumed. Alternatively, supply `LOOPZ_MAC_P12_PASSWORD`
or SingZ's `SINGZ_MAC_P12_PASSWORD` in the calling environment. Neither password
is forwarded to the staging process. Certificate/store paths also accept
`--certificate` and `--password-store`; arguments after `--` go to
`prepare_install.py`. `.local`, `.keys`, private-key exports and keychain files
are ignored by Git. No credentials need to be copied into this repository.

For a keychain already prepared by CI, `prepare_install.py` accepts
`--engine-signing-keychain`, `LOOPZ_ENGINE_SIGNING_KEYCHAIN` or `CSC_KEYCHAIN`,
and `--engine-signing-identity` / `LOOPZ_ENGINE_SIGNING_IDENTITY` selects the
certificate name or fingerprint. A direct staging invocation without an identity
still uses ad-hoc development signing; use `stage_signed.py` for signed updates.
The certificate applies only to the engine; driver, broker and checker retain
their existing signing policy, preserving compatibility with the loaded driver.

Certificate staging enables Hardened Runtime and secure timestamping. Unlike
SingZ's Electron app, this native helper needs no JIT or unsigned executable
memory entitlement. It carries exactly `com.apple.security.device.audio-input`
and retains its microphone usage description. It does not enable App Sandbox.
Staging verifies these properties, an Apple team identifier and a designated
requirement that is not tied to a code hash. The manifest records them. The
broker still pins the exact staged helper code hash; its authentication is
unchanged. This workflow reuses an existing certificate; it creates no new Apple
certificate and does not modify the login keychain or existing key permissions.

Use the same signing certificate class, team, bundle ID and installation path
across development builds. Switching from ad-hoc signing can require one more
initial approval. Apple's [signing requirements guidance](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements)
explains why ad-hoc identities are specific to a code version and why stable
designated requirements matter for saved privacy grants. Certificate signing
does not itself grant microphone access. Notarization, a distributable
installer and automatic login registration remain separate release gates.

## Document routing runtime payload

The new multi-device runtime uses a separate staging workflow:

```sh
cmake --build build/routing-macos
python3 scripts/stage_signed.py --routing
```

This reuses the disposable SingZ signing keychain and publishes a new
`artifacts/routing-install-…` directory. It does not overwrite the feasibility
package or install anything. It requires Developer ID Application signing for
the engine, broker, driver, configuration tool, probe, native pin checker and
separate routing installation checker.
All components must belong to the same team, enable Hardened Runtime and carry
only their expected entitlements. Ad-hoc routing production staging is rejected.

The driver declares its broker Mach service in its plist and contains a sealed
`Contents/Resources/routing-service.json`. The helper's root-installed trust file
contains the identical broker requirement. These requirements identify the
broker's Developer ID team, certificate class and identifier across updates.
The broker separately authenticates the engine and Apple's concrete audio hosts.
Staging signs the driver after writing the pin, then uses the native pin checker
to verify the actual signed bundle and exact sealed resource bytes. It publishes
the package only after these checks pass, with a complete manifest of file hashes.

The generated user-agent plist supervises document routing when explicitly
bootstrapped. Its default document path is
`~/Library/Application Support/LoopZ/routes.json`; override it with
`stage_signed.py --routing -- --document /absolute/path/routes.json`. Staging
does not create that document, capture audio, start an agent or register login
startup. A missing document leaves the runtime waiting for configuration.

This payload is not accepted by the legacy `install_local.py` workflow. The
separate initial-activation installer first supports an ordinary-user preflight:

```sh
/usr/bin/python3 -E -s -S scripts/install_routing.py --package /absolute/package/path --replace-engine --check
```

The result includes the reviewed manifest SHA-256. Installation requires that
exact digest and macOS administrator authorization:

```sh
sudo /usr/bin/python3 -E -s -S scripts/install_routing.py --package /absolute/package/path --manifest-sha REVIEWED_SHA256 --replace-engine
```

The initial migration requires a stopped Engine/user job and an empty production
routing catalog. Existing legacy audio objects can remain active: their driver,
broker, tools and daemon are preserved. The signed routing probe is installed
under `Application Support/LoopZ/routing` rather than replacing its legacy tool.
The new driver is added separately. An identical installed routing driver keeps
its inode; a changed loaded driver is rejected before mutation and needs a
separate, verified isolated-host update path. Active-graph updates and persistent
background/login integration remain unfinished product acceptance work.

The root transaction uses a private verified package snapshot and retains
original files until actual publication, withdrawal and continuity checks pass.
A temporary console-user helper runs with capture and physical monitors disabled,
publishes a private validation device and withdraws it before exit. Defaults and
the system Core Audio process identity must remain unchanged. The normal user
document is never written, and the generated login/user job is not registered.
Failure attempts file/service/runtime restoration and reports any repair needed;
adjacent backups are retained when repair is incomplete. A newly loaded driver
image can remain resident after file rollback, even with no published devices.

The prepared command and manifest identity are recorded in
`artifacts/routing-install-command.json`. Current evidence is in
`artifacts/routing-install-proof.json`. The first fresh-slot activation failed;
the separately authorized existing-slot migration subsequently passed, recorded
in `artifacts/routing-migration-installed-proof.json`. The installer writes actual
results to `/Library/Application Support/LoopZ/routing-install-result.json`.

## Permission diagnostics

The installed helper can report microphone authorization without connecting
the bridge, opening audio devices or displaying a permission prompt:

```sh
"/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine" --permission-status
```

`--request-microphone` performs an explicit foreground request only when the
status is undetermined. Its bounded wait services the main run loop and reports
the resulting status as JSON. A denied or restricted grant must be handled in
macOS settings. Run these commands from the user's ordinary session; a tool
sandbox can report its own denial instead of the installed helper's actual
authorization. System audio capture has a separate permission.

The first guarded fresh-slot activation timed out waiting for the routing
driver and rolled back successfully. Its result is recorded in
`artifacts/routing-activation-failure.json`. Do not treat the prepared package
or preflight as installed activation evidence. The separately signed migration
for the already-discovered legacy slot passed after explicit authorization; see [the existing-slot migration](routing-migration.md) for its
guards, rollback and remaining fresh-install limitation.

## Saved routing configuration

The macOS build includes `loopz-config` for configuration files. It does not
publish devices or start capture itself:

```sh
build/loopz-config create /absolute/path/routes.json "Studio"
build/loopz-config read /absolute/path/routes.json
build/loopz-config validate /absolute/path/routes.json
build/loopz-config rename /absolute/path/routes.json DEVICE_ID "Calls"
build/loopz-config resize /absolute/path/routes.json DEVICE_ID 8
build/loopz-config disable /absolute/path/routes.json DEVICE_ID
build/loopz-config enable /absolute/path/routes.json DEVICE_ID
build/loopz-config delete /absolute/path/routes.json DEVICE_ID
```

`write PATH` reads a complete versioned document from standard input. Saves
validate before changing files, atomically replace mode-0600 files and retain
the previous valid document as `.backup`. Reads report backup recovery rather
than silently repairing a corrupt primary. Future schema versions block loading
and replacement. The caller must serialize writers; concurrent readers see
complete documents. Controller integration and automatic route restoration
remain pending.

Disabling preserves source settings and wiring. Deletion rejects a device that
another stored virtual source still references, including disabled sources;
remove those source references explicitly before deleting the device.

## Timestamped stability check

After updating the installed helper and signing `build/LoopZ Test Source.app`,
run `python3 scripts/soak_test.py --seconds 7200` from the GUI user's session.
The selected fixture outputs only to LoopZ Audio. The helper captures that app
with original playback muted, explicitly disables pass-through, captures the
built-in microphone at 0.01 mix gain and exercises the built-in speaker callback with zeros.
Two independent clients validate each second of output with fixed memory.
Only measurements are saved; no microphone recording is written to disk.

The test stops its two manually registered agents on completion or handled
interruption. It does not register either agent for login. A shorter duration
is a rehearsal and cannot satisfy the two-hour gate. Results and tested binary
hashes are written under `artifacts/soak-*/result.json`.

For the current explicit low-latency configurations:

```sh
python3 scripts/soak_test.py --seconds 20 --latency-frames 768 --driver-period 1024 --driver-queue 0 --without-microphone --without-monitor
python3 scripts/soak_test.py --seconds 600 --latency-frames 1024 --driver-period 128 --driver-queue 0 --exercise-client-churn
```

The first matches the fresh Loopback app-only setup. The second retains the
microphone and muted monitor and opens/closes an extra recording client every
ten seconds. The controller configures the driver's advertised source delay
and the helper's actual lookback together. A post-recording telemetry checkpoint
must acknowledge completion before the helper is stopped. Neither short run
satisfies the two-hour gate. The helper default lookback remains 2048 frames;
general mixdown may require that larger margin.

The smaller-block app-only experiment requires driver build `loopz-timing-3`
and the matching helper:

```sh
python3 scripts/soak_test.py --seconds 20 --block-frames 64 --latency-frames 192 --driver-period 128 --driver-queue 0 --without-microphone --without-monitor
```

The 192-frame lookback is 4 ms at 48 kHz and 4.35 ms at 44.1 kHz, before
measurement uncertainty. This is a requested setting, not a measured result.
Repeat the short strict rehearsal at both rates before any longer regression.
Small blocks require one eligible device-stream app capture and explicitly
disable microphone, monitor, tone, mixdown and pass-through. The graph remains
prepared for a maximum of 256 frames; actual render calls use 64 frames. Capture
history still accepts callbacks up to 4096 frames. The default render block,
source lookback and driver read queue remain unchanged.


## Desktop and background Engine deployment

The full routing backend must already be installed and running. The local
`desktop-review` pair can now be deployed separately, preserving the existing
HAL bundle, root broker, trust pins, Core Audio process and default selections.
The package must be a Release build with the exact signed editor, Engine and
nested login-helper identities. All files must match the reviewed manifest.

```sh
/usr/bin/python3 -E -s -S scripts/install_desktop.py --package /absolute/desktop-review --replace-engine --check
sudo /usr/bin/python3 -E -s -S scripts/install_desktop.py --package /absolute/desktop-review --manifest-sha REVIEWED_SHA256 --replace-engine
```

The first desktop install targets `/Applications/LoopZ.app` and the existing
`/Applications/LoopZ Engine.app`. A changed existing editor additionally requires
`--replace-editor`. This initial deployment path refuses an active Engine,
loaded background session, nonempty routing catalog or registered login helper.
Active graph updates and login-helper re-registration need the separate future
update transaction. No existing session is force-stopped by this installer.

The administrator transaction copies and re-verifies a private package snapshot,
normalizes and synchronizes replacements privately, then moves complete app
bundles atomically on the same filesystem. Cleanup uses recorded directory
identities and directory descriptors, rather than following mutable paths in
`/Applications`. Original apps remain as adjacent rollback bundles until all
checks finish. A partial promotion, failed capability check or failed native
activation attempts file restoration and reports incomplete recovery explicitly.

Validation reads the installed UI resource seal, Engine capabilities and actual
login-registration state without opening a window or registering anything. It
then publishes one temporary capture-free stereo device using the installed
Engine and protected installed tools. The device must appear with the expected
channels/rate/timing and be withdrawn before the temporary user job is removed.
The validation Engine has a 90-second lifetime bound. A separate zero-output
physical client checks hardware timestamp continuity. Saved user routing files
and the production session job are never created by installation validation.

Results are written atomically to
`/Library/Application Support/LoopZ/desktop-install-result.json`. Preparation and
read-only preflight are recorded in `artifacts/desktop-deployment-proof.json`;
they are not evidence of administrator deployment. The exact prepared local
command is `artifacts/desktop-deployment-command.txt`. Do not rerun the initial
HAL-slot migration command to update these apps.

After successful deployment, opening LoopZ starts/reuses its supervised Engine
from the canonical paths. Creating the first routing document remains explicit.
Login startup remains off until the user chooses it in Settings. The installed
application launch, login behavior, broader hardware recovery and active-update
workflow remain acceptance gates after this deployment step.

## Guarded removal status

The workspace uninstaller defaults to a read-only preflight. It reviews fixed
installed components, audio defaults, saved routing, the Core Audio process and
every discoverable active GUI session. Each account must have LoopZ jobs and
login startup off. Another user's login-status query requires administrator
context; preflight does not stop that user's jobs or change their settings.
Session identities and the console account are checked again during admission
and before the isolated driver transition.

Snapshots retain unavailable login status explicitly when the desktop is in
quarantine. Removal may then use the same session's previously verified off
choice; recovery permits a recorded account's new session generation and reads
fresh status after restoring the desktop. New/unrecorded accounts block that
fallback. Logged-out users' registrations are outside this active-session check.

`artifacts/uninstall-sessions-proof.json` records private regression fixtures and
a read-only rehearsal on the current single-user installation. Full installed
removal/reinstall, other-user login registration, physical continuity and abrupt
loss still require acceptance. No system removal has been performed, and this
workspace feature is not included in the frozen recovery update package.
