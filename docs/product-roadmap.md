# Full LoopZ implementation and acceptance

## Latest installed Engine and driver observation, 21 October

The installed signed app pair now has manifest
`fef199c33a8ecdb6cf02f0298c85d822226ed1f13be620d16fbafd4d0ac16e19`.
The pinned graph-containment overlay reduces per-sample counting overhead while
preserving exact finite output and saturating diagnostics. The original
196-file SingZ archive stays unchanged. Release, sanitizer, portable and upstream
graph golden checks pass. Matching offline graphs improve from 486 to 162
microseconds per block; the complete portable scene with online silent sources
and meters improves from 979 to 650. These are CPU measurements, not physical
latency results.

The driver observation package `08e24e68…e1b1748` is installed. Its separate
actual/nominal property is verified against the loaded HAL endpoint. The
administrator step is finished. The subsequent app update runs as the user,
without sudo, and preserves that driver, the broker and login choice.

Private and installed mixed-graph runs each pass six simultaneous strict
30-second 2/8/64-channel recordings through 32 devices with zero missing samples,
cadence/timestamp errors or added renderer deadline misses. Exact app bytes,
unchanged interface resources, original files, empty history, canonical session,
audio defaults, physical 512-frame buffers, driver/broker and Core Audio are
verified. Evidence: `artifacts/graph-containment-delivery-proof.json`.

Full 32-independent-wide acceptance remains open. Both current diagnostic runs
fail preparation before recording starts: the observer's initial 32-device query
and its later four-device query each time out without a result. The runtime
snapshots show slowly advancing public-timing batches and intermittent restored
state. The failures are retained. No new raw actual/nominal measurements under
wide load, clean full-width capacity, physical latency or matched Loopback result
is claimed. Next isolate the slow native metadata call and the timing transitions
while preserving strict recorded-sample validation.

## Driver observation preparation before deployment, 21 October

The signed driver candidate has manifest
`08e24e68f8b07cd934e8fd0ad5c6ebf87b23e9831b3479cf18acba051e1b1748`.
It adds a separate read-only `lzcb` property reporting per-session maximum
accepted WriteMix frames and maximum nominal IO period. The legacy `lztm`
version-one, four-key dictionary and its conservative routing policy are
unchanged. The app payload is byte-identical to the installed `13ea…c217` pair;
the broker pin, transport protocol, buffers, scheduling and latency approval
mechanism are unchanged.

The counter is portable and uses one lock-free 64-bit word for a session and
both maxima. A stale callback cannot overwrite a reset session. Values are
session maxima, not a claim that both maxima came from one callback or that
frame count establishes callback arrival cadence. Portable concurrency/reset
checks pass. Release and sanitizer driver-library tests pass both driver slot
names, including distinct actual/nominal sizes, multiple clients, legacy
decoding, clock continuity and existing transaction/recovery cases. These use a
fake host and do not establish live HAL acceptance.

A bounded read-only probe verifies LoopZ virtual identity, validates the
versioned dictionaries, and opens no audio. Its actual installed-old-driver
compatibility check passes with `supported=false`, without a permission request;
the original empty document, app/driver bytes, settings and audio defaults are
restored. Nineteen installer-policy tests pass. The exact signed package and
frozen installer sources pass preflight. At this earlier recorded state, the prepared Terminal command awaited the Mac
administrator credential and the new driver was not installed. Deployment has
since passed as recorded above. Earlier evidence:
`artifacts/callback-observation-candidate-proof.json`.

After authorization, verify the actual loaded driver and collect both maxima
during the owned 32-client fixture. The read-only observer is prepared, but no
new live measurements or timing-policy fix are claimed. Full 32-wide stability,
latency/visibility continuity and the complete Loopback objective remain open.

## Latest installed update — batched equal-rate input reads, 21 October

The installed signed app pair now has manifest
`13ea46db0c47d8c32360a9b3653f229cb7b5fc32f8b98b26656402fa8940c217`.
Aligned equal-rate public input reads use bounded 32-frame transport tiles,
retaining every frame's atomic publication checks, missing-frame counters,
retirement/heartbeat guards and finite containment. Fractional and different-rate
reads retain the reference resampler. A strict interior margin avoids an actual
floating-point exponent-boundary mismatch; the unguarded candidate fails the new
regression, while the original and corrected readers match exactly. No buffers,
render lead/cadence, source budgets, driver protocol or SingZ DSP graph execution
change. The pinned SDK's 196 files still verify unchanged.

Seven interleaved matching-binary rounds measure the aligned 64-channel/64-frame
adapter at 6.208 versus 3.625 microseconds median-of-medians, about 1.71 times
faster. This is an adapter benchmark, not a whole-app or physical-latency result.
Release and sanitizer checks pass, including the full conversion matrix,
retired/partial/ring-wrap reads and the boundary regression. Diagnostic results
now include the first cadence anomaly and host underrun/deadline counters, with
the latter explicitly covering preparation and shutdown as well as recording.

Full-wide stability remains failed. An early private candidate has 32 clean
native ten-second recordings, but its original verifier incorrectly applies live
timing invariants after the readers stop. That failure is retained; corrected
replay passes, and a fresh repeat is still required for acceptance. Subsequent
repeats fail: six clients develop one cadence error and zeros each when their
reported periods grow from 64 to 128 and live latency reconfiguration begins;
the final guarded candidate has one reader with zeros and no cadence error.
The latter trial completes control calls within 112 ms. The verifier retains
strict measured samples and live graph timing, while allowing post-stop timing
renegotiation with an unchanged document and no reported audio errors.

Private and installed mixed-graph repeats each pass six simultaneous strict
30-second 2/8/64-channel recordings through 32 devices, without missing samples,
cadence/timestamp errors or added renderer deadline misses. The app-only update
requires no administrator access. Exact app bytes, signatures, unchanged
interface resources, original files, empty history, canonical job, physical
512-frame buffers, audio defaults, login choice, driver/broker and Core Audio are
verified; the desktop is reopened. Evidence:
`artifacts/public-block-input-delivery-proof.json`.

Next, measure the separate actual driver WriteMix and nominal IO observations
after installing the prepared candidate above; the currently installed property
combines both in one high-water value. Apple's driver contract requires native approval
for presentation-latency changes, so bypassing that approval is not a valid fix.
Investigate both period-growth reconfiguration and capture-publication margin
without hiding failures through larger buffers. Visibility, long/physical and
matched Loopback measurements, compatibility, lifecycle and distribution gates
remain open. The older notarization ticket does not cover this build.

## Earlier installed update — coherent timing and responsive control, 21 October

The installed signed app pair now has manifest
`750998b467d306f067f0eae14a3b6d1da232d5543fe4595792271e3b372ed051`.
Each graph submission freezes one timing plan for transport allocation and DSP
preparation. A new IO session without measured callbacks retains the last actual
period until a fresh callback supplies evidence; genuinely larger periods and
legacy timing reports remain conservative. Read-only native public timing now
runs in one background batch, with generation rejection, immutable snapshots,
bounded refresh frequency and no query or new mutex in the render callback.

Release and sanitizer timing/runtime/diagnostic suites pass. The new timing
regressions reject reconstructions of the former two-query controller and
unknown-period policy. A signed private run now opens all 32 independent
64-channel clients and completes their strict ten-second recording windows.
Its maximum completed control call is 107.3 ms, versus earlier reply timeouts.
Full-wide audio acceptance still fails: ten clients return zero-filled samples,
and the renderer adds 6,443 publication deadline misses. All measured-window
snapshots report restored; the final snapshot after stopping the readers reports
pending restoration. No callback cadence or timestamp error occurs. Retain the
failed verdict and inspect render cost, source availability and native stop
transitions before repeating this capacity gate. The identical markers across
devices do not establish independent-device crosstalk isolation.

Both private and installed six-client regressions pass six simultaneous strict
30-second 2/8/64-channel recordings through the 32-device chained/fan-out graph.
All twelve recordings have zero missing/nonfinite samples, cadence/timestamp
errors or render failures. Neither trial adds a renderer deadline miss. The
user-owned app update requires no administrator access; exact app bytes,
signatures, unchanged interface resources, original files, empty history,
canonical job, physical 512-frame buffers, defaults, login choice, driver/broker
and Core Audio are verified. The desktop is reopened. Evidence:
`artifacts/timing-observer-delivery-proof.json`.

Next: profile the full-wide render path and sample availability, then repeat
strict capacity acceptance after a measured fix. Visibility continuity,
physical/analog latency and a matched Loopback reference remain separate open
gates, along with compatibility, lifecycle and complete distribution acceptance.
Apple accepted the separately approved older archive; its ticket does not cover
this newer installed build. The full Loopback objective remains incomplete.

## Earlier installed update — responsive hardware discovery, 21 October

The installed signed app pair now has manifest
`b9142247dd41f9c1236eb45066422fcdc67188cb6e96d3247a91047920b045b3`.
The 32 independent 64-channel client experiment exposed a control failure before
recording: snapshots exceeded the unchanged three-second reply deadline. A live
stack sample of a private optimization shows the complete two-second sample in
hardware discovery called by `DeviceSettings::snapshot`. Caching immutable public
device validation alone did not close that failure.

Native read-only discovery now runs one background query at a time. Snapshots
return cached names and explicit refreshing/freshness state; invalidation rejects
old-generation results. Query failure preserves names and retries. Shutdown does
not join a potentially blocked native read, and the worker owns no inventory,
Engine or buffer-writer reference. Pending discovery neither writes a physical
buffer from stale data nor revokes an already-applied preference. Explicit buffer
validation and expected-value writes remain synchronous. Public timing still
resolves each UID and reads period/session/activity on every probe, while caching
model/transport/capability validation per object. Replacement, disappearance,
read errors and audio-service reset invalidate that validation.

Normal and sanitizer runtime/timing/inventory/settings suites pass. Blocked-worker
tests cover responsive snapshots, a single in-flight query, stale-result rejection,
failure/recovery and safe destruction. A readiness-controller bug was also fixed:
an initial unhealthy observation must not prevent the two-second timer from
starting when signal arrives on unchanged mappings. The original failed trace
replays successfully after 3.169 seconds, and regressions retain the full two
healthy seconds after either signal loss or a timing change. This replay is not
waveform acceptance; fresh native recordings provide that evidence.

Both private and installed repeats pass six simultaneous 30-second 2/8/64-channel
recordings through a 32-device chained/fan-out graph at actual 64-frame callbacks.
All twelve recordings have no zero/nonfinite samples, cadence/timestamp errors,
render failures or point overflows. Neither run adds a renderer deadline miss.
Saved buffer preferences remain applied, including observed pending refreshes.
The initial full-wide control test observes a maximum completed call of about
971 ms after the change. This is not a general response-time guarantee.

The separate 32-independent-64-channel experiment still has no strict recording
result: its corrected controller reaches a client-preparation deadline. Keep that
capacity gate open; do not reinterpret the six-client pass as 32-wide acceptance.
The first failure's cleanup also timed out. Its separately guarded LoopZ-only
recovery restored exact original empty route bytes and canonical state; no global
audio service was restarted. The latest app-only installation and all later
fixtures preserve original files, settings, defaults, 512-frame physical buffers,
login choice, driver/broker and Core Audio. Interface resources are unchanged and
the desktop is reopened with empty fixture history. Evidence:
`artifacts/background-discovery-delivery-proof.json`. The full Loopback objective,
visibility continuity, sustained/physical/compatibility and distribution gates
remain incomplete; the older Apple ticket does not cover this build.

## Earlier installed update — bounded producer settling, 21 October

The user-owned signed app pair now has manifest
`be0f6d9996df295f8e477c94be9a336e00719045f5d2766a2f56be48f0482d3d`.
During a playback-visibility edit, an already-active producer keeps its exact
capture mapping warm until native configuration completes and for a bounded
500 ms settling window. The first IO resume no longer cancels that window,
because the same approval can pause and resume IO several times. Pending holds
expire after ten seconds; idle or retired sinks and new-format storage are
excluded. Commit and abort both release to the bounded settling window. A real
client stop within that window can defer source teardown or original-playback
unmuting until the grace expires and the normal control poll runs.

Release and sanitizer renderer/runtime checks pass, including repeated native
pause simulation, timeout, commit/abort, retired/idle exclusion and input-format
isolation. With the same synchronized input-only diagnostic, the unchanged
baseline returns 120 zero samples and the first hold candidate returns 84. Two
revised signed private trials return zero zero samples, with no driver capture
miss increments during their eight edits. Their baseline, unrelated 28-second
reader and fresh-reader controls are clean. These are sequential trials, not
statistical acceptance. All native continuity verdicts remain failed: the revised
trials have 16 and 15 forward gaps, with maximum host jumps about 279 and 283 ms.
All anomalies fall within edits. No new physical or matched Loopback latency is
measured. Evidence: `artifacts/visibility-profile-comparison.json`.

The app-only update passes and restores the saved empty graph without sudo.
Driver, broker, Core Audio, defaults, login choice and saved document are
preserved. The synchronized diagnostic is now included in the installed Engine.
The same fixture through the normal installed session captures 1,263,936 edited
frames with zero zero-filled samples and no capture-miss increments. Its clean
baseline, unrelated and fresh-reader controls pass; 15 forward callback gaps
remain, with maximum host jump about 239.533 ms. Mapping generations and both
1,088-frame scene latencies remain unchanged. Exact installed bytes, signatures,
original files, empty history, 512-frame hardware buffers and reopened desktop
are verified in `artifacts/visibility-settling-delivery-proof.json`.
Full edited-input continuity and the complete Loopback objective remain open.

## Earlier installed update — full-width graph transition and metering, 21 October

The signed user-owned app pair now has manifest
`ca8b00a79a2fa0b5679a692a92a6749cc7d25dc038e6bc21a248394cebccf6ba`.
A new installed test first reproduced a real overload: adding 31 active
64-channel virtual fan-outs to a running 64-channel route produced 48,504,256
zeroed samples in its existing recording. Callback cadence stayed intact;
renderer backlog and unavailable public capture frames caused the gaps.
The failed run and a separate three-second profile remain retained.

Two measured costs are removed. Devices without enabled monitors now use their
public sink directly, avoiding unused monitor history allocation/publication.
Preparing or rejecting monitor removal leaves the current physical monitor
unchanged; committed removal closes it and re-enabling restores it. Equal-rate
output conversion copies sanitized history with the existing one-sample delay,
including initial/reset padding and ring wraps. Different-rate filtering is
unchanged. Buffer sizes, timing budgets and scheduling policy are unchanged.

The exact failing workload passes after this app-only update. One existing
64-channel client records 1,440,512 frames over 30 strict seconds at actual
64-frame callbacks while 31 more active 64-channel devices are added, held for
more than ten seconds and removed. All 32 published devices have 64 input/output
channels and valid signal on every output meter. The original client's waveform
has zero zeroed/nonfinite samples, callback sequence/frame/host-time cadence
errors or render failures. Its mapping, latency and saved definition stay stable
in live observations. The recording is never reopened. After input shutdown,
the remaining writer can renegotiate its period; a retained verifier failure
incorrectly applied live-mapping invariants to that later snapshot. The corrected
verifier retains strict native waveform/cadence checks for the complete recording.

The parent build added 1,065 late publications through capture completion despite
its passing waveform. The latest update avoids scanning already-cleared source
samples to calculate silent meters. Positive frame counts retain valid silent
meters, while idle meters still clear their observation. Online partial-channel
audio retains normal metering even when every frame has some missing channels.
Atomic ordering, buffers, source timing and scheduling policy remain unchanged.

Two actual installed repeats capture 1,440,512 and 1,440,448 frames without zeroed
or nonfinite samples, timestamp/callback-cadence errors or render failures. The
same continuously open root client survives graph addition/removal; all 32
64-channel devices have live outputs and all 31 offline pass-through inputs have
valid silent meters. Live root latency stays at 192 frames in both runs, matching
the parent run. The new runs add 3 and 0 late publications respectively; neither
adds a miss during its greater-than-15-second full-graph hold. Maximum lateness
is 464,833 ns and zero respectively. Transition/load margin still needs sustained
and worst-case acceptance. This is one wide reader, not 32 simultaneous readers.

Release, sanitizer and backend-disabled portable scene regressions pass, including
silent/active/idle meter publication with concurrent observers, no callback heap
events, and partial-channel recovery. Native runtime and renderer checks pass.
The parent's full endpoint conversion matrix and monitor checks remain retained.
An interleaved offline benchmark improves equal-rate 64-channel/64-frame output
conversion from 7.333 to 2.375 microseconds median, about 3.1 times faster; this
is neither a whole-app speedup nor a physical latency result. Exact signatures,
app bytes, unchanged driver/broker/Core Audio/defaults, original saved files and
settings, 512-frame hardware buffers and canonical session are verified. The
desktop is reopened with no fixture history. Interface resources are unchanged.
The new offline scene benchmark improves the disconnected-pass-through case from
950.792 to 876.042 microseconds median (about 7.9% less render time). Its online-silence
control takes about 4.5% longer; both results are retained, without a general speedup
claim. Evidence: `artifacts/silent-meter-delivery-proof.json` and
`artifacts/silent-meter-repeat-delivery-proof.json`. The prior native failure,
profile and conversion acceptance remain in the parent
`artifacts/capacity-transition-delivery-proof.json`.

The parent capacity manifest below is historical. Remaining full-product gates
include renderer margin, long/worst-case load and simultaneous wide consumers,
the original physical-input dropout, matched Loopback/analog latency, actual
hotplug/sleep, real browser/calling/DAW compatibility, initial native installation,
installed high rates and distribution/lifecycle acceptance.

## Earlier installed update — capacity and strict capture, 21 October

The signed user-owned desktop/Engine pair is installed with manifest
`bc9b3dcba0e5d3baee827b4000053e53d5c23f76e8c9602adb51ae33286482e1`.
Device discovery now skips unused channel-label reads and filters LoopZ endpoints
before expensive metadata. Satisfied hardware-buffer preferences avoid redundant
enumeration and writes. The portable routing compiler omits silent inputs and
unused processing while preserving input validation, original source order,
SingZ DSP and public meters. An interleaved offline benchmark improves the
64-channel live-plus-silent case from 54.625 to 15.041 microseconds median, about
3.6 times faster. This is not a whole-app or latency speedup claim. Read-only
discovery agrees with the full SingZ inventory, including buffer bounds, and
measures 4.372 ms versus 174.427 ms on this host.

Actual installed acceptance publishes 32 virtual devices, three eight-device
chains and seven fan-outs, with 2-, 8- and 64-channel layouts. Six simultaneous
clients each record a strict 30-second window at actual 64-frame callbacks.
Every client captures at least 1,440,000 frames, with zero zeroed/nonfinite
samples, callback sequence/frame/host-time cadence errors or render failures.
All 32 devices remain active. The full saved fixture, Engine epoch, mapping
generations and timing remain stable in 42 control observations. Contributing
sources add no missing frames and no new publication deadline miss occurs.

An earlier test incorrectly let a root writer stop while downstream clients
were still recording. The final diagnostic waits for all clients and two seconds
of stable graph timing, checks every sample without a capture-window warm-up
exemption, and retains writers until all readers finish. An initial stricter
attempt also caught ongoing startup timing negotiation and is retained. The
successful strict window begins 3.900 seconds after all clients report prepared.
Its 27 prior lifetime publication misses belong to startup and remain reported;
they are not presented as a clean entire session. Genuine pre-optimization
control timeouts, deadline misses and failed recordings are also retained.

Focused Release/sanitizer checks and a backend-disabled portable-core build
pass. Exact installed signatures/bytes and original file/settings restoration
are verified. Driver, broker, Core Audio/defaults, permissions and login choice
remain unchanged; Zen and Quad Cortex buffers remain 512. The desktop is
reopened with the original empty mix and no test history. Interface resources
are unchanged. Evidence: `artifacts/capacity-optimization-delivery-proof.json`.
Earlier app-pair identities below are historical; their source policies remain.

This closes this bounded capacity scenario only. Sustained/worst-case source
and monitor load, startup deadline behavior, the original physical-input miss,
matched Loopback/analog latency, physical hotplug/sleep, real browser/calling/DAW
compatibility, initial native installation, installed high rates and full
distribution/lifecycle acceptance remain open. Full Loopback functionality is
still the completion boundary.

## Installed app relaunch acceptance — 20 October

The installed fixed-tap app pair now passes a source app exit/relaunch while
two original recording clients remain open and a distinct peer app records
continuously. The source uses an owned marker application with a second virtual
output prepared; the peer emits distinct 1300/2600 Hz tones from a different
bundle identity. All endpoints are temporary owned virtual devices.

Both affected clients preserve their public device/stream identifiers and clock
origins without reopening. Original-playback muting is restored automatically
when the relaunched app is captured. The saved six-device graph, bindings and
controls remain byte-equivalent in every observed document. Capture readiness
is observed about 2.112 seconds after requesting relaunch. The affected clients
record about 3.455 seconds of silence including the intentional app absence and
reacquisition; affected audio is not claimed continuous while its source exits.
Baseline and recovered marker windows pass without missing/zeroed samples,
timestamp errors or render failures.

The unrelated peer records for 28 seconds with actual 64-frame callbacks,
zero zeroed samples, quiet frames, timestamp errors or bad signal windows. Its
capture acquisition count and lifetime missing-frame count stay unchanged.
The Engine epoch remains the same during the source fault. Cleanup removes all
fixtures and restores original saved files/settings, 512-frame hardware periods,
canonical session and desktop. Driver, broker, Core Audio/defaults, permissions
and login choice are preserved; installed app bytes are unchanged.
Evidence: `artifacts/app-relaunch-policy-delivery-proof.json`.

An initial harness incorrectly required internal mapping generations to remain
unchanged after recording clients had already closed and idle timing adapted.
That failure is retained. The final test checks those mappings while clients
are open; independent native observers check public identities and clock origins
through each complete client lifetime. No public continuity check was relaxed.
Engine failure while original playback is muted, physical/browser/calling/DAW
compatibility and wide/load/sleep lifecycle remain open.

## Earlier installed update — app preview and original playback, 20 October

An installed three-device test reproduced a user-facing error: previewing app
meters muted the app's original playback even with no recording clients.
Preview demand is now separate from actual public/monitor demand. Preview can
acquire live meters and follow virtual dependencies while leaving the app's
original playback enabled. Disabled devices remain inactive. Any demanded
route's mute-original setting still applies to a shared app provider.

Changing the recording tap's live mute description caused actual sample gaps
without a C++ provider reopen. The final implementation keeps recording and
mute tap descriptions fixed. A separate private aggregate drains the
mute-when-tapped control tap only while muting is required; starting/stopping
that reader switches original playback without changing recording IO/history.
The original unmuted recording tap remains continuous. Capture teardown stops
the mute reader before destroying its tap. Unsupported/failed policy changes
still take the guarded whole-group close/retry path.

The final signed desktop/Engine pair is installed without sudo, manifest
`68f343aec40479db83b133f7f8f0d5975a69fb5e99acaaf0e3de799eeab5f3f4`.
Ten installed phases verify preview, two simultaneous consumers, combined mute
ownership, policy toggles, automatic original-playback restoration, independent
half-gain/source mute, and capture release after preview ends. Both original
48 kHz recording clients run for 12 seconds with actual 64-frame callbacks;
policy changes occur after their one-second warmup. Both report zero zeroed
samples, silent frames, timestamp errors, callback failures and bad signal
windows. One shared app provider is acquired once throughout. Independent gain
and source mute are checked by rendered meters after the recordings finish.
Original playback is observed on the actual virtual-output path, not acoustically.

Focused Release and sanitizer source/scene/runtime checks pass, along with a
backend-disabled portable-core build. Earlier direct-property and unread-tap
failures remain recorded. The driver, broker, Core Audio process/defaults,
permissions, login choice and original saved files/settings are preserved;
hardware buffers remain 512 and the desktop is reopened. Evidence:
`artifacts/app-playback-policy-delivery-proof.json`.

This closes the tested owned-app virtual fan-out/mute policy case. Wider app
and multi-output compatibility, crash/relaunch while muting, sustained load,
physical dropouts and reliable 3–5 ms total recording remain open. The prior
setup/publication Engine and backup-feedback GUI hashes below are historical.
The backup warning and its interface bytes remain present.

## Installed cold-start acceptance, 21 October

The current signed pair, manifest `ca8b00…ccf6ba`, now passes actual canonical
session startup with both saved files unreadable, an unsupported newer primary
plus a valid older backup, and an unreadable primary plus a newer backup. The
snapshot has no loaded graph and does not offer creation. Authenticated edit and
initialization attempts are rejected without changing either file. A valid
primary restores the empty original document in that same process. A newer
backup still blocks saving over it after the primary becomes readable.

With both files absent, the snapshot permits explicit initialization. That
authenticated request creates a real two-channel input-only device with no
sources or monitors; no device exists before the request. A repeated initialize
is rejected. The first saved definition restores after an actual Engine restart,
with the same public UID and no new capture consent. The fixture deletes its own
device and restores the original primary, backup, settings, empty history,
canonical session, desktop, defaults and 512-frame hardware settings. Core Audio,
driver/broker, login choice and installed app bytes remain unchanged.

The debug client now exposes the existing initialization API; its bounded input
and production trust/revision checks remain in force. Existing normal/sanitized
private WKWebView cold-start guidance checks match the current installed interface.
Those are presentation fixtures, not a new live GUI-gesture test. Evidence:
`artifacts/cold-start-delivery-proof.json`. This closes the specified user-document
cold-start cases; fresh system installation, login/reboot, physical recovery,
visibility-edit callback gaps and full distribution remain open.

## Earlier visibility investigation and rejected candidate, 21 October

The installed `ca8b00…ccf6ba` pair was checked with two actual input-only AUHAL
clients: one on the edited device, one on an unrelated upstream device. An owned
marker source writes only to a virtual endpoint. Eight authenticated pass-through
visibility changes produce 14 forward callback gaps and 44 zero samples on the
edited input. Its five-second baseline and fresh-reader checks pass; the unrelated
28-second recording has zero zeroed samples or timestamp errors. Both devices'
live latency stays at 1,088 render frames and their mapping generations remain
unchanged. Thus this reproduction does not show latency or mapping churn.

Apple's AudioServerPlugIn contract permits the host to stop/restart device IO for
stream-layout approval. Loopback documents that disabling Pass-Thru removes output
availability. Those facts justify retaining host approval and the required output
visibility behavior; they do not prove that every observed silence is unavoidable
or establish a measured Loopback reference for these changes.

A bounded producer-retention candidate reproduced and fixed a real synthetic
idle/reset case. Release and sanitizer native suites passed, including commit,
rollback, timeout, idle-source and fresh-format isolation checks. However, the
actual candidate run still had 13 forward gaps and 1,736 zero samples. Unrelated
audio and fresh-reader recovery stayed clean. The candidate was rejected and
rolled back; its three source/test edits were restored from verified pre-experiment
copies. The installed apps again match `ca8b00…ccf6ba` exactly, original saved state
and audio settings are restored, and restored Release tests pass. No driver,
broker, global Core Audio process or physical buffer was changed.

The original input-only diagnostic has a fixed one-second warmup rather than the coordinated
capture barrier already used by the newer multichannel diagnostic. It does not
timestamp each zero span. Increased zeros prevent candidate acceptance but cannot
by themselves establish that the hold caused them: startup overlap, host padding
and transport misses need separate observations. This limitation motivated the
synchronized private diagnostic below. Keep the callback-gap gate open. Evidence:
`artifacts/visibility-rollback-proof.json`, with the rejected signed candidate,
source snapshot, native results and before/after counterexample retained.

The new input-only `stdio-v1` diagnostic is implemented and tested in a private
signed Engine, using its existing microphone grant. It waits for preparation and
the controller's explicit measurement start, checks every measured sample without
a warmup exemption, and holds shared periods until both readers finish. Fixed
storage captures timestamped zero-span fragments and callback gaps, including
render flags and invalid/overflow states; no audio samples are saved. Release and
sanitizer checks pass all five selected groups, including parser/protocol bounds
and marker regressions. Two backend-disabled portable groups also pass.

The synchronized baseline on the unchanged installed routing Engine has a clean
five-second no-edit capture, clean unrelated 28-second capture and clean fresh
five-second capture. The edited 28-second capture still fails: all 16 forward
callback gaps are inside the eight visibility edit intervals, with maximum host
gap about 290.989 ms. A single 16-frame stereo zero span occurs on the first
resumed callback during the first hide operation, about 2.299 seconds after
measurement began. That edit's driver counter grows by 15 missing capture frames;
later edit counter deltas are zero. Both mappings and 1,088-frame scene latencies
stay unchanged. The zero span's render flags are zero. These facts locate the
baseline failure but do not establish an exact 15-plus-1 causal decomposition or
the reason for the previous candidate's larger aggregate count.

The private mode is not installed over `ca8b00…ccf6ba`. Installed app bytes,
driver/broker, Core Audio/defaults, permissions, login choice, original files,
512-frame hardware buffers and canonical session are verified unchanged; the
desktop is reopened. Evidence: `artifacts/input-trace-delivery-proof.json` and
`artifacts/coordinated-visibility-analysis.json`. A fair private re-evaluation can
now use the synchronized window; native edited-input continuity and the full
Loopback objective remain incomplete.

## Earlier installed update — saved-document recovery, 20 October

The installed Engine now has direct acceptance for a damaged primary with a
valid backup at startup, damage to both files while routing, automatic recovery
after repairing the primary, and an unsupported newer primary while routing.
The complete two-device virtual fixture, controls, sources and connections are
retained in every observed document. Startup fallback leaves both original files
untouched; while both files are unreadable, edits are rejected rather than
silently replacing saved intent. A newer schema does not fall back to an older
backup. Valid file repair restores normal status automatically.

An existing 12-second virtual recording remains continuous across running-file
errors and repairs, with zero zeroed samples, timestamp errors or render
failures. This checks a virtual path; it does not establish startup continuity,
physical recovery or a new microphone latency result. The later cold-start
acceptance above uses the current installed pair.

The desktop now warns when routing was restored from a potentially older
backup, suppresses that notice when cached state is disconnected or a later
file error occurs, and clears it after primary-file recovery. Cold-start error
copy no longer claims a working graph exists. The interface-only update is
installed without sudo and retains the exact running Engine, driver, broker,
login helper and audio defaults. Normal/sanitized WebKit fixtures each pass
eight checks and 57 interface unit tests pass. Original empty routing, backups,
settings and 512-frame hardware buffers are restored; the desktop is reopened.
Evidence: `artifacts/document-recovery-delivery-proof.json`, interface package
manifest `0e1a767424e3dae6ae07baa3a4bcbf0cff1f24243ae0f7e093c11679b68a3a96`.
The Engine/interface hashes in this section are historical; the current pair is
identified at the top. Full product acceptance, physical dropouts and reliable 3–5 ms total
recording remain unresolved.

## Earlier installed update — setup and graph publication, 20 October

The signed desktop and Engine pair is installed as the console account without
sudo. Setup diagnostics distinguish missing/invalid Engine, routing driver and
service files from a present driver that has not connected. Checks run outside
meter polling and use the existing SingZ UI components. This is inspection and
recovery guidance; native initial installation remains open.

Portable graph publication now tracks readers per immutable graph version.
Overlapping readers of newer graphs no longer prevent unrelated old versions
from being reclaimed and stall subsequent graph edits. Actually held old
versions still enforce bounded backpressure; reclamation stays off audio
callbacks. Deterministic overlap, concurrent readers and ThreadSanitizer pass.

An installed 12-second, 64-frame virtual recording remains continuous across
20 graph replacements, with no zero samples, timestamp errors, source misses
or new publication deadline misses in the checked intervals. Its marker delay
is 6.25 ms on a virtual-only path, not physical microphone latency. Normal and
sanitized native WebKit fixtures each pass 84 checks; 54 interface unit tests
pass. The full sanitized 72-case conversion matrix passes with a longer test
timeout, which is functional evidence rather than a realtime performance claim.

Evidence: `artifacts/setup-publication-delivery-proof.json`, installed manifest
`c92067b240371210b34821276a715b84c7fbfdd87c9b3f3f0b7f8c2da52fe7ee`.
Driver, broker, original Core Audio process, audio defaults, saved files, login
choice and original 512-frame hardware buffers are preserved. Original Quad
Cortex missing audio and reliable 3–5 ms total physical recording remain open.
The interface-only delivery and Engine identities below are historical.

The active objective is full Loopback-style functionality on macOS, using
SingZ's zcore/zdsp and design system, with a portable core for later Windows and
Linux backends. The initial stereo feasibility milestone is evidence to build
on, not the definition of completion. Implemented code and installed behavior
require separate proof.

## Immediate delivery priority — 20 October

The latest installed change is an **interface-only upstream recovery fix**.
Virtual-source cards previously reported availability when a physical or app
input feeding their source device was disconnected. They now follow connected
signal edges across dependencies, show a partial-input warning and navigate to
the device containing the fault. Healthy channels retain live meters; quiet
samples, disconnected upstream monitors and disabled/unwired inputs do not
create a false dependency warning. Recovery clears the warning automatically.
Traversal is bounded across shared dependencies and defensive against cycles.

All 48 interface unit checks pass, and normal/sanitized native WebKit fixtures
verify warning display, deep source navigation, recovery and unchanged intent.
The updated projection also correctly classifies before/absent/restored
snapshots from the real native device-removal test above. The installed signed
UI matches that tested bundle. The update uses the console account without
sudo and preserves Engine PID 3404, its exact bundle, driver/broker, login helper,
saved routing, microphone grant and 512-frame hardware settings. Evidence:
`artifacts/upstream-feedback-delivery-proof.json`. The earlier full-pair GUI
identity in `default-timing-delivery-proof.json` is now historical; its exact
Engine remains installed and its timing measurements retain their stated scope.

The normal signed desktop/Engine update is now **installed and verified**.
Verified application timing is enabled by default, with conservative history
for unknown formats and the active-recording timing guard described below.
The exact installed canonical session has no opt-in flags. It passes real
microphone/app/dependency recordings at both 32- and 64-frame Zen/client
periods, with hardware-adjusted microphone window medians of 12.571–12.573 ms
and 12.916–12.918 ms. All three steady source/driver miss deltas are zero;
callback, timestamp, silent-block and app marker checks pass. Both physical
routes use linear phase, with 448-frame app and 512-frame aligned scene history.
The original 512-frame hardware preference, empty document/backups and undo
state are restored after testing, and the normal desktop is reopened.

The update runs as UID 501 without sudo. Installed bundle hashes/signatures,
unchanged driver, unchanged Core Audio process/default devices, preserved
broker/login choice and canonical background Engine are verified. A separate
two-owned-app test keeps one recording continuous through two peer relaunches
and edits with a bounded low-priority CPU worker launched. The untouched
provider never retries during observation, and lifetime provider/driver miss
and publication deadline deltas are zero. Explicit global diagnostic budgets
retain their old behavior; explicit app timing can still be disabled.
Evidence: `artifacts/default-timing-delivery-proof.json`. This closes default
promotion and ordinary app delivery for this tested configuration. Browser/
calling/DAW compatibility, wider load and hardware/monitor recovery, fresh
installation/uninstall/multi-user/distribution and the full requirements below
remain open. Reliable 3–5 ms total latency is not established.

### Installed Quad Cortex meters and connected-graph recovery

The installed desktop now has direct acceptance evidence for the reported
eight-channel Quad Cortex case. Adding that physical source creates no wires
and no pass-through source. The actual desktop's preview demand, without a
diagnostic preview lease, produces fresh valid eight-channel native meters.
Channels 1–4 carry the observed low-level signal; 5–8 are silent. Unwired
outputs remain silent. This verifies telemetry and activation, not a pixel
comparison or proof of a signal on all eight physical inputs.

A separate installed test wires Quad Cortex and Zen inputs into a four-device
graph including pass-through and a virtual dependency. After terminating only
the verified LoopZ Engine process, normal supervision and desktop reconnection
restore the saved document and both physical providers in 0.732 seconds.
Existing virtual playback/recording clients retain device/stream identifiers
and clock origin without reopening. Their marker path has no timestamp errors;
audio is silent for 520 ms during the forced fault and becomes continuous again
after recovery. Physical channels are checked by their restored meter arrays;
continuous waveform verification applies to the virtual marker path.

The first installed Quad Cortex observation nevertheless records an unexpected
512-frame history miss after warm-up. Later installed meter checks and private
30-second capture checks do not reproduce it. Its cause remains unresolved; the
passing repetitions do not close that defect. No queue or clock acceptance
limit was widened. Quad Cortex remains at its original 512-frame buffer, no
physical outputs are opened, and saved routing/settings, defaults, installed
apps and Core Audio are preserved. Evidence:
`artifacts/quad-cortex-recovery-proof.json`. Physical USB removal/reconnection,
sleep/wake, physical-monitor recovery and broader client compatibility remain
separate gates.

Cause-specific read instrumentation now distinguishes missing clock coverage,
resampling support, offline reads and expired or unpublished
sample storage. It is opt-in and uninstalled. Portable/native regressions and a
fresh 30-second Quad Cortex capture pass without steady missing frames, with
exact signed probe/source snapshots retained in
`artifacts/input-history-diagnostics-proof.json`. This improves diagnosis of the
remaining gap; it does not establish its cause or close continuity acceptance.

### Native input and monitor device replacement

The workspace runtime now passes real macOS device-inventory loss and return,
using a process-private aggregate over Quad Cortex and a continuously active
Zen peer. The fixture removes only its owned aggregate, waits until its UID
and source are absent, then recreates the same UID with a different native
object ID. Native inventory notifications trigger ordinary recovery; the saved
four-device graph is unchanged. The failed branch and its dependency become
silent, while the peer signal remains present at the mixed output. Input-only
restoration takes 196 ms in the observed run.

Two further runs include real native output backends for both monitors. All
monitor definitions are muted at zero gain, with a final test boundary that
clears every output sample regardless of renderer success. The target input
and monitor recover in 360 and 354 ms. The peer input and monitor never reopen;
its callback count continues, and missing-frame, graph, timestamp, discontinuity
and xrun counters do not grow. The final probe observes 487,936 native output
frames with zero nonzero output samples even before the final silence guard.
Release and sanitizer regressions also verify that this guard clears nonzero
and nonfinite output from a failed renderer and rejects format changes.

Evidence: `artifacts/native-device-recovery-proof.json`. These tests use real
capture/monitor owners, native device removal/recreation and the four-device
DSP graph, with an unpublished virtual-driver link. They preserve the installed
apps, session, routing, defaults, original 48 kHz/512-frame hardware settings
and Core Audio. They do not close physical USB/power-loss, sleep/wake, audible
monitor recovery or installed end-to-end hotplug acceptance. The separate
512-frame input history miss remains unresolved.

The private production Engine now exposes cause-specific physical history
diagnostics through both CLI status and authenticated XPC. A ninety-second
steady run on two published devices captures Quad Cortex and Zen at their
unchanged 48 kHz/512-frame settings. Each provider has 4,330,496 requested
history frames after warm-up, with zero source/history misses, failed sample
lookups, reacquisitions or renderer deadline misses during that interval.
Joined delivery data has zero source-counter discontinuities, rejected timing
observations or unsupported clock intervals. The run uses editor-preview demand;
no public recording client or physical monitor is opened and no latency is
measured. Release, sanitizer, portable-core and native runtime regressions pass.
Evidence: `artifacts/full-source-diagnostics-proof.json`.

The first collection's test cleanup used an invalid command, and a subsequent
repeat was stopped when its patch failure was detected. Both are retained as
failed runs. Only their owned fixtures were removed, with original empty
document/backup bytes recovered against earlier hashes. The corrected run
passes all cleanup checks and restores the canonical job, empty history, actual
installed app/driver hashes, permissions, defaults and hardware settings.
Diagnostics remain opt-in and private; no clock/queue policy was relaxed.
The retained installed 512-frame history miss remains unexplained despite the
new passing observations, and the original full acceptance gates remain open.

### Private fast automatic mixed recording acceptance

The current signed diagnostic Engine passes a fresh 60-second simultaneous
solo microphone, microphone/application mix and downstream app-dependency
recording at each authorized Zen 32/64-frame period. Automatic app timing now
works alongside per-scene causal/linear input selection in this faster
32-frame graph/publication-lead profile. Hardware-adjusted solo window medians
are 7.970–7.978 / 8.978–8.979 ms, and mixed medians are 9.919–9.928 /
10.912–10.914 ms. The shared physical source is acquired once; the scene
budgets are 288/384 frames at 32 and 320/416 at 64, with verified 352-frame
app history. Every microphone client receives 2,880,000 samples. Approximately
56-second steady intervals have zero source/driver/history-reason/lookup miss
deltas, no reacquisitions and no new publication deadline misses. Dependent
app signal and callback/timestamp checks pass. Isolated zero-valued mixed
samples are retained as signal observations rather than misclassified gaps.

Evidence: `artifacts/adaptive-fast-mixed-proof.json`, bound to the exact signed
private Engine, both recordings, portable clock tests and reverified installed
state. No physical outputs, raw audio files or new permissions are used.
The normal apps and canonical session are restored; document, settings and
backups match their originals, hardware returns to 512 and defaults/Core Audio
remain unchanged. This profile remains uninstalled and is not 3–5 ms total
recording acceptance. Native apps beyond the owned generator, fault/edit/load
continuity, wideband clock-transition quality and the earlier Quad Cortex miss
remain open.

### Distribution signatures and an accepted notary archive

Release staging now has an explicit `--distribution` path with secure Apple
timestamps, a Release-build requirement and no reuse of development signing
envelopes. Every architecture is checked for fixed publisher identity,
Hardened Runtime and exact role entitlements. A separate signed desktop/driver
candidate passes actual validation. Its metadata-free archive contains exactly
the 17 reviewed files (962,993 bytes), with complete archive and payload hashes.
Signing, submission-state, archive-admission, update, reuse and signing-isolation
tests pass: 68 checks. Evidence:
`artifacts/distribution-candidate-proof.json`.

After the original automatic approval rejection, the user explicitly authorized
the exact 962,993-byte archive. Apple accepted submission
`0127c7fc-23f3-4c43-81c0-e6e40ffd1528`, bound to archive SHA-256
`314d00ac0aae3bac0a4a7b3d36f7206cc5be9dcfacade81d84829c944328fcad`.
The log reports no issues and includes the app, login-helper and driver tickets.
Separate copies of both apps and the driver pass stapling, ticket validation
and strict signature checks. Both app copies pass Gatekeeper as Notarized
Developer ID. Execute-type assessment of the driver says its code is valid but
it is not an app (exit 3); driver installation acceptance is not inferred.
Evidence: `artifacts/notary-release-vlqkl64d/acceptance-proof.json` and its bound
Apple log and receipt. The original archive and package are preserved.

This accepted candidate predates the current installed capacity fixes; it was
not installed over them. Its stapled copies have changed inventories and cannot
use the original installer manifests. Current-build release validation,
first-run GUI checks, full initial-install packaging/activation and fresh-machine
offline distribution remain open. The historical installed-driver
`Unnotarized Developer ID` result does not explain the fresh-slot timeout.
Installed apps, driver, canonical session, saved state, defaults and hardware
settings are unchanged by this notarization work.

### Independent input/output clock filter acceptance

The optional minimum-phase input now has portable regressions for all 25
8/44.1/48/96/192 kHz source/output pairs, nine independent drift combinations
through the full +/-0.5% bounds, alternating one-sample timestamp corrections,
fractional positions and 32/64-frame reads. Each build verifies 2,700 reads
(129,600 output frames), zero missing frames/failed sample lookups and no
instrumented append/read heap events. A separate 15,606-response steady-drift
stopband sweep stays below -91.401 dB. Release, address/undefined sanitizers and
the backend-disabled local portable build each pass the filter, input-timing
and mixed-phase groups. Exact sources, binaries and logs are bound in
`artifacts/minimum-phase-independent-clocks-proof.json`.

This adds synthetic clock/signal coverage without changing installed code or
enabling the experimental filter by default. Abrupt output-clock cutoff
transitions, the full local-correction aliasing envelope, wide realtime/load
and lifecycle acceptance remain open. It neither fixes the retained Quad
Cortex miss nor proves a 3-5 ms total virtual-recording path.

The following paragraphs retain the preceding private-candidate milestones.

The private automatic app-timing policy now holds a recording mix's origin
until it is idle, avoiding the reproduced cold-start latency shrink that caused
19 silent blocks and a timestamp error. Verified private tap timing survives
matching idle/mute/relaunch reopenings, while changed formats remain
conservative. Portable/native acknowledgement, retained timing and unsupported
timing recovery regressions pass. Lifetime provider missing-read counters now
span scene changes instead of masking discontinuities when counters reset.

A stricter test finds 49 driver misses with no microphone-provider loss and a
1.704 ms render operation, so the renderer now uses Apple's recommended audio
workgroup for its independent clock and exposes real publication-deadline
telemetry. The exact private signed candidate passes 40 seconds of continuous
microphone recording through original-playback mute and app exit/relaunch, then
a complete idle close/reopen. Microphone/driver miss deltas and observed
publication deadline misses are zero; normal linear-profile delivery remains
about 12.59 ms with 512-frame history. App markers pass in both playback mute
states and after recovery. A separately reproduced waveform-matcher alias is
fixed without reducing its acceptance threshold, and the fresh microphone
correlations exceed 0.9998. Earlier failed samples/reports are retained.
See `artifacts/application-timing-continuity-proof.json` and the latest section
in `docs/latency-comparison.md`. These bounded private checks supersede the
earlier open affected-mix lifecycle gate for the tested configuration. The
exact final candidate also repeats cold activation and passes a minute at a
64-frame hardware/client period, with zero steady source/driver miss deltas and
12.91–12.92 ms microphone-in-mix window medians. Both microphone clients receive
2,880,000 frames and the 61 app marker windows pass. Broader app/load and physical
recovery acceptance, default promotion and ordinary installed delivery remain pending;
the policy is still opt-in and the installed apps are unchanged.

An opt-in automatic app-timing policy now replaces the fixed app history only
after every admitted private tap has verified rate/period/latency and callback
delivery. Application budgets are 352 frames in the fast renderer and 448 in
the normal linear-phase renderer. Real minute-long fast-profile microphone/app/
dependency checks pass at 32 / 64 frames with zero steady source/driver miss
deltas, reducing hardware-adjusted microphone-in-mix medians from roughly
23.25 / 23.57 ms to 9.92 / 10.91 ms. The final signed candidate also passes
the normal renderer at 12.58 ms and app marker delay at 11 ms (1 ms resolution);
its fast profile reports an 8 ms app marker delay. Marker and microphone
delivery metrics are distinct. An unrelated microphone client remains continuous
for 20 seconds across app activation. Unknown/partial timing keeps conservative
history; unsupported timing reacquires only that app instead of poisoning graph
replanning, and source-retention bounds are checked. Focused Release, sanitizer,
portable and native acknowledgement/recovery regressions pass. Exact signed
CLI and installed-payload/session/default/buffer restoration checks pass.
See `artifacts/application-timing-proof.json`. The policy is off by default and
uninstalled. Affected-mix cold activation, live original-mute/relaunch and
broader real-app/load compatibility remain before default promotion; the first
candidate's minute evidence and final candidate's marker/activation evidence are
tracked separately. This supersedes the earlier fixed 1024-frame app bottleneck
for the tested opt-in configurations, not the full product gates below.

Minute-long mixed acceptance now passes on the same private phase-policy Engine
at 32 and 64 frames. Solo/mixed microphone clients each receive 2,880,000 samples,
the app dependency passes 61 stereo sine windows per period, and all three
source/driver miss deltas are zero over roughly 56-second steady intervals.
Graph and transport generations remain stable; both providers acquire once.
The mixed linear recordings contain 1 / 9 isolated zero values, with no silent
blocks or reported missing frames; zeros alone must not be equated with loss.
Hardware-adjusted solo medians remain about 7.98 / 8.99 ms; the app-aligned mix
is about 23.25 / 23.57 ms. These use reported hardware delay, not analog
calibration. Expanded mixed-reader phase tests cover all 25 source/render pairs
from 8–192 kHz at sampled frequencies/positions and pass Release, sanitizers and
backend-disabled builds. The initial long harness field-name error is retained;
corrected minute runs and all cleanup pass. See
`artifacts/mixed-phase-sustained-proof.json`. The Engine remains private and
uninstalled. Minimum-phase clock/cutoff quality, sustained external load, live
hardware beyond 48 kHz, physical recovery, ordinary app delivery and the full
compatibility/release gates below remain open.

The next private Engine now preserves filter phase in mixed-source graphs.
Control-only DAG analysis keeps causal conversion for independent solo physical
routes and selects linear-phase reads for physical inputs contributing to mixes
of independent capture identities, including upstream virtual dependencies.
One shared capture can provide both immutable reader types without reopening or
mutating the old prepared scene. Automatic budgets match the selected filter;
retained timing preserves the largest verified budget per physical identity.
Correlated direct/dependency regressions reduce a reproduced 0.371215 residual
below 1e-6, and callback heap/reconnect and native policy tests pass in Release,
with sanitizers and with the backend disabled where applicable. Real six-second
simultaneous microphone/app/dependency acceptance verifies the selected filters,
one acquisition per provider and zero source/driver misses and recording errors.
The final signed candidate is uninstalled; all original routing, session, undo,
settings, buffers and installed payloads are reverified. See
`artifacts/mixed-input-phase-proof.json`. Analog/event-time alignment, broader
rates/clocks, sustained mixed/load and physical recovery/default promotion remain
open. This replaces the earlier all-physical-minimum-phase mixed behavior.

Recording measurements in this roadmap are driver-timestamp-to-client software
delivery times unless explicitly marked hardware-adjusted. The Zen's currently
reported input-device plus selected-stream hardware latency adds 74 samples,
or 1.542 ms at 48 kHz. Thus the private 4.45 / 4.77 ms results correspond to
approximately 5.99 / 6.32 ms hardware-adjusted estimates, not a calibrated analog
measurement. The total 3–5 ms target is still open. See the measurement correction
at the top of `docs/latency-comparison.md`; raw historical evidence is retained.

Newer evidence supersedes the fixed-budget safety inference below: a rerun of
the manual 192-frame profile at 64 frames records 99 source misses and is
rejected. The corrected probe detects nonzero reference-AU latency and withholds
hardware-adjusted results until its disabled output bus's default 44.1 kHz
client format is normalized to hardware. Fresh checks verify zero reference-AU
latency and 7.99 / 8.99 ms hardware-adjusted estimates for automatic 32/64-frame
profiles. An analog calibration is still absent. Automatic causal-input
timing now retains device/scheduling reserves and passes a real simultaneous
microphone/app/dependency check: 288-frame solo input, 1024-frame mixed and
dependent devices, zero new source/driver misses, seven clean stereo app windows.
Separate minute-long microphone recordings at both periods have zero source or
driver misses, zero post-warmup zero samples and continuous timestamps. The
signed candidate is private and uninstalled. See the automatic-policy and
measurement-correction sections of `docs/latency-comparison.md`.

A new comparison with the normalized reference probe estimates hardware-adjusted
delivery at 7.99 / 8.99 ms for automatic private LoopZ and 19.12 / 19.44 ms for
Loopback 2.4.10 at 32 / 64-frame recording periods. Both six-second recording
checks have no zero samples or callback/timestamp errors. Loopback's internal
input period remains unknown/unmatched; no analog calibration is claimed. The
same owned fixture and exact original preferences, defaults and buffer settings
are restored. See `artifacts/normalized-reference-loopback-comparison-proof.json`.

Physical-input timing now has a separate private diagnostic override, retaining
the app and pass-through policies. Native regressions prove 193-frame public
latency metadata for a 192-frame microphone-only route and 1025 frames for app
mixes and downstream dependencies. Both runtime/CLI groups pass in Release and
under address/undefined sanitizers. Actual simultaneous private IO then passes:
microphone-only recording at 4.454–4.456 ms, microphone in an app mix at
21.781–21.783 ms, and seven clean stereo app/dependency sine windows. The three
steady source/driver miss deltas are zero; shared physical/app providers each
acquire once. The six-second check uses an authenticated bounded preview lease
with no desktop clicks or physical outputs. The initial no-demand harness wait
is retained as a failed rehearsal, and the installed app, empty graph/undo and
512-frame settings are restored. See `artifacts/physical-input-budget-proof.json`.
This private signed Engine is not installed. Automatic policy, correlated-source
phase alignment, broader rates/clocks and sustained mixed acceptance stay open.

The authorized driver update has passed and its actual installed payload is
verified against manifest
`bf2ce02f4956035a0bcbaa8ae0415c98460d2d7e921774f7f6ddc277fe105ff1`.
The recovery/Settings app pair then installed without sudo, manifest
`2d0c9b014a44e4c1111892bf0f40336b209b3bff7380ee0c6b6431f7a09e133c`.
Normal routing, microphone access, account ownership, default devices, broker,
disabled login startup and Core Audio identity are preserved. Updated installed
recording checks pass at approximately 11.04 / 11.38 ms for 32 / 64 frames.
See `artifacts/post-driver-delivery-proof.json`.

The private minimum-phase Engine now passes **60 seconds at each 32/64-frame
hardware/client period** using 32-frame rendering, 32-frame publication lead and
a 192-sample source budget. Window median microphone-to-recording delivery is
**4.445–4.447 / 4.773–4.775 ms**, respectively; the largest window p95 is
4.744 / 5.369 ms. Both recording clients have continuous timestamps, zero
post-warmup zero/silent samples and zero source/driver miss deltas. Waveform
checks span seconds 1, 20, 40 and 58. The longer 64-frame-lead / 64-hardware-frame
profile fails source availability and remains rejected. All fixtures, test undo,
saved settings and the canonical installed session are restored afterward.
This is bounded private microphone-only acceptance. Generic automatic timing,
mixed-source phase alignment, wider clocks/rates, long-session/physical recovery
and a fresh physical Loopback comparison still precede default promotion. The
installed default remains conservative; these figures do not certify the full
product gates below.

A fresh passive physical-input comparison now measures Loopback 2.4.10 at
17.543–17.563 ms / 17.893–17.894 ms for 32 / 64-frame recording clients, versus
installed LoopZ at 11.031–11.038 / 11.383–11.385 ms and the private profile above.
All use the same Zen microphone input, 48 kHz and signed recording probe.
Loopback is measured as configured; its internal capture period is not matched
or known. Its six-second clients report no silent/zero samples or timestamp/
callback failures; private backend counters are unavailable. The existing owned
Loopback fixture and exact original preference bytes are restored, both devices
remain disabled, and defaults/Core Audio/512-frame settings are verified.
See `artifacts/physical-loopback-comparison-proof.json`. Longer and internally
matched compatibility comparisons remain open.

Adaptive physical-input timing is installed. With the existing 64-frame graph,
microphone-only virtual recording now measures about 11.05 ms at 32-frame
hardware/client buffers and 11.37 ms at 64, versus the earlier 21.71/22.04 ms.
The signed candidate passes a 60-second recording observation with no new source
or driver misses and retains 3.875 ms physical monitoring. Real app + microphone
+ virtual-dependency acceptance preserves application alignment and signal.
New mixes start empty; pass-through and source wiring are explicit choices.
The final interface-only update preserves the identical Engine and needs no
administrator access. See `artifacts/input-timing-delivery-proof.json` and
`docs/latency-comparison.md` for scope, cleanup and historical rejected profiles.

Next audio work is a converter/deadline change for 3–5 ms virtual recording and a
fresh matched Loopback comparison. The private 7.04 ms profile remains an
experiment; lower-budget profiles that lose source or driver samples remain
rejected. Physical disconnect/reconnect, everyday application compatibility and
release/distribution gates remain open. Do not broaden updater work in place of
these audio and everyday-app acceptance gates.

Installed application-source relaunch/isolation now passes a bounded real-audio
check. Two independently identified virtual-output generator apps feed separate
process taps; the first also feeds a downstream virtual device. The first app
exits and relaunches twice while its saved gain/mute controls are edited. Its
existing source and dependency recover automatically, with verified stereo
signal after each return. One recording client on the second app stays open for
40 seconds across the complete sequence: zero silent/zero samples, callback
failures, timestamp jumps/errors, bad waveform windows or driver-capture misses.
The Engine epoch and transport generation remain unchanged during the test.
The test graph, backup entries and undo history are removed afterward; hardware
buffer 512, defaults, existing permissions, installed payloads and Core Audio
are reverified. See `artifacts/installed-app-relaunch-proof.json`.
This certifies these two owned app identities and live gain/mute edits; real
browser/helper-process capture, broader topology changes, physical unplug/wake
and long-session compatibility remain open. Source miss counters are scoped to
prepared scenes and must not be subtracted across graph replacement; the
continuously open audio checker and unchanged transport counters span the edits.

A separate pass-through transition defect is now reproduced and fixed in the
installed signed driver. An approved visibility-only
prefix previously returned silence while another device awaited configuration
approval. It now keeps capture usable only when the same mapping generation,
input format and timing remain committed; fresh storage still stays silent until
the complete transaction commits. Rollback-wait capture and fresh-mapping
containment regressions pass both driver variants in Release and under address/
undefined-behavior sanitizers. The signed package and account-side installation
preflight are ready; see `artifacts/visibility-input-prefix-proof.json`.
Administrator installation now passes with verified payloads and hardware
continuity; full same-device transition acceptance remains open.
This fixes extra LoopZ muting; it does not prove elimination of the separate
Core Audio stream-reconfiguration callback pause. That continuity gate stays open.

Settings now has a separately signed interface candidate that preserves the
selected hardware-buffer UID when a device disconnects or the system default
changes. An unavailable device remains named in the picker, its saved preference
stays visible, and Apply is disabled. Reconnection restores that device's actual
value without submitting its old draft; selecting another device is explicit.
The original silent-substitution failure is reproduced, all 45 interface unit
tests and 23 native WebKit checks pass, and the signed candidate's resources
exactly match the tested interface. See `artifacts/buffer-disconnect-proof.json`.
The combined recovery/Settings pair is now installed after the driver update.
The earlier Settings-only candidate could update the interface
without replacing the Engine; the combined candidate below also updates Engine
recovery. Neither account-owned app update needs administrator access. Actual
physical unplug acceptance remains open.

The installed signed app pair combines that Settings fix with corrected recovery
deadline accounting. Source and monitor owners formerly timed every acquisition
against the start of the whole control poll. Regressions reproduce a nine-second
first open falsely expiring a healthy two-second later open, and prove that a
genuinely expired provider can retry without reopening healthy peers. Retry and
completion timestamps now refresh from the monotonic control clock for each
provider; monitor first-callback grace starts at admission. The three source,
monitor and Engine-runtime groups pass in Release and under address/undefined
sanitizers, and the source group passes with the macOS backend disabled locally.
The signed pair retains the existing microphone grant and exactly the interface
resources covered by the 23 WebKit checks. Account-owned update preflight passes.
See `artifacts/recovery-clock-proof.json` and `artifacts/post-driver-app-update.json`.
This pair supersedes the earlier Settings-only candidate and its ordinary app
update now passes without administrator access. Bounded installed microphone
recording acceptance passes; physical recovery acceptance remains open. Backend calls remain
synchronous: this fixes false timeout accounting, not cancellation of a hung
native call or parallel source acquisition.

An experimental portable minimum-phase converter now removes future-sample
support in isolated DSP tests. It uses real-cepstrum factorization of the existing
Kaiser-windowed sinc design, with a control-only radix-2 FFT following SingZ's
legacy implementation. The immutable causal table is optional; default readers
still use the existing linear-phase path. Minimum-phase reads always filter,
including integer-aligned positions, so clock phase crossing does not alternate
between differently phased paths. Native tests cover sampled pass/stop bands at
six rate pairs, 1/2/8/64-channel reads, fixed corrected-clock integer crossings,
causal source availability, missing past support and strided output guards.
Default resampling, causal public-output conversion and source recovery remain
green. This prototype is not selected for normal production captures, installed,
or included in the installed stable recovery/Settings pair. Optional portable history
integration now pins the filter shape to the nominal source format, fixing an
extra phase jump when the measured source-clock slope changes. Clock-step and
timestamp-correction regressions have no missing frames or callback heap events;
sampled rejection exceeds 90 dB across ±0.5% source-clock drift with a fixed output
clock. All five affected groups pass in Release, under address/undefined
sanitizers and with the macOS backend disabled locally. Output-clock cutoff
transitions, alias rejection across large local corrections, mixed-source phase
alignment, wider realtime processing limits and generic automatic 3–5 ms timing
remain open. Bounded private IO acceptance is recorded above. Physical-input history now supports explicit diagnostic selection
through `CaptureSourceOptions` and the Engine's
`--experimental-input-filter minimum-phase` flag. The CLI requires an explicit
source-delay budget, reports the selected filter and preserves the default
linear-phase path and app-tap readers. Capture-free native construction, CLI,
Engine-runtime and DSP checks pass in Release and under address/undefined
sanitizers. A workspace-only signed Engine preserves the microphone grant and
passes the exact signed CLI checks. Live microphone-only IO now passes as recorded
above; mixed-source acceptance remains open. See `artifacts/minimum-phase-native-proof.json`,
`artifacts/minimum-phase-prototype-proof.json` and
`artifacts/minimum-phase-clock-proof.json`; mathematical feasibility is not
an end-to-end latency measurement.

The actual history reader's offline processing cost is now measured across 48
cases and 48,000 reads. At 16 input channels, 32 frames and 48 kHz, minimum-phase
reads take about 46 µs median and 69 µs at the 99th percentile on this M2 Max,
within the 667 µs block period in that bounded run. One 64-channel corrected
44.1 kHz case has one over-period elapsed read; this ordinary-thread benchmark
does not certify real-time whole-graph capacity, concurrent callback contention
or zero-loss live IO. No installed code or native audio changes. See
`artifacts/input-history-benchmark-proof.json`.

The user-app migration is reverified against the current installed pair: every
payload hash and recursive UID-501 ownership matches, microphone authorization
remains granted, the saved empty document is restored, and the Engine confirms
the saved 512-frame buffer is applied. No further administrator step is needed
for ordinary app-only updates.

Both production user updater paths now use the durable file journal. Commit is
recorded only after app/runtime/continuity validation. Explicit
`manage_app_update.py --recover` restores uncommitted original app inodes or
retains committed new apps, restores current saved intent rather than a journal
copy, leaves temporarily disconnected hardware pending, and preserves Engine
operation for interface-only recovery. Pending user recovery blocks other app
updates and backend maintenance, with a check inside the maintenance boundary.

Installed SIGKILL acceptance now passes: the updater died between moving the
original GUI app and promoting its replacement. Recovery restored the signed
original apps and full three-device graph in 2.18 seconds; the same public clients,
device/stream IDs and clock origins survived. There was 1.615 seconds of deliberate
audio silence, with clean pre/post signal and no timestamp errors. The recovered
mic route remains 3.875 ms at 32 frames. All test routes, their backup entries and
undo history are removed; 512 frames is restored. The normal journal-enabled GUI
update also passes without an Engine restart. Private file and runtime fixtures
cover all moves, partial cleanup, current user intent, failures and fixed-process
admission. See `artifacts/user-app-recovery-delivery-proof.json`.

Automatic same-login updater recovery now passes installed acceptance. Each
update arms an independent temporary account job with a frozen, hashed module
closure. App promotion waits for its live acknowledgement. The full three-device
graph recovered after actual updater SIGKILL with no recovery command; existing
public clients, device/stream IDs and clock origins survived. Measured silence
was 2.041 seconds, and the complete receipt/job/workspace cleanup was observed
4.030 seconds after the updater exited. The mic monitor remains 3.875 ms at 32
frames. The guard stopped cooperatively and the job, readiness record, permit
and workspace were removed. Normal login startup remains off.

The ordinary interface updater also passes with that owner armed: 7.005 seconds
with zero silent samples or timestamp errors throughout the update and unchanged
Engine bytes/inode/PID. Final cleanup restores the empty mix, its recovery backup,
test undo/redo history and the saved 512-frame setting. Private fixtures cover
different login/boot identities, startup-off file-only repair, user-started
sessions, enabled startup policy, signature cache invalidation, foreign workspace
content and live guard acknowledgement. See
`artifacts/automatic-app-recovery-delivery-proof.json`.

Later-login policy is implemented and fixture-tested, but actual logout/login and
reboot/power-loss acceptance remain open. Signed distribution packaging, a
fresh-machine interpreter strategy, native GUI admission during early setup and
native enabled-login replacement remain open. Do not extend this updater result
to the still-pending physical disconnect, real-app compatibility, virtual-recording
latency or release/distribution gates below.
The earlier `artifacts/app-update-journal-proof.json` records the foundation
checkpoint before production integration.

Active user-owned updates now pass installed acceptance. The managed updater
prepares files while routing continues, pauses the canonical Engine only for
promotion and restores the full saved graph, buffer intent and previously ready
sources/monitors. The three-device test retained existing public clients and
their device/stream identities and clock origins. Engine replacement caused
1.26 seconds of silence, with acknowledgement after 1.43 seconds and 5.33 seconds
total command time. Preparation, partial promotion, signature and restoration
failure fixtures cover rollback and protected-intent/process checks.

Fresh signed GUI packages retain byte-identical Engine/login components only
when all architectures' full signed code hashes and every non-executable resource
match. Actual fresh-package interface replacement preserves the Engine's bytes,
inode and PID, with zero silent samples or timestamp errors before, throughout
the 4.624-second update interval and afterward. The earlier undifferentiated
whole-capture failure is retained as failed evidence. The current installed pair
at that checkpoint was manifest
`e7aeffbb2c647b7e6c9c1ee9545ecb69e813a86cb801788b44058fc1aaa52871`;
the current installed manifest is recorded in the immediate delivery proof above.
All test devices, their recovery-backup entries and test undo history are removed;
512 frames is restored. See `artifacts/active-app-update-delivery-proof.json`.
Explicit and automatic same-login abrupt-updater recovery are now verified above.
Actual power-loss, native enabled-login replacement and physical unplug/wake
acceptance remain open. Full scope below remains unchanged.

Saved buffer recovery was first installed in pair manifest
`2162f9036f430cf24015711027c97cff4629a13e26486d5a5f53d10d24105cba`.
The user updater again passed without sudo; all 11 installed hashes and UID-501
ownership match. The Engine owns atomic versioned buffer preferences, a valid
backup, bounded metadata/retry and saved-versus-running state. Native fixtures
cover restart, disconnect, different UID, discovery/write/storage failure,
changed limits, corrupt/newer data, symlinks and size bounds.

A real installed Engine crash restored the complete three-device test document
and its 32-frame hardware IO setting in 588 ms. The same stereo public clients
and their device/stream identities and clock origins survived. The deliberate
crash caused 464 ms of silence; stable pre/post signal checks have zero silent
frames or timestamp errors. The physical mic route measures 3.875 ms before and
after restoration. All temporary devices are removed and 512 frames restored
and saved. This does not certify physical unplug/wake or all disconnect classes.
See `artifacts/device-settings-delivery-proof.json`. Active paired Engine updates
are now verified above; enabled-login and the remaining full product table stay open.

The ownership migration has passed, followed by an ordinary user app update
without sudo. The currently installed pair is manifest
`e7aeffbb2c647b7e6c9c1ee9545ecb69e813a86cb801788b44058fc1aaa52871`;
all 11 payload hashes and recursive UID-501 ownership match. Its Settings buffer
operation now runs in the Engine process through authenticated XPC. The first
installed acceptance exposed that setting the UI/diagnostic process to 32 did
not change the Engine's 512-frame IO period. Those runs measured 23.875 ms and
are explicitly failed buffer-application evidence, not 32/64-frame results.
The fixed build reports negotiated monitor periods and reads Settings inventory
from the Engine. Actual installed waveform/continuity acceptance now passes:
3.875 ms at 32 frames and 5.208 ms at 64 frames, six agreeing waveform windows
per buffer, with zero reported source/monitor missing frames, hardware clock
errors, discontinuities or host xruns during each 60-second observation. Both
temporary devices are removed; the original empty document, 512-frame Engine
setting, defaults and Core Audio are preserved.
Session buffer persistence now passes the installed restart check above. See
`artifacts/engine-buffer-installed-latency-result.json`,
`artifacts/engine-buffer-user-update-verified.json` and
`artifacts/zen-buffer-process-scope.json`.

The same-device duplex monitor path is now integrated into EngineRuntime /
MonitorOutputs, including full device and monitor DSP, staged edits, permission
fallback and recovery. Private signed waveform measurements give 3.875 ms at a
32-frame Zen buffer and 5.208 ms at 64 frames; six windows agree at each period.
This measures mic input 3 to internal loopback 9 through monitor output 1, not
the virtual recording path or speaker/air latency. The migration now installs
this implementation; installed waveform acceptance now passes as recorded above.

The capture investigation found an additional portable clock-mapping defect:
small host-time projection corrections were treated as unsupported rates even
when the source sample counter proved continuous. Fixing bounded closed-interval
conversion removes those artificial silent/missing blocks. Fresh one-minute
checks at both 32 and 64 frames pass with the existing AUHAL capture adapter:
zero missing source/monitor frames, source-counter gaps, monitor clock errors,
xruns, skipped graph frames or graph/sink failures. A lower-level HAL capture
comparison is retained only in the diagnostic tool; it is not needed as a
production adapter replacement. Delivery observations now end before teardown,
so closing another stream cannot contaminate running-path acceptance.

Keep application, pass-through, virtual and independent-device routing on the
existing independent-clock path. Never interpret zero added monitor queue as
zero total latency. Queue-only reductions previously failed; do not install
those profiles as defaults. Capture histories remain at their existing defaults
in these successful checks. Evidence is summarized in
`artifacts/input-clock-investigation-result.json`; restored hardware settings,
audio defaults and the original Core Audio process are independently verified in
`artifacts/input-clock-final-cleanup.json`. Both tested buffers restore to 512.
The final signed pair now includes this Engine integration, the clock fix and
the Settings control. Its installed payload and recursive UID-501 ownership are
now verified: manifest
`327280a782afafaba6210dd1601c3850bf4652ffaea972517da1b97ae3c629c4`, with metadata in
`artifacts/input-clock-signed-stage.json`. The earlier Settings-only package
does not contain the final changes.

The requested REAPER-style hardware-buffer control is implemented and tested in
Settings. Native backend and WebKit checks pass, including actual 512→128→512
Zen changes. The earlier Settings-only pair was staged with manifest
`0bb97010c1c361156b5d6ad24bf855e5f922a9e1925747791114a3a16e6df69c`;
it is superseded by the installed integrated pair above. Desktop updates now
have a separate user-owned path. The user authorized tearing down the old mix; it
was removed via authenticated debug commands. Shared hardware-buffer experiments
are now explicitly authorized, and all completed tests restore 512 afterward.

The app-only delivery milestone is now installed: manifest
`0aba9781d268759435cc5f2309a59f237db2ce16fccd3ade824ba787e5164e02`.
The root receipt and all 11 installed app payload hashes are verified in
`artifacts/editor-preview-deployment-verified.json`. The user-created mix and
its explicit wiring were preserved through this update. The interface has
Inputs, Outputs and Monitors columns; newly added sources request no automatic
connections. Authenticated editor preview activates enabled source meters
without a public audio client, and connection shutdown releases that demand.
Portable unwired-source metering, native acquisition/stop and two-editor
connection lifetime checks pass. Actual installed Quad Cortex capture has fresh
eight-channel meters, one acquisition and no public IO client, recorded in
`artifacts/editor-preview-installed-live.json`.

The ownership split is now implemented in the prepared installer and user
updater: desktop/Engine apps become console-user-owned, while the driver,
broker, system configuration and jobs stay root-owned. Ordinary idle app
updates use no sudo. Mixed-owner uninstall/recovery and coordinated backend
updates preserve this split. A managed migration step verifies the actual empty
graph and canonical session, requests authorization before interruption, and
restores the session and desktop afterwards. Publisher pins, entitlements and
installed locations are unchanged. Native system-Python and temporary-file
regressions pass; installed read-only preflight passes. The one-time migration
has passed: all 11 installed files match, both app trees belong to UID 501,
the authenticated Engine session is restored, and driver/broker/defaults/login
and the Core Audio process are preserved. The subsequent ordinary user update
also passes installed waveform acceptance. Evidence and limitations are in
`artifacts/user-application-update-proof.json`. The updated portable core also
builds with the macOS backend disabled and passes all 26 checks; this preserves
the backend boundary but does not certify a Windows/Linux audio backend.
Interface-only updates now have a separate user path that allows active saved
mixes and public clients, replaces only `LoopZ.app`, and never opens audio or
stops the Engine. Registered login state can be retained when its helper/job are
unchanged. Filesystem/policy and system-Python tests pass; the actual installed
Engine guard passes read-only observation. Installed paired and fresh-package
interface updates now pass active-audio checks. Installed automatic abrupt-updater
recovery now passes above. Enabled-login and actual reboot acceptance remain open.
See `artifacts/editor-only-update-proof.json`; no update may silently tear down
a user's mix.

Ship the already signed and tested desktop/Engine pair before expanding the
combined updater. The app-only deployment preflight passed for manifest
`5c4e5602c2e61f9c7ecf47f0b0bb7397ad2f9f9d2807650336795cadf47f18dc`.
This pair works with the installed broker and driver; the four-component
updater is not a prerequisite for delivering it.

1. Install the pair through the existing guarded app-only installer, then verify
   the installed files and authoritative installation result.
2. Open the installed SingZ interface and exercise first-device creation,
   source selection, wiring, meters and routing. Preserve existing user routes;
   use temporary fixtures for destructive recovery checks.
3. Fix failures in that everyday workflow before broadening infrastructure.
   Then close the known visibility-change callback gaps and perform a matched
   Loopback latency comparison.
4. Resume combined updater, wider lifecycle and distribution acceptance after
   this working-app milestone. The full functionality objective above remains
   unchanged and incomplete until its remaining acceptance gates pass.

The candidate has passed private signed ten-device/five-client recovery and
monitor-retention checks. This is not yet proof of canonical installed UI
acceptance, uninterrupted visibility edits, or a consistent 3–5 ms profile.
Its exact bundled interface also passes 79 native WebKit/editor-bridge checks
in `artifacts/working-app-ui-acceptance.json`, including first creation and
saved-runtime reconstruction. That fixture uses fake driver transport and
injected permission metadata; it does not certify installed or physical IO.

## Required behavior

| Area | Required result | Current evidence / remaining work |
| --- | --- | --- |
| Virtual devices | Multiple independent devices: create, rename, delete, enable, persistent identifiers and settings, publication to normal clients | Native lifecycle/transaction fixtures cover 32 devices. The current installed pair publishes and withdraws 32 devices in the six-client mixed recording test, preserving the original document and defaults. Full independent 32-wide recording stability and broader lifecycle remain open. |
| Channels | Stereo default, output pairs through 64 channels, multichannel hardware/pass-through, stereo app capture | Portable graph and native conversion fixtures cover 1–64 channels. Current private and installed 2/8/64-channel mixed recordings pass for thirty strict seconds; independent 32-by-64-channel repeats remain failed. Independent-device crosstalk, sustained load and installed high-rate acceptance remain open. |
| Application sources | Multiple apps, launch/exit and helper-process tracking, source controls, original-playback mute during capture | Shared owners deduplicate capture, combine original-mute policy and isolate failures; process/route/format/stall recovery implemented. Candidate shared-source/mute checks passed. Installed two-app exit/relaunch, live gain/mute edits and downstream virtual recovery now pass while one unrelated recording client remains continuous for 40 seconds; installed owned-app virtual fan-out/mute policy now passes two simultaneous recordings and ten phases with one provider acquisition; new fixed-tap runtime also passes a source relaunch restoring both open clients while a distinct 28-second peer recording remains continuous; real browser/helper-process, physical original playback and broad compatibility acceptance remain open |
| Device sources | Multiple physical inputs and other virtual devices, channel selection, reconnection, independent clocks | Shared input owners and 1–64-channel histories preserve partial profiles and retry UID/default/rate/channel/authorization/stall loss. Installed Quad Cortex eight-channel preview meters and wired Quad/Zen restoration after an Engine fault pass; sources start without implicit wires. One installed 512-frame history miss remains unresolved. Physical unplug/wake and long-session acceptance remain open. |
| Wiring | Fan-out, summing, editable source/output/monitor connections, automatic channel matching and mono duplication | Portable graph and full-scene DAG render wiring; staged DSP/native commit and failure isolation tested offline; desktop wiring commands are connected to the authenticated Engine; installed gain/mute and rate edits passed short checks; unrelated-route continuity pending |
| Pass-through | Multichannel input from apps selecting LoopZ as output; source controls; enabled state governs public output availability | Actual 2/8/64-channel IO and chaining passed short checks; stereo output visibility, disable/re-enable and removal/restoration pass while an unrelated client streams; the installed update preserves input clock origin during visibility edits, but eight edits still produce 16 forward callback gaps and continuous same-device input is not accepted |
| Device controls | Independent enable, mute, volume and meters, preserving saved source settings | Full scenes render controls and source/device meters; atomic meter reads and staged native/DSP integration tested offline; desktop controls use validated commands and runtime meters; installed gain/mute and live meters now observed; sustained integration pending |
| Monitoring | Multiple monitors with independent mapping/controls and hardware-clock conversion; monitor use keeps capture active | Independent SingZ output owners and staged programs pass recovery/concurrent-edit fixtures; the newer clock/conversion candidate passes two real always-muted 48/44.1 kHz outputs and all 16 Antelope channels in separate 12-second checks with zero missing frames; existing rates/periods/defaults unchanged; the signed private retention candidate now passes full ten-device mixed recovery and monitor continuity after client teardown; deployed retention, real unplug/wake and audible/latency checks remain pending |
| Live edits | Volume/mute and topology updates without interrupting unrelated routes; explicit format transitions and feedback prevention | Prepared graphs, cycle rejection, atomic resizing and staged transport commit exist; authenticated, revision-safe editor commands now reach the runtime with real DSP meter changes and unrelated mapping retention in isolated tests; desktop controls now save through the actual Engine; installed gain/mute and rate edits passed short checks; unrelated-route continuity pending |
| Persistence | Versioned document, atomic saves, corruption recovery, stable device identities and restart restoration | Installed four-device wired recovery after Engine fault, running-file rejection/repair and valid-backup restoration pass on their recorded builds. Current pair also passes both-invalid/newer-schema cold starts, overwrite refusal, automatic same-process file repair, explicit first creation and restoration of that first definition/UID after restart. Login/reboot, multi-user and wider persistence lifecycle remain open. |
| Background operation | Editor exit preserves routing; configured routes restore after login; explicit login-start choice | Session supervision and explicit SMAppService login controls are implemented in a signed candidate. The real Engine automatically recovered four saved devices after a forced crash and restored 2/8/64-channel IO; canonical desktop/Engine file deployment and capture-free activation now pass; login registration/logout/login, active updates and deployed audio recovery remain pending |
| Interface | SingZ components, device list, routing editor, real meters/pickers, themes, keyboard and accessibility | Native desktop and React interface import actual @singz/ui 1.9.5 components and complete kit styles; authenticated Engine connection, explicit first creation, real meters and inventory pickers are implemented; signed GUI first creation and input discovery observed, installed live-audio and full UI acceptance pending |
| Permissions | Clear setup/denial/revocation states, capture-free discovery, stable signed identity across updates | Two certificate-signed updates installed; app and microphone/monitor checks pass after the latest update without further prompts; denial/revocation UI and broader lifecycle remain pending |
| Installation | Fresh install, updates and uninstall without Core Audio restart, default changes or unrelated audio loss | Existing-slot full routing migration passed actual activation/withdrawal without global restart; fresh-slot activation and production lifecycle unresolved |
| Recovery/security | Authenticated local control, helper/driver recovery, device isolation, failure silence, no recursive self-routing | Feasibility bridge verified; protocol-2 per-device heartbeat silence, mapping isolation/retirement and bounded XPC codec tested locally; portable and stereo engine recovery pass; desired-document controller retries rejected/rolled-back/quarantined native transactions and survives reconstruction in local tests; authenticated role-bound XPC and three-process replacement tests pass; full-scene failure silence and staged audio publication tested with native XPC/fake-host IO; signed candidate now passes real automatic Engine recovery and full four-device restoration with installed-driver IO; driver-host loss, physical recovery and deployed supervision acceptance remain pending |
| Performance | Recorded low-delay settings, two-hour mixed-source stability, bounded memory/clocks and measured wide-graph limits | Earlier two-hour mixed-source and physical checks remain historical evidence for their recorded builds. The current installed pair passes six simultaneous thirty-second mixed recordings through 32 devices without missing samples or added renderer misses. The aligned 64-channel input adapter benchmark improves about 1.71 times; this is not an end-to-end latency result. Full 32-wide repeats, current-build sustained/physical acceptance, minimum live queues and matched Loopback comparison remain open. |
| Compatibility | Concurrent recording/calling apps, DAW multichannel IO, source/rate/device changes, promised OS/CPU matrix | Two probe clients verified; real-app and compatibility matrix pending |
| Distribution | Reproducible signed/notarized packages, update policy, rollback and full uninstall | Secure timestamped release archive accepted by Apple; app and driver ticket copies validate, both app copies pass Gatekeeper. This older candidate does not cover the latest installed build. Complete distribution container, initial activation, fresh-machine offline install, actual removal/reinstall, multi-user and abrupt-loss lifecycle remain open; no remote configured |

The portable graph currently bounds a route to 32 sources and 64 channels.
These are preparation bounds, not an installed concurrency or latency guarantee.
The final supported concurrency must be measured and clearly exposed.

## Implementation order

1. Preserve the completed two-hour app/microphone/monitor baseline and its
   exact hashes (`artifacts/two-hour-mixed-source-proof.json`). Repeat sustained
   installed validation after the new recovery/service/audio runtime is deployed;
   this pass does not certify those uninstalled changes.
2. Connect the preview driver and its tested format/rollback coordinator to an
   authenticated broker and persistent user agent. The desired-document
   configuration controller now reaches native transaction/recovery entry points
   through its authenticated XPC adapter, with isolated three-process tests.
   Full-scene DSP preparation/publication and native IO now pass isolated tests.
   Persistent scheduling and source/monitor ownership now run in the document
   engine mode and authenticated fixtures. Client rate-request forwarding now
   reaches saved intent and native approval in isolated tests, including replay
   protection across helper restarts. Mixed-path origin compensation and retained
   virtual histories now align the audio path. Derived input latency metadata now
   travels through immutable descriptors and host-approved timing transitions,
   including rollback and notifications in isolated tests. Validate installed
   root-domain activation and real client timing next. Native object
   publication, IO and transactions have fake-host tests; actual installed
   publication and a 48/44.1 kHz transition now passed short capture-free checks.
   The two SDK/source preflight gaps are now corrected: the driver declares its
   broker Mach service and verifies a sealed bundle trust pin instead of using
   the helper's external-file loader. A separate Developer ID routing-package
   stager validates all component identities and the real native pin loader.
   Evidence is `artifacts/bundle-trust-proof.json`. The signed payload is staged;
   a guarded initial install/activation/rollback transaction is prepared with
   capture-free graph publication, native inventory checks, output continuity and
   failure injection. `artifacts/routing-install-proof.json` records the prepared
   payload and preflight. The first authorized Apple-host activation timed out
   waiting for the new driver; files and defaults were restored and the hardware
   continuity guard passed. `artifacts/routing-activation-failure.json` records
   that actual failed gate. The separately authorized existing-slot migration subsequently passed actual
   Apple-host activation and withdrawal, preserved defaults/Core Audio identity
   and passed physical-output continuity. Evidence is
   `artifacts/routing-migration-installed-proof.json`.
   Active-graph updates, changed-driver reload and background/login
   integration remain unfinished acceptance gates.
3. Multiple capture/monitor owners are implemented. Verify actual installed channels,
   device lifecycle and isolation before connecting an editor to those capabilities.
4. Add production background supervision/login choice and the SingZ desktop shell
   and routing editor. Portable atomic commands, optimistic revisions and durable
   saves now reach EngineRuntime through authenticated bounded XPC. Actual meter
   snapshots and separate-process reconnect/replacement fixtures are implemented;
   see `docs/editor-control.md`. The repository desktop now connects real inventory pickers and controls to
   these APIs, with explicit first creation and stale-meter handling. A signed
   GUI review created durable intent through the production identity pins.
   Installed UI live meters and editor-exit independence are now observed. The candidate now includes session supervision, actual OS login-state controls and setup feedback. Real automatic Engine crash recovery and restored multichannel audio passed; A separate guarded desktop-pair deployment transaction now has signed-file promotion/rollback rehearsal and read-only preflight evidence; administrator deployment, actual login acceptance and broad UI coverage remain open. The successful existing-slot migration and subsequent IO rehearsal are recorded
   in `artifacts/installed-routing-io-proof.json`. Its optimized candidate has
   not replaced the installed Engine or default runtime profile.
5. Exercise recovery, format/permission changes and real client applications;
   complete fresh installation, compatibility and distribution acceptance.

Independent implementation can proceed while macOS authorization is pending.
Offline tests cannot substitute for installed or physical-clock gates. The full
objective remains incomplete while required behavior or its evidence is missing.

The driver recovery candidate now separates shared-memory replacement from public
format changes. Rebinding an unchanged graph preserves device/stream IDs, client
registrations and clock origin/seed, and avoids a HAL configuration request.
Repeated 2/8/64-channel reconnects and mixed mapping/format commit and rejection
pass both bundle factories and address/undefined/thread sanitizers; evidence is
`artifacts/driver-handoff-proof.json`. The subsequent existing-slot driver update
installed this change. Continuous clients under the actual Apple driver host now
pass Engine crash recovery with a privately staged signed Engine.
An opt-in continuous-client diagnostic and guarded crash-test mode are now
implemented to close that acceptance gap, with portable recorder, native clock
and identity checks, bounded control protocol and independently attempted
cleanup. Offline and sanitizer evidence is in
`artifacts/continuous-client-proof.json`; actual virtual-route acceptance is now
recorded in `artifacts/continuous-recovery-installed-proof.json`.

Capture and monitor histories now have a portable bandlimited conversion
candidate with a prepared Kaiser-windowed sinc table and tiled multichannel
reads. Spectral, lifecycle, concurrency and processing-cost evidence is recorded
in `artifacts/sinc-history-proof.json`. The existing source/monitor queue sizes
are unchanged; required filter support is explicit and missing support remains
an underrun. Public output and stereo compatibility conversion, changing-clock
quality, installed low-delay profiles and broad real-time concurrency remain
acceptance work. The frozen administrator package predates this candidate.

The history converter is now shared through portable `AudioResampler` and also
handles audio entering virtual routes from public capture/playback mappings.
Native 2/8/64-channel spectral and underflow fixtures, strided-output bounds,
partial-plane containment, graph restoration, sanitizers and backend-disabled
build checks are recorded in `artifacts/public-input-proof.json`. Public output
now has a portable causal filter and coordinated latency metadata, with evidence
in `artifacts/causal-output-proof.json`. Equal-rate delay remains one frame;
44.1/48 kHz conversion needs about 2.35 ms of filter delay. First impulses,
partition independence, large timestamps, wide channels, reset containment and
retained output history through staged edits are checked offline. Minimum
off-rate queues and actual installed audio remain unverified; the pending signed
administrator package is unchanged.

Application capture in the full document runtime and legacy stereo capture now
share `AudioHistory` through the `CaptureBuffer` adapter. This removes the
remaining linear stereo input converter and duplicated clock/ring implementation.
Stop/restart invalidates cached generations, and native app/microphone teardown
uses SingZ callback admission before clearing history. Offline spectral,
independent-reader, restart, concurrency and sanitizer evidence is in
`artifacts/stereo-history-proof.json`. Actual tap/microphone stop/reopen and
minimum-queue validation still require the signed candidate; the pending desktop
package remains frozen.

Source clock estimation now uses a bounded recent window rather than a capture-
lifetime average, and measured drift at nominal 8/192 kHz no longer makes valid
source reads fail. A regression, six history/stereo clock-step fixtures, backward
timestamp recovery and sanitizer/portable checks are recorded in
`artifacts/changing-clock-proof.json`. Settled quality is measured separately
from transient error. Phase continuity through changing clocks, physical jitter
and live minimum queues remain open; this is not full ASRC acceptance.

Historical callback-clock mapping now addresses the earlier transition phase
error. A bounded stamped clock ring and precision-checked stable-span fitting
preserve past source intervals instead of applying the newest clock slope to
them. Dense step and per-callback varying-rate traces, old-snapshot/reset
concurrency, partial underflow and nominal/drifting processing-cost checks are
recorded in `artifacts/clock-phase-proof.json`. The intermediate wide-downsample
CPU regression was corrected. Physical timestamp jitter and live/deployed
minimum profiles remain open; the newer output-clock estimator is described
below. This is still a workspace candidate and the administrator package remains
frozen.


A portable output-clock estimator now supplies each monitor's actual read rate
from hardware timestamp progression. The native endpoint retains it through
wiring edits and resets it on stream loss, discontinuity or timestamp-quality
changes. Settled drift audio comparisons include 1 ns and approximately 42 ns
timestamp quantization, with rate reversal and lifecycle checks recorded in
`artifacts/output-clock-proof.json`. These are uninstalled synthetic checks;
physical-monitor and transient audio quality, minimum live queues and matched
Loopback measurements remain open. No configured audio delay was added.


The newer signed Release candidate now has actual silent-monitor and virtual
client evidence in `artifacts/output-clock-live-proof.json`. Both independent
48/44.1 kHz output clocks and all 16 physical Antelope output channels passed
12-second checks. Three simultaneous virtual clients (2/8/64 channels, including
an 8-channel chain) passed 30 seconds without silent holes or timing/render
errors, measuring 4.75/4.75/5.25 ms. A transient removal-time catalog observation
was fixed with bounded whole-snapshot retries and a separate cleanup rehearsal;
failed and passing reports are both retained. This does not prove a consistent
3–5 ms profile, live physical input/application capture under the new code,
continuous-client crash recovery, acoustic quality or a new matched Loopback
comparison. The candidate is not installed and the administrator package remains
unchanged.


Actual multichannel input capture now has signed-candidate evidence in
`artifacts/capture-live-proof.json`: Antelope 16 channels, Quad Cortex 8 channels
and a second Antelope stereo reader. A newly fixed policy transition retains
capture when required channel demand changes within its prepared layout. Stereo
remained connected for 20 seconds while both wide routes ran for 12 seconds,
with one Antelope acquisition, no missing-frame increase across growth/shrink,
and zero steady history/render errors. Muted public outputs were verified silent;
only levels and counters were saved. The 1024-frame source lookback is not a
minimum-latency result. Injected physical wiring/isolation, real unplug/permission
loss, multi-app capture, longer workloads and installed UI/recovery acceptance
remain open.


Application capture now has signed-candidate evidence in
`artifacts/app-live-proof.json`. Two actual Core Audio generator processes with
separate bundle identities feed isolated virtual sinks; one app is captured by
two shared routes. A shared binding's original-playback mute silences that app's
original sink while both captured copies continue; clearing the policy restores
original playback. Exiting/relaunching that app restores the same saved routes,
and an unrelated app delivers 50 seconds with zero silent samples, bad signal
windows or timestamp errors and no capture reopen after readiness.

The initial rehearsal exposed eager acquisition before an app had a running
output clock. New taps now wait for active output and a published route. A
fixture with a deliberate 1.5-second output-start delay restored in about
3.3–3.5 seconds; startup deferrals are preserved separately from steady checks.
The tested 1024-frame source lookback and 512-frame clients measured roughly
42–43 ms app-capture markers, not a 3–5 ms app profile or a new Loopback comparison.
Discrete multichannel app outputs now have the additional evidence below.
Surround end-to-end and multiple-output mixdown, browser/helper attribution, permission loss,
long sessions and canonical deployment remain open.

The 16-channel output rehearsal exposed a real mixdown failure: the normal
1024-frame source lookback yielded silence. Raising only the diagnostic run to
4096 made audio available at about 129 ms, but its level was one eighth of the
original. Neither increase nor gain compensation was adopted as a product fix.
Native capture now accepts bounded discrete float32 streams and folds channel
pairs at unity through the existing SingZ graph. macOS's synthesized all-unknown
layout is recognized as numbered channels; known surround layouts are excluded.

`artifacts/app-wide-proof.json` records the corrected signed candidate. Two
shared app captures from a 16-channel virtual sink preserve the original level
at the unchanged 1024-frame lookback, including original-mute and restore edits.
An unrelated app continues for 35 seconds with no detected silence or signal,
timestamp or queue errors and no capture reopen. Portable fixtures check every
discrete input through 64 channels without render-time allocation; native layout
fixtures reject truncated layouts and explicit surround assignments. Actual
live channel injection covered the first stereo pair of the 16-channel sink;
other pairs, surround, multiple outputs and a 3–5 ms app profile remain unproven.

The subsequent candidate adds standard speaker-layout capture without returning
to system mixdown. `AudioFormatGetProperty` provides the stereo coefficient
matrix during preparation; the existing SingZ graph applies it in capture.
Mono remains duplicated at unity, and reversed stereo labels disable the direct
copy fast path. Layout changes now invalidate the prepared channel plan and
enter source recovery. Real macOS format-service tests cover mono/stereo,
reordered 5.1 and 7.1 layouts, per-channel signal projection at 44.1/48 kHz,
malformed/truncated layouts and changed-plan detection. This is preparation/DSP
evidence; live surround playback, unsupported matrices and multiple-output
capture remain separate acceptance gates. Evidence and the ordinary live
multichannel regression are in `artifacts/app-speaker-layout-proof.json`.

The next document-runtime candidate replaces the single-tap application owner
with a bounded native capture group. It discovers every selected output UID and
stream, preserves device-wide channel numbering when slicing channel plans,
and combines independently clocked stereo histories at the requested host time.
Per-frame coverage counts overlapping gaps once and keeps other valid streams
audible. Group publication/reclamation uses the existing render-borrow mechanism;
no capture history is freed while a render borrower can still use it.

`artifacts/app-multiple-streams-proof.json` records two owned audio processes
with one app bundle identity playing into a 16-channel 48 kHz sink and a stereo
44.1 kHz sink. Both distinct frequency pairs reach two shared captures at their
original levels. Shared original-mute silences both original outputs while the
combined captures continue; clearing it restores playback. Exiting only the
secondary process restores the remaining output, and relaunching it restores
both components. An unrelated app streams for 60 seconds with zero silent
samples, bad signal windows, timestamp errors or capture reopen after readiness.
Portable tests cover exact gap unions, valid silence, stopped contributors,
44.1/48 kHz conversion and two concurrent independent readers without audio-thread
heap activity. Address/undefined/thread sanitizer checks are recorded separately.

This live scope is two processes of one bundle, one native stream per device.
One process opening multiple outputs, multiple native streams on one device,
aggregate output devices, dormant-output activation and broad browser/helper
attribution remain acceptance gates. Unsupported non-float/layout plans expose
retryable acquisition errors in this document owner; they no longer silently
fall back to the known-bad system mixdown. The legacy compatibility mode retains
its fallback. Current conservative app markers remain about 42–43 ms, and the
3–5 ms app profile and a new matched Loopback comparison remain unfinished.

## Single-process output recovery and public timing

`artifacts/public-pass-timing-proof.json` extends application testing to one
process with a prepared but dormant second output, followed by start, original
mute/unmute, close and reopen of that output without replacing the process.
The outputs have independent 16-channel/48 kHz and stereo/44.1 kHz formats;
both frequency pairs reach two shared captures at their original levels.
Unrelated app capture continues for 70 seconds without silent samples, bad
windows, timestamp/queue errors or capture reopen after readiness. These checks
cover one stream per output device; multiple streams on one device, aggregate
outputs, pause with an IO unit retained and different-bundle helper attribution
remain open.

The initial run failed because the unrelated app's original pass-through had
90 zero samples at the fixed 128-frame queue. That report is retained. A deeper
diagnostic queue and then automatic device-specific floors passed subsequent
rehearsals. Writer-side 32/64-frame reports did not establish smaller queues:
the Engine still observed the 512-frame fallback. The latest workspace code
adds read-only driver callback-period/session reporting and uses that evidence
when available. Isolated tests cover its native property, session reset,
transport replacement and acknowledged graph/latency updates. The new driver
is not installed, so actual Apple-host telemetry and lower-delay acceptance
remain required. Passing fallback markers are about 44 ms at 48 kHz and
52–53 ms at 44.1 kHz, with a 1 ms search resolution. This does not establish
the requested 3–5 ms app profile or a new comparison with Loopback.

## Canonical desktop deployment

`artifacts/desktop-installed-proof.json` now confirms the prepared desktop and
Engine pair in `/Applications`: all 11 manifest files match, with root ownership
and no group/world write access. The administrator's success report contains
actual capture-free virtual-device publication and withdrawal plus a passing
physical-output continuity guard. Driver/broker binaries, the Core Audio
process, audio defaults, user document and login choice were preserved.
The installed interface seal verifies, microphone authorization is retained,
and login startup remains available and disabled. This closes the prepared
pair's canonical file deployment gate. It does not install the newer workspace
capture/timing changes, certify audible desktop routing, enable login startup,
or complete active-update/uninstall and logout/login acceptance.

## Existing-driver update preparation and deployment

`artifacts/driver-update-proof.json` records a signed, optimized desktop/Engine
and existing-slot driver update, including the new callback-period property.
The read-only installer preflight passes against the installed pair and driver.
Nineteen updater fixtures cover complete payload/signature policy, strict required
and additive runtime capabilities, partial third-component promotion, rollback
ordering, unchanged audio identity, automatic host replacement, mixed old/new
mapped images and superseded PID generations. Existing desktop, migration and
installer regressions pass; the new output-only diagnostic's argument/virtual
guards also pass under address/undefined instrumentation.

A real capture-free publication rehearsal proves that the newer diagnostic
refuses the older installed driver before acquiring IO, then withdraws its device
and stops its owned Engine. Signing, the native bundle-pin loader and installed
file/default/Core Audio preservation are separately recorded. This establishes
prepared update/recovery behavior, not actual privileged deployment or successful
callback telemetry under Apple's driver host. Administrator authorization
was required for that prepared package. The broker, user document, defaults and
login choice are outside the update; active graph and enabled-login updates
remain unfinished.

The subsequent administrator transaction passed with manifest
`267c8ffca51696d9cacc942fde13bc921ea413669241df24772e7586ffa3f7a4`.
Independent inspection confirms all 15 protected installed files match. The
Apple-hosted driver now returns positive callback telemetry; the installer
observed a 512-frame client, 144384 rendered frames and no timestamp/render
errors. Only the verified LoopZ host changed (94187 to 12892); Core Audio stayed
PID 29793 with its original start time. The physical-output guard passed for
8.18 seconds, and catalog withdrawal, defaults and existing microphone grant
were independently rechecked. The staging proof above is retained unchanged;
the newer workspace features below are not included in this deployed package.

`artifacts/driver-update-installed-proof.json` now adds actual callback-session
and short virtual pass-through acceptance. Output-only clients correctly expose
512/32/64/128/64 frames in successive sessions. Eight-second stereo 48 kHz signal
checks pass at all four client sizes with both an explicit diagnostic queue and
automatic queue selection. Automatic marker delays are 4.25/6.25/11.25/44 ms for
32/64/128/512 frames. Checked spans exclude the first warm-up second and contain
no silent samples, timestamp errors or render failures. Both private graphs
withdraw cleanly with defaults, Core Audio and installed files preserved. This
closes the positive installed callback telemetry gate and adds short low-buffer
virtual-path evidence. App/microphone 3–5 ms, sustained minimum-buffer stability
and a new matched Loopback comparison remain open.

The post-deployment app sweep now distinguishes capture readiness from actual
signal availability. With a 64-frame source/reader and automatic public timing,
installed 128/256-frame source-history profiles fail with silence/missing audio;
512 and 1024 frames pass short checks at 13 and 25 ms. A signed workspace
candidate adds a bounded private aggregate request option (64 or 128 frames,
default unchanged). Its 64-frame request passes at a 384-frame source delay,
measuring 11 ms, while smaller profiles still fail. Source construction/option
guards, runtime/reordering regressions and address/undefined instrumentation
pass. Evidence and failures are retained in `artifacts/application-buffer-proof.json`.
No installed replacement or default promotion occurred. Measuring actual tap
delivery and newest available history is now the next latency step; a connected
source and smaller requested property are insufficient evidence of low latency.

The subsequent delivery diagnostic and smaller-period candidate are recorded in
`artifacts/capture-delivery-proof.json`. Portable observation collects timing
without callback allocation/logging; the native adapter supplies host times only
when explicitly enabled and prints summaries after callbacks join. History
publication failures have a separate counter. Scene scheduling/preparation and
monitor queues now support 32 frames, with cadence, mixed-path alignment,
recovery and monitor-queue regression checks. Eight native groups and seven
address/undefined groups pass. A fixture's old hard-coded 64-frame recovery call
was corrected to respect a 32-frame scene, preserving waveform tolerances and
adding explicit valid-block assertions; initial failures are retained.

An actual signed stereo 48 kHz app-capture candidate with 32-frame source,
capture, graph and reader, zero render lead and 160-frame source history passed
60 seconds at approximately 5 ms (1 ms marker resolution), with zero silent
samples, bad windows, timestamp/render failures or late/skipped render frames.
The smaller 128-frame history profile fails and is retained. Actual capture
callbacks are 32 frames; capture publication/processing observations remain
separate from a future deadline guarantee. The candidate is uninstalled and all
defaults/filters remain unchanged. Fresh matched Loopback, broad app/period/rate,
microphone/monitor, recovery and sustained mixed-source acceptance remain open.

## Application helper ownership

The workspace now includes a portable process-membership policy and a macOS
ownership collector. Exact identity, verified app-bundle containment and
responsible PID/start generation can select helpers; bundle-name prefixes and
global capture fallbacks cannot. Reused process identities trigger capture
recovery. Independent apps and LoopZ Engine processes are excluded from
responsibility-based expansion.

`artifacts/app-ownership-proof.json` records portable, runtime and sanitizer
checks plus real metadata-only Chrome/Zoom/Safari selection. Shared WebKit
ownership depends on an optional undocumented system SPI; its absence keeps
exact/contained matches usable but shared-helper coverage remains limited.
Generated audio, mute/relaunch continuity, actual Safari audio and the supported
OS matrix remain open. No live graph was started while the administrator driver
update was pending. These changes are not included in that frozen package.

An owned nested-app fixture additionally exposed and fixed `/var` versus
`/private/var` executable-path attribution. Its metadata-only parent/helper
lifecycle passes start, stop and relaunch with distinct helper generations and
an unchanged surviving helper. See `artifacts/app-family-proof.json` for current
source and test evidence. The fixture's virtual-only tone generation mode is
prepared for subsequent captured-signal checks; it is not audio acceptance.

## Native recovery notifications

The document Engine now listens for wake/session activation, app lifecycle,
audio process inventory and device/default changes. Events accelerate missing
endpoint retries without reopening healthy captures or monitors. Inventory
bursts are coalesced. Audio-service reset handling joins and releases stale
native resources, preserves desired routing, reconnects the authenticated
session and re-registers listeners. It never restarts Core Audio itself.

`artifacts/recovery-events-proof.json` records listener-registration retry,
reattachment, reset-during-delivery/reattachment, callback teardown, unchanged
healthy streams and full desired-graph restoration fixtures. Actual native
listener setup passes on this host under optimized and instrumented builds;
actual reset delivery, sleep/wake, unplug and continuous-client restoration
remain acceptance gates. This change is not in the frozen pending driver update.

## Editing saved source and monitor selections

Source and monitor options now expose the existing native retargeting commands,
with 1–64-channel input/monitor layouts, preserved IDs/controls/surviving wires,
unavailable saved choices and virtual feedback prevention. Channel reductions
preview removed wires, revalidate an open confirmation after layout changes and
reset the draft when cancelled. Monitor mapping labels now state signal direction.

`artifacts/endpoint-editor-proof.json` records UI unit tests and production-bundle
browser checks through the real native command decoder/reducer, including atomic
input changes, fractional wire gain preservation, a concurrent layout edit,
disconnect, keyboard focus and both SingZ palettes at two window sizes. Native
XPC, persistence, WebKit and live routing are not exercised by this offline bridge.
The installed desktop and frozen pending package do not include these controls.

## Saved list ordering

Device, source and monitor options now expose saved order with accessible
movement controls. Full native ID permutations preserve settings and wires;
stale orders are rejected. Selection is pinned before a device move and persists
across interface reload, including a state reply arriving before the edit reply.

`artifacts/order-editor-proof.json` records browser/native-reducer acceptance,
eight persisted device/source runtime reorders and two-rate monitor reordering
without reopening injected endpoints or replacing transport regions. Optimized
and address-instrumented runtime checks retain strict sample continuity.
Thread-instrumented wall-clock checks initially underrun/reanchor; the retained
failures explain its separate ownership/eventual-audio profile. Deterministic
monitor bus continuity is checked under thread instrumentation. Production
queues remain unchanged; installed continuous-client and live timing acceptance
are still open. These controls are outside the frozen pending installer.

## Matched input-only comparison

`artifacts/matched-input-comparison-proof.json` now adds a repeated, matched
source-only input comparison. A signed virtual-only/input-only diagnostic reuses
the same native recorder and portable marker checker for both products, with
bounded guards, existing-permission checks, callback joining, cancellation and
period restoration. Six native and three address/undefined groups pass. Both
products use the same generator identity/binary per pair, stereo 48 kHz, 32-frame
source/readers, original mute, no pass-through and no physical monitor. Actual
32-frame generator callbacks are independently recorded in the repeat.

Two 20-second runs measure approximately 3/3 ms for the LoopZ candidate and
16/15 ms for installed Loopback 2.4.10/ARK 13.1.3 (1 ms marker resolution), with
zero checked signal/timestamp/callback failures. LoopZ's earlier duplex 5 ms
measurement is a different client path. The prepared Loopback device and original
generator were restored, temporary graphs/processes withdrew and installed
files/defaults/Core Audio stayed unchanged. This closes the fresh matched
comparison gate only for these recorded conditions; acoustic, other rate/app/
period, broad real-client and sustained mixed-source acceptance remain open.


## Continuous clients across Engine replacement

`artifacts/continuous-recovery-installed-proof.json` records four passing forced
Engine replacements against the installed Apple-hosted driver. Each uses a private
four-device graph and keeps the original 2-, 8- and 64-channel clients open,
including an 8-channel virtual-device chain. The replacement acknowledges the
complete canonical document and matching desired/active generations through the
authenticated editor service. A separate all-channel readiness interval and
three-second marker check then establish actual audio recovery.

Device/stream identities, clock origins and client stream generations survive;
healthy spans have no silent samples, timestamp/render failures or invalid
callbacks. Two settled-session runs measured about 296 and 312 ms of silence.
Early-job runs take longer under launchd's configured ten-second crash throttle.
These are recovery interruptions; the previous matched app-capture comparison
remains approximately 3 ms for LoopZ versus 15–16 ms for Loopback under its
recorded conditions.

The diagnostic now establishes clock identity after startup warm-up, balances
explicit timing-service ownership, separates deliberate shutdown counters from
active observation, and retains every client's result when one fails. Earlier
failed attempts remain in the proof. Four native and four address/undefined
groups, thirteen controller tests and the backend-disabled recorder check pass.

The installed driver, desktop/Engine files, broker, defaults and Core Audio
identity remain unchanged during these tests. The newer signed Engine and
read-only desktop status command are staged privately. This does not complete
physical/app-source/monitor graph restoration, driver-host loss, wake/unplug,
login acceptance or the broader compatibility and distribution requirements.

## Mixed-graph recovery and shared-client lifecycle

The mixed acceptance harness now builds ten private devices, two routes sharing
one marker application capture, built-in microphone and 16-channel input branches,
and two always-muted monitors at 48/44.1 kHz. It retains the original virtual and
app readers across the planned Engine fault. Authenticated snapshots verify
capture ownership, channel layouts, source histories, advancing monitor callbacks
and zero new missing frames during healthy intervals. Writer-only devices may
idle when they have no input consumer; physical monitor demand remains active
after the diagnostic readers stop.

The first actual run published all ten devices and opened both input/monitor
branches, then failed the continuous-client baseline before any crash. One client
lost its clock on an output shared by two writer processes and a third prepared,
idle client. Virtual-only inspections reproduce the stop, and cleanup/default/
profile/Core Audio checks pass. Direct-launch fixed/automatic latency controls
did not reproduce it, so automatic latency is not established as the cause.

The driver candidate now counts per-client IO references, releases a removed
client's remaining references without stopping its peers, and reports first-start/
last-stop running transitions to the HAL outside its state mutex. Reentrant host
queries, nested starts, peer removal, clock/session preservation and idle-client
registration have native regressions. Eight native and eight address/undefined
groups, both ThreadSanitizer driver factories, seven mixed-quality cases and
thirteen controller cases pass.

`artifacts/shared-client-driver-update-proof.json` records the signed candidate,
read-only deployment preflight, retained failures and exact source snapshots.
The installed driver is unchanged. Fresh administrator authentication is required
for the prepared guarded update. Whether the candidate resolves Apple's observed
shared-client stop, and whether the full mixed graph then survives an Engine
crash, remain unverified acceptance gates.

## WebKit interface acceptance

The interface now has an isolated macOS `WKWebView` acceptance runner using the
production custom-scheme resource handler and a fresh bundle containing actual
`@singz/ui` 1.9.5 modules. Seventeen checks pass for layout, styles, accessible
dialog naming, focus wrapping/restoration, DOM keyboard shortcuts, and appearance/
device selection after document reload. The native fixture also passes address
and undefined-behavior sanitizers; all fifteen interface unit tests pass.
`artifacts/webkit-interface/result.json` records source fingerprints and a WebKit
snapshot. The fixture never starts the production Engine or opens audio devices.

This establishes browser-engine coverage beyond Chromium. A second native WebKit
runner now exercises the actual desktop bridge, authenticated anonymous XPC and
document runtime. UI creation/rename/mute/duplicate/deletion, render continuity
after editor exit and reopening against a reconstructed runtime pass with a
private saved graph. Device identity and canonical intent survive while the
runtime epoch changes. Evidence is `artifacts/webkit-engine/result.json`.
The same integration passes address/undefined sanitizers and five related
native regression groups; `artifacts/webkit-engine-proof.json` ties the reports
to source fingerprints and unchanged installed files, defaults and Core Audio.
The injected session host and fake driver keep this independent of production
launchd jobs, public devices and physical audio. Installed UI/audio integration,
appearance preferences across application restarts, native keyboard delivery,
VoiceOver and broad real-client acceptance remain open. The shared-client driver
candidate is still awaiting administrator installation; the installed manifest
continues to be `267c8ffca51696d9cacc942fde13bc921ea413669241df24772e7586ffa3f7a4`.

## Source recovery feedback

Source/monitor feedback now uses structured presentation conditions rather than
native-error substring matching. Revoked microphone metadata overrides cached
capture success, partial inputs retain their wiring and offer source review,
and stale status cannot advertise healthy endpoints or offer an ineffective
retry. Settings gives separate guidance for denied, restricted, initial and
unknown access. Explicit recovery and access-review actions preserve saved
intent; no permission request occurs during reconnect or reload.

Twenty-three interface unit tests and native WebKit guidance checks use controlled
permission/disconnect metadata. Appearance/focus and real bridge/service/storage
regressions pass with the new interface. The workspace UI changes are not in the
frozen pending driver package. Actual macOS denial/revocation, deployed initial
consent and deployed mixed-graph recovery remain open gates.
Inactive-route and offline/revoked-input meters now clear instead of retaining
the last peak while other routes keep the global render clock active.
`artifacts/source-recovery-ui-proof.json` records 15 recovery-guidance, 17
appearance/focus and 10 real bridge/service/storage checks against the same
bundle, plus the 23 unit cases and unchanged installed-system checks.

## Explicit microphone permission setup

Settings now provides the first-time microphone action through the production
origin-checked bridge. A separate desktop permission adapter reuses installed
Engine signature/capability validation and invokes only its existing foreground
permission command. The permission queue leaves editor polling and graph
rendering independent. Pending state survives panel reopen, duplicate requests
are rejected, and closing the desktop cancels/reaps its owned helper operation.
No request is sent by automatic reconnect or document/runtime restoration.

Native child/codec regressions and the real-bridge WebKit tests use fake consent
responses and private graphs; they do not grant/revoke access or open OS privacy
dialogs. Address/undefined instrumentation covers the launcher and bridge flow.
Deployment and actual macOS consent behavior remain required. The pending signed
driver payload and installed system remain unchanged.
`artifacts/microphone-setup-proof.json` records 18 real-bridge checks in each
normal/instrumented build, launcher/session/signature regressions, matching UI
bundles and the actual read-only installed-Engine identity preflight. The
instrumented interaction failure and its test actionability correction are
retained. Foreground stderr diagnostics are excluded from JSON grant responses.

## Real Chrome output compatibility

An isolated instance of the installed Chrome 155.0.8059.85 now passes stereo
48 kHz pass-through with both 512- and 64-frame clients. A local Web Audio
worklet sends channel-specific tones only to an explicitly selected private
LoopZ sink. The input-only diagnostic verifies amplitude, channel isolation,
timestamps, callback failures and zero runs after warmup. It reports no marker
delay for this sine profile; callback receipt offsets and Chrome's latency
properties are not end-to-end latency measurements. The existing marker profile
retains its correlation and delay checks.

The browser fixture uses a temporary profile, opens no media input, and stores
only signal/timing counters. Its owned browser, controller, private routing job
and devices close cleanly, with audio defaults and Core Audio identity preserved.
Normal and address/undefined diagnostic regressions pass. Scope regressions
reject unrelated browser processes and any output outside the private sink.
Evidence is `artifacts/chrome-output-compatibility-proof.json`.

Application capture and fan-out remain unvalidated for Chrome: the native app
family includes helpers from the user's existing browser, so the process-scope
guard stopped that phase before opening capture. An isolated browser session or
explicit permission for that broader capture scope is required. Browser faults,
calling apps, DAW IO and other rates/formats remain separate acceptance gates.
The signed test Engine runs privately against the installed driver; it has not
replaced either installed app or the frozen driver-update candidate.

## Public device lifecycle and unrelated-route continuity

The signed private Engine now passes twelve saved-document edits against the
installed driver: rename, pass-through hide/restore, source/device mute, zero
gain, disable/re-enable, pass-through removal/restoration and device
deletion/restoration with its saved UID. A metadata-only observer verifies
actual Core Audio names, device identity, rates, stream IDs and channel counts.
Mute and zero gain retain the output; disabling/removing pass-through hides it,
and disabling/deleting a device withdraws it. Re-enable/restoration preserves
the saved UID and restores working stereo IO.

One unrelated 48 kHz stereo client remains open for 30 seconds across all twelve
edits. Its public device/stream identity remains unchanged; marker validation
reports zero silent samples, timestamp errors, render failures and nonfinite
samples after its stated one-second warmup. A separate five-second check passes
on the restored edited device. The graph, test job, defaults and Core Audio
identity return to their original state. Evidence is
`artifacts/device-lifecycle-live-proof.json`.

Playback visibility changes recreate the edited device's public input stream
ID on this host. No client was open on that endpoint during those transitions;
active edited-input recovery remains an explicit gate. The test uses atomic
private-document reloads, not desktop gestures, and does not certify unrelated
physical/app routes or long-session concurrency. Rate-request receipts may
advance during re-enable; the observer requires exact persisted intent apart
from that acknowledgement field. Initial controller/acceptance-assumption
failures and their successful cleanup remain in the evidence. Neither installed
payloads nor the frozen shared-client driver candidate changed.

## Active input during playback visibility changes

The stricter private-device check keeps one input-only AUHAL client open while
pass-through visibility changes eight times. Its signal comes from a separate
virtual source, so hiding playback does not intentionally mute the input. The
first installed-driver trial passed its five-second baseline and fresh-reader
check, but the 28-second client spanning the edits failed strict continuity:
15 timestamp discontinuities and 198 zero samples, despite good tone/marker
windows and no render failures. It resumed audio; this is not evidence of a
permanently disconnected graph or a seamless transition.

The driver also reset its input clock at host approval for a playback-only
change. A new native regression reproduced that avoidable reset. The workspace
correction inherits the latest input clock when rate, channel count and input
latency stay unchanged, both on forward approval and rollback. It still
requests host approval, retains transaction muting, and preserves genuine
client-start and input format/timing resets. Clock handoff now uses the existing
reader revision guard, including rollback to a Runtime retained by older IO
tables. Native checks cover eight toggles, rejected suffix/rollback, a client
restart while approval waits, and real channel/rate/latency changes.

`artifacts/active-input-visibility-proof.json` separates the installed failure
from the uninstalled correction and its native/instrumented tests. Core Audio
can still pause IO while changing stream topology. Neither the native clock
test nor a fresh reader proves that those pauses have been eliminated. The
original installed client case must be repeated with the corrected driver;
the already reviewed shared-client installer/package remains frozen.

The combined signed recovery package is now prepared with manifest
`06b03b0744aee6d309b065727d6a79f156b730421fb49bd60b60e0b8386a1981`.
It combines both driver corrections with the current desktop/Engine pair.
Its exact signed driver passes the private native suite, and package/runtime
preflights and four deployment/signing/rollback groups pass. Evidence and
36 source snapshots are recorded in
`artifacts/combined-recovery-update-proof.json`. The new administrator command
supersedes the earlier pending shared-client command; both earlier artifacts
and the reviewed installer implementation remain unchanged. Installation and
the same live acceptance cases are still required.

## Native keyboard navigation

Actual AppKit key delivery exposed a WebKit configuration gap: a trusted Tab
event moved focus from the Settings controls to the page body. The desktop now
uses shared WebKit preferences that enable Tab navigation through links and
form controls, while retaining its automatic-window-opening restriction.
Both isolated fixtures use those same production preferences.

A fixture-only, strictly bounded native-key handler sends events only through
its own NSWindow. It rejects unsupported keys/modifiers and has no system event
injection, production bridge or accessibility-permission path. The normal and
address/undefined runs each verify 18 native key events, including Tab/Shift-Tab
traversal and containment, Enter/Space activation, Escape/focus restoration,
Command-N/Command-comma, modal shortcut isolation and routing socket
selection/cancellation. WebKit marks the received events trusted; no routing
edits or system actions are sent. Appearance/recovery guidance, sealed resources
and the real bridge/private-runtime checks also pass with the preferences.

`artifacts/native-keyboard-proof.json` binds the failure, passing runs, source
fingerprints and unchanged signed/installed payloads. The correction is a
workspace desktop change and is **not** in the frozen combined recovery update.
Physical keyboard delivery, production menu shortcuts, launch focus, VoiceOver
and the full deployed keyboard workflow remain separate acceptance work.

## Routing history

Workspace Undo/Redo now covers device creation/deletion and ordinary routing
edits through one bounded portable journal and the authenticated native control
owner. SingZ controls, desktop shortcuts and native menu routing actions share
the existing revision and uncertain-save protections. Failed saves, branching,
retention limits, external intent changes, receipt-only updates and Engine
restart are checked. Private WebKit acceptance verifies stable device identity,
native menu actions, text-focus routing isolation and reconstruction with an
empty journal. Native and address/undefined runs, frontend and backend-disabled
core checks are bound in `artifacts/edit-history-proof.json`.

History is limited to 32 edits / 8 MiB of retained snapshot payload and the
current Engine session. Native typed-text undo/redo and the shared Edit menu's
routing key equivalents now pass private normal and address/undefined runs;
each graph shortcut commits once, and text shortcuts preserve routing history.
The shifted Redo key equivalent and bounded fixture requests are checked in
`artifacts/native-history-menu-proof.json`. Physical menu/keyboard delivery,
layout/locale coverage and live audio during traversal remain acceptance work. This feature and the
preceding native keyboard correction are not included in the frozen combined
recovery update; system installation and recovery gates remain pending.

## Guarded removal

The prototype's unconditional deletion script has been replaced by a read-only
default entry and an explicit reviewed-plan removal transaction. Fixed inventory
covers the current desktop pair, both driver slot names, modern/legacy brokers,
support tools and known reports, preserving saved routing and unknown files.
Descriptor-based inspection, private quarantines and atomic no-clobber rollback
avoid following substituted paths or overwriting concurrent replacement files.
Only a re-audited isolated Apple driver host can be stopped; there is no global
audio reset or automatic default/login change. Runtime withdrawal and output
continuity precede the irreversible purge boundary. Failures retain repair or
cleanup paths and distinguish partial committed cleanup from rollback.

Temporary-file failure tests, synthetic driver process/image rejection tests,
both local Python interpreters and a real read-only review of 19 installed
components are bound in `artifacts/uninstall-preparation-proof.json`. No installed
component has been removed. Actual uninstall/reinstall, driver unload/restore,
multi-user login registration, abrupt loss and prototype/damaged-installation
teardown remain required before full removal support can be certified. This
implementation does not change the signed recovery package awaiting deployment.

The next workspace increment adds a durable uninstall journal and file-recovery
backend. Its pre-move quarantine records and pre-purge decision distinguish
restoration from partial committed cleanup. Original snapshots and parent/inode
identity checks prevent overwriting recreated destinations or deleting foreign
quarantine data. Active records block the new entry points before installed
tool lookup, and uncertain commit writes preserve originals for recovery.
Both Python interpreters pass nine real private-child SIGKILL checkpoints plus
schema, collision, flush-failure, normal-rollback and commit-boundary tests;
`artifacts/uninstall-journal-proof.json` binds the evidence. Recovery reports
file disposition separately from runtime finalization and retains its record.
An administrator recovery coordinator and scoped diagnostic/workspace shutdown
are now implemented as described below. Native service/driver restoration and
power-loss/multi-user acceptance remain unverified. The frozen existing installers still need pending-journal admission
after their currently approved deployment; they remain unchanged now.

The `--recover` entry now follows the durable decision under the installer lock.
Pre-commit recovery uses a validated checker, current console/default/document
baseline, a fresh continuity guard and publisher checks around original file and
service restoration. Its driver observer cannot signal a host. Committed
cleanup needs no installed runtime and preserves recreated targets. Old owned
diagnostics stop through a private cookie and verified executable-path checks;
workspace cleanup precedes journal finalization in normal and recovery paths.
Coordinator failures, current-user changes, missing tools and repeated process
loss are rehearsed; three added SIGKILL checkpoints and an actual CPU-only native
child complement the earlier file-recovery fixtures. A read-only native factory/
signed-checker rehearsal also passes. `artifacts/uninstall-recovery-proof.json`
binds these checks. Full installed removal/recovery, physical continuity and the
remaining lifecycle/distribution gates are not certified by these tests.

The next removal increment reviews active GUI sessions using launchd's user
domains rather than the local account list. Each session's UID and audit/session
identity, three LoopZ job labels and per-user status-only login query are
recorded, then the session set is rediscovered. Unreadable/unknown launchd state,
loaded jobs, enabled/pending login registration and identity changes reject
admission. Console identity is read before and after each session scan, including
fast user switching that leaves both GUI domains present. A second check after
quarantine precedes driver signalling; the same session fence also guards the
result. Another account's status query requires
administrator context and uses the verified desktop's fixed status entry point.
No other user's jobs or registration are changed.

When the desktop is quarantined, its unavailable login status remains explicitly
unknown in snapshots. A previously verified off choice can admit only the same
account/session during removal. Recovery can accept a new session generation for
a recorded account, checks a fresh current-session baseline, and reads fresh
login status after restoration. Legacy journals supply no grant for additional
accounts. Private session/race tests and a real read-only single-user preflight
are bound in `artifacts/uninstall-sessions-proof.json`. Real multi-user/root
removal, logged-out users' registrations and power-loss recovery remain gates;
this does not certify their lifecycle or modify the pending signed package.

Workspace appearance now also follows macOS automatically. Fresh preferences
default to Match system; existing Atelier/Night Studio choices remain fixed.
The actual SingZ segmented control exposes all three choices, and native
form-control colors track the resolved palette. Native AppKit window changes
reach WebKit's real media query, switching palettes without routing commands or
rewriting the automatic choice. Document reload retains automatic mode and its
subscription. The titlebar quick toggle chooses an explicit opposite palette.
Normal/sanitized native checks, Settings layout, keyboard and authenticated
private bridge/storage regressions are recorded in
`artifacts/system-appearance-proof.json`. The additional complete-process
preference check below covers private application identities. Physical
keyboard/VoiceOver and deployed UI acceptance remain open. The frozen signed
update is unchanged.

Default WebKit preference storage now has seven-process, private bundled-app
acceptance. A document-start probe observes saved state before UI initialization.
Both explicit palettes, automatic appearance and device selection survive actual
application process exit. Opposite native appearances resolve the automatic
choice afresh, and a second application identity starts empty without replacing
the first identity's settings. Re-signing a version-2 bundle under the same
identity changes its executable image while retaining those preferences.
Production resource/configuration functions and the real interface are used;
the bridge is synthetic and cannot edit routing or open audio. Both private
stores report zero remaining records after public WebKit cleanup.
`artifacts/preference-persistence-proof.json` records this evidence. Canonical
installed-app acceptance, Developer ID updates, logout/reboot and power-loss
durability remain unverified; no installed payload or pending package changed.

Gain controls now commit semantic range changes that have no pointer/key release
or blur. The initial real bridge/storage case showed a -6 dB UI draft while the
saved gain stayed at unity. Native committed-event handling corrects that path,
retains one history step for held-key/pointer gestures and deduplicates release/
blur events. Slider-position comparison also prevents focus/blur from changing
fractional saved gains. Rejected device/source/monitor saves restore their saved
values and preserve uncertain-save review. Native AppKit arrows, direct primary
file reads, history/reconstruction, three command paths, failure/reload and
normal/address/undefined checks are bound in `artifacts/gain-commit-proof.json`.
These checks cover semantic event handling, not actual VoiceOver operation;
full accessibility and deployed audio acceptance remain open. The frozen signed
package and installed files are unchanged.

Workspace virtual-device formats now cover 44.1, 48, 88.2, 96, 176.4 and 192 kHz
through one portable policy shared by transport, broker, configuration, renderer
and HAL. Nominal rates and stream formats enumerate the same choices. The real
SingZ creation dialog and device header consume Engine capability metadata;
older helpers retain the two-rate fallback. Creation defaults to a fresh,
supported default hardware output rate. A capability change preserves an open
draft visibly and prevents unsupported submission. Private native WebKit edits
create 96 kHz, durably save 192 kHz and restore it after runtime reconstruction.
Driver factories exercise all six formats at 2, 8 and 64 channels, including
clock progression, carrier samples and persisted native client rate requests.
Conversion coverage includes 8 kHz through 192 kHz render clocks. Evidence and
limits are recorded in `artifacts/high-rate-proof.json`. Matching driver, root
broker and Engine deployment is required: the pending signed recovery package
is unchanged and does not contain this feature. Real high-rate audio, its CPU
cost and latency remain unmeasured; a higher rate alone is not a latency result.

Workspace high-rate choices now also follow the authenticated running components.
Negotiation extends only an acknowledged new handshake; old attachments and old
four-field pair notices remain compatible. A broker intersects helper, driver and
local rate policies, scoped to the current pairing epoch. Missing or unavailable
components keep the two baseline choices. Backend save/publication guards prevent
new unsupported formats from reaching an older peer. Existing higher-rate intent
is retained during downgrade, including unrelated saved controls, and resumes
when compatible components return. HAL format-list changes preserve the current
nominal format, IO state and clock; temporarily unsupported native rate requests
remain queued until compatible peers return. Tests compile unchanged prior wire
implementations alongside current code, cover all eight version combinations in
both connection orders, malformed masks, a partial rate set, snapshot metadata,
publication refusal and durable downgrade/recovery. Native factory checks verify
the retained active format and clock. `artifacts/rate-negotiation-proof.json`
records the evidence and deployment limits. Installed payloads and the pending
signed recovery package remain unchanged.

Individual output pairs can now be removed from the workspace editor, including
the first or a middle pair. The portable operation removes attached wires and
renumbers surviving pass-through, source, monitor and dependent virtual-source
wiring without changing physical channel numbers or surviving gains/controls.
The actual SingZ confirmation reviews all affected devices, rejects stale or
unsupported submissions, supports native keyboard focus/activation and preserves
rejected intent. The final pair cannot be removed. Engine capability metadata
keeps this action hidden with older helpers. One durable edit supports whole-
graph undo/redo. Core tests cover every pair position through 64 channels,
sample-level zdsp routing and dependent virtual audio. Private factory checks
exercise saved 8→6→8→6 changes with active IO while the unchanged dependent
mapping/clock stays intact. Normal and instrumented native WebKit checks cover
actual private bridge/storage, history and failure/reload behavior. Evidence is
`artifacts/output-pair-removal-proof.json`. Installed format transitions, actual
hardware continuity and full accessibility remain acceptance gates; system files
and the frozen pending signed package are unchanged.

The workspace now has a signed coordinated app/driver/broker package boundary.
An outer complete manifest pins the nested app/driver package and new broker;
fixed publisher identities, entitlements, executable shape and compiled rate/
pair-edit capabilities are checked before publication. Capability inspection is
forbidden as root and cannot start a routing session. Verification repeats after
inspection. The real SingZ disposable-keychain workflow signs all components;
the exact signed driver passes private factory regression checks and the desktop
interface seal verifies. Private policy and four-target file rollback rehearsals
pass under both Python versions. `artifacts/suite-update-staging-proof.json` binds
the candidate and evidence. This is packaging progress, not a deployed upgrade:
authenticated broker stop/restart, complete runtime rollback, pending-journal/
multi-session admission and abrupt-loss recovery still need a coordinated
installer. The pending signed package/command, frozen installer modules and
installed payloads remain unchanged.

The coordinated update now has durable file preparation, promotion and recovery
for the desktop, Engine, HAL driver and root broker. A protected journal records
inventories and parent/replacement identities before active renames. Recovery
restores original inodes without overwriting foreign replacements and retains
new images until the runtime owner certifies restoration. Durable commit and
restoration decisions make interrupted cleanup resumable, including partially
deleted obsolete directories. Private child processes are terminated at each
recorded preparation/promotion/rollback/cleanup boundary. The actual installed
and signed candidate payloads also pass disposable file-only replacement and
rollback rehearsals. Pending uninstall/update admission is mutual in the new
file layer and workspace uninstaller, with lock-time rechecks for removal.
`artifacts/suite-file-recovery-proof.json` records evidence and limits. Exact
broker generation/lifecycle, runtime recovery, full update session admission and
installed high-rate/continuity validation are still required before a coordinated
root updater is available. Physical power loss is not tested. The old reviewed
installer modules, pending signed command/package and installed payloads remain
unchanged.

The broker lifecycle now binds the running process's dynamic signing identity,
kernel generation and mapped executable inode to the fixed root launchd policy.
Old code remaining resident after a file rename is distinguished from the new
canonical file. Only that audited job may be unloaded, without PID signals;
startup observes the proxy until the expected image is running. Completed or
ambiguous startup resumes by observation rather than another bootstrap. Private
policy tests reject changed publishers, jobs, session identities, endpoints,
images and process generations. Real signed native GUI fixtures verify four-file
promotion, broker replacement and original-inode/runtime restoration. A separate
worker exits immediately after launchd accepts startup; recovery reuses that
running replacement before restoring the original installation. These fixtures
open no audio and register no public bridge. `artifacts/broker-runtime-proof.json`
records evidence, preserved production broker job/bytes and limits. The regular
user cannot perform detailed kernel inspection of the root broker; acceptance
of the production root lifecycle remains part of the guarded administrator path.
Isolated driver restoration, durable full runtime context, integrated multi-user
admission and actual routing/continuity acceptance still precede a coordinated
root installer. Installed payloads, the pending command/package and all twelve
frozen installer modules remain unchanged.

The isolated-driver recovery layer now tracks original/replacement executable
inodes and every image seen by each verified host generation. A tainted host
cannot certify rollback by retaining an old mapping after unloading new code.
A transaction-bound companion record persists signal intent/outcome and the
updater's kernel generation. Fresh recovery can retry only after proving the
prior actor ended and auditing the same isolated host again. The same actor
does not blindly repeat a request. Native private GUI fixtures load different
signed dylib versions, restore four original file inodes and verify fresh old
runtime after replacement. Actual worker exits cover before/after dispatch and
after outcome recording. Mixed-image history remains effective after the new
dylib is really unmapped. Protected record storage is shared with the unchanged
file schema, and orphan-driver records block new updates/removal.
`artifacts/driver-runtime-proof.json` binds the evidence. These are private
user-owned loader fixtures, not the installed Apple HAL host. A complete durable
broker/protected-state context, coordinated owner/finalization, integrated
session/continuity admission and installed routing/latency acceptance still
precede the administrator updater. Production services, signed packages and
the twelve frozen installer modules remain unchanged.

The administrator-applied combined recovery package is now verified against the
root-owned installation report and all 15 installed payload files. Its manifest
is `06b03b0744aee6d309b065727d6a79f156b730421fb49bd60b60e0b8386a1981`.
The installed shared-client regression passes with two concurrent writers and a
third prepared, nonrunning client: both readers retain their device/stream/clock
identity through private Engine crash recovery. The fixture normalizes the two
summed markers to the diagnostic's unit-gain reference; its earlier unnormalized
baseline failure is retained. This replaces the old pending-deployment baseline,
without installing the newer coordinated suite candidate.

Installed visibility edits retain the existing input device and no longer show
backward timestamp resets in the detailed run. They still interrupt callbacks:
eight edits produced 16 forward timestamp gaps, the largest about 255 ms. That
run had no zero samples, but an earlier pre-edit baseline had isolated zero
samples and remains a separate failure. Visibility continuity is not accepted.
Mixed-test observations also expose timing reconfiguration during concurrent
client startup. An optional `stdio-v2` diagnostic barrier keeps every client open
until the public timing plan is stable, then starts the same strict baseline and
recovery checks. It uses a private signed probe and the unchanged installed
routing Engine. All five clients (2/8/64 channels and both app-capture readers)
pass recovered signal checks without reopening or post-recovery zero samples or
timestamp errors. The immediate physical-monitor check after those clients close
still fails, although the subsequent snapshot shows both monitors consuming
again. This is partial acceptance, not a full mixed-test pass or proof of
continuous monitors across client teardown. Evidence and preserved failures are
in `artifacts/combined-recovery-installed-validation.json`; no new end-to-end
latency or Loopback comparison is claimed.

The private monitor-retention candidate now passes the complete ten-device mixed
test, including all five existing clients through Engine crash recovery and the
physical-monitor checks after those clients close. Unchanged monitor programs
retain their read cursor, meter and cumulative observations across unrelated
scene replacements; real monitor edits remain staged. The mixed test now tracks
actual monitor callbacks/history coverage and physical acquisition owners while
public IO timing reconciles, and then requires the full graph to settle and
render steadily. The previous signed build still fails that same continuity
check. The corrected build adds no monitor missing frames or physical input
reacquisitions after the healthy baseline. Normal, address/undefined and thread
sanitizer fixtures pass, including concurrent committed/aborted edits and a
physical rate recovery while an identical edit is pending. Evidence is
`artifacts/monitor-retention-proof.json`. This corrects the earlier inference
from reset online flags/counters: those flags alone did not prove an audible
interruption. The corrected app/Engine pair is privately signed and tested, not
installed. The installed baseline remains manifest `06b03b…a1981`, and visibility
callback gaps, coordinated production deployment, unplug/wake and broader
acceptance remain open.

The coordinated suite owner now persists the original broker/session/protected
state context and terminal cleanup decisions, including private diagnostic
references. Private four-target rehearsals cover 20 actual worker-exit boundaries,
ordinary rollback, cleanup after restoration, a foreign inode with identical
bytes, and a rejected external diagnostic path. The macOS runtime/diagnostic
adapter and administrator entry point are still pending; these model-backed
owner tests do not certify a production broker/driver update.

## Reference behavior

- [Device lifecycle and persistence](https://rogueamoeba.com/support/manuals/loopback/?page=devicebasics)
- [Sources and controls](https://rogueamoeba.com/support/manuals/loopback/?page=sources)
- [Channel counts and wiring](https://rogueamoeba.com/support/manuals/loopback/?page=outputchannels)
- [Pass-through and output visibility](https://rogueamoeba.com/support/manuals/loopback/?page=passthru)
- [Monitors and activation](https://rogueamoeba.com/support/manuals/loopback/?page=monitors)
- [Client use and sample rate](https://rogueamoeba.com/support/manuals/loopback/?page=usingvirtualdevices)
- [Appearance and update preferences](https://rogueamoeba.com/support/manuals/loopback/?page=settings)
