# LoopZ architecture

## Pinned zCore runtime

LoopZ builds its portable shared engine from the zCore Git submodule in
`vendor/zcore`. The parent repository's gitlink pins the exact source revision.
`scripts/verify_vendor.py` and CMake verify that the submodule is initialized at
that revision and clean without fetching or modifying it.

The graph runner, planar gain/limiter code, and reusable graph/runtime tests come
from zCore. LoopZ no longer generates a local runtime overlay. The shared
runtime includes finite-sample containment, planar processing, and pre-limit
limiter peak/RMS telemetry. Initialize the dependency before configuring a fresh
checkout:

```sh
git submodule update --init --recursive
```

LoopZ and SingZ pin the same zCore main revision,
`b8905f6ca6da354997db3e6f9c5aa730bd3c64f1`. The gitlink records the exact
source in each checkout. Ubuntu portable-core builds require `libasound2-dev`,
`libudev-dev` and `pkg-config`; these are build dependencies for the shared
portable layer and do not provide a LoopZ Linux desktop audio backend. LoopZ's
product capture, device management and lifecycle remain in its macOS backend.

## Candidate: separate callback dimensions, 21 October

`CallbackTimingObservation` is a platform-independent, header-only metadata
counter. One serial callback producer accumulates the actual WriteMix frame
maximum and nominal IO-period maximum. One lock-free 64-bit word holds the
32-bit IO session plus two 16-bit maxima. Control assigns distinct nonzero
sessions; a callback takes its expected word at entry and uses one conditional
compare/exchange. A reset therefore rejects late old-session updates without
spinning. There are no callback allocations, locks, OS calls or timing-policy
decisions in this counter. A snapshot pairs coherent accumulated maxima, not
necessarily values from the same individual callback.

The candidate driver exposes these facts through the additional version-one
`lzcb` CFPropertyList dictionary. The legacy four-key `lztm` property remains
unchanged for installed Engines. Both properties are advertised and read-only.
Reads do not contribute observations; only accepted WriteMix data does. Counter
resets follow existing IO-session starts, while multiple active client references
and transport handoffs retain the lifecycle. Existing transaction and timing
approval paths are unchanged.

The standalone `loopz-callback-timing-probe` accepts at most 32 distinct LoopZ
UIDs, validates virtual model/transport identity and bounded versioned data, and
opens no audio. It reports unsupported observations for the installed older
driver rather than inventing actual sizes from caller buffer settings. The
legacy and new properties are separate reads; `sameSession` does not claim that
they were fetched at one instant. The prepared fixture observer authenticates
the private Engine generation and its frozen package, and reads only its owned
device UIDs. It cannot stop, restart or edit the fixture. Its metadata queries
may perturb timing and are diagnostic evidence rather than clean capacity
acceptance.

Portable, Release and sanitizer checks pass, including stale reset and coherent
concurrent snapshots. Fake-host driver tests cover 64 actual/128 nominal and 128
actual/64 nominal; old-driver live probe compatibility is verified. The signed
candidate and frozen installer passed deployment; the added property is verified
on a real capture-free HAL endpoint. Under full-wide load both diagnostic
attempts time out before returning new raw values. No latency fix is inferred.
Evidence: `artifacts/callback-observation-candidate-proof.json`.

## Batched aligned public input, 21 October

`TransportAudioSource` batches aligned equal-rate reads into 32-frame interleaved
tiles, then sanitizes and deinterleaves the requested output planes. The tile is
bounded stack storage; readers retain independent cursors and no mutable shared
workspace. Every transport frame still validates its atomic tag. No atomic
ordering, transport protocol, IO ownership, scheduling, lead or delay changes.
Fractional and different-rate reads retain `AudioResampler`.

The fast path stays 1e-7 inside the reference alignment cutoff. Adding frames can
cross a floating-point exponent boundary and move a near-cutoff fraction across
that cutoff mid-block. A demonstrated unguarded failure at 44.1/88.2/176.4 kHz
is retained; the corrected path falls back to the reference for those cases.
Exact comparison also covers missing frames, ring wraps and channel subsets.
The standalone adapter improves about 1.71 times for aligned 64-channel blocks;
no whole-app speedup or physical-latency improvement is inferred.

The diagnostic stores only fixed-size first-cadence-anomaly metadata in the
callback and encodes it after IO stops. Host counters explicitly include startup
and shutdown. The fixture's graph verifier checks live timing throughout sampled
recording observations and unchanged documents after stopping. Input stop can
legitimately renegotiate timing; it cannot exempt any measured native sample or
live mapping change. Regression tests cover both sides of that boundary.

The signed installed manifest is
`13ea46db0c47d8c32360a9b3653f229cb7b5fc32f8b98b26656402fa8940c217`.
Private and installed mixed-load recordings pass; independent 32-wide repeats
remain failed. One observed mechanism is live period growth followed by latency
reconfiguration, and another is a capture miss without cadence failure. The
installed driver's timing value currently combines actual callback frames and nominal IO
period. Separate observations are prepared in the candidate above; live measurement is pending.
Presentation latency must still use host-approved configuration changes, as
[Apple's driver contract](https://developer.apple.com/documentation/coreaudio/audioserverpluginhostinterface/requestdeviceconfigurationchange)
requires. Evidence: `artifacts/public-block-input-delivery-proof.json`.

## Coherent graph plans and asynchronous public timing, 21 October

The installed signed app pair has manifest
`750998b467d306f067f0eae14a3b6d1da232d5543fe4595792271e3b372ed051`.
`GraphActivation::plan` freezes source timing,
pass-through floors and the resulting endpoint latencies once per submission.
Transport allocation and DSP preparation consume that same single-use plan;
active reconciliation still inspects subsequent observations. This removes the
observed allocation/preparation mismatch when native callback timing changes
between two reads. Regression binaries linked against the former controller
and former restart policy fail their corresponding new checks.

`PublicTimingObserver` moves UID resolution and timing-property reads into one
background batch. The control owner consumes immutable results, schedules the
next batch 100 ms after consumption, and never waits for the native query.
Target changes or explicit service invalidation reject older-generation results.
The detached worker owns its catalog and completion state, with no Engine,
renderer or control-owner references. Per-device errors preserve normal retry
behavior. No native operation or new lock enters the render callback.

The catalog still caches only identity validation internally; its returned live
period, session and running state are now retained in the observer's last
completed batch. Runtime debug snapshots expose that batch revision and each
observation without additional native reads. A newly started IO session with no
measured callback retains the last actual period instead of inventing a larger
512-frame period. An absent endpoint or legacy session-zero report clears that
measured-period assumption; an actually larger callback still grows the queue.

All 32 independent 64-channel clients now reach a strict ten-second recording
window. The first candidate trial completes control calls within 108 ms, but
ten recordings contain zero-filled samples and the renderer adds 6,443
publication deadline misses. Callback cadence and timestamps remain intact.
This is a failed capacity result, not acceptance or a physical latency result.
The exact files, installed apps, physical buffers, defaults and Core Audio are
verified restored afterward. Both private and installed six-client mixed-graph
regressions pass thirty strict seconds without missing samples, cadence errors
or added renderer misses. The reviewed app-only update preserves the driver,
broker and interface resources. Evidence is retained in
`artifacts/timing-observer-delivery-proof.json`; the failed wide trial remains
included rather than being treated as acceptance.

## Read-only discovery outside the command loop, 21 October

Native `DeviceInventory` uses a single background metadata query. The control
owner publishes completed results and cached names, with `devicesRefreshing` and
`devicesFresh` distinguishing pending discovery from a current result. Invalidation
increments the generation; a late old-generation result cannot become current.
Failure preserves names and retries after the bounded polling interval. A blocked
query owns a copied provider and shared result state, so inventory destruction
does not wait for it or leave it referring to a destroyed owner. It never opens
audio or receives the buffer writer. Injected providers retain synchronous mode
by default; explicit background providers must own their captures and support a
concurrent explicit validation read.

While discovery is pending, `DeviceSettings` retains existing applied preferences
and polls for completion without writing from stale metadata. Explicit buffer
requests retain fresh native validation and expected-value checks. Native public
timing caches only immutable object validation: UID resolution and live
period/session/activity reads still occur each time. Disappearance, replacement,
read failure and audio-service reset invalidate that cache. Render callbacks
never access either discovery mutex or native queries.

The original 32-wide fixture times out in control; a live sample locates its
remaining private-candidate wait in snapshot-triggered inventory discovery. The
combined change answers observed wide-fixture calls below one second. Private
and installed six-client mixed-graph repeats pass strict 30-second recordings
with no missing samples, cadence errors or added renderer misses. A separate
preparation limit still prevents full 32-independent-wide recording acceptance.
Other synchronous native operations are not made asynchronous by this change.
Evidence: `artifacts/background-discovery-delivery-proof.json`.

## Synchronized input-only observation, 21 October

The diagnostic `--virtual-input-check --capture-protocol stdio-v1` separates input
preparation from measurement. It opens an input-only AUHAL unit with the existing
virtual-device and microphone-authorization guards, renders preparation callbacks,
then emits `prepared`. The controller waits for all clients and routing timing to
settle and sends `begin-capture`. Every measured callback and sample then counts;
the legacy one-second warmup is disabled only in this opt-in mode. Preparation
callback/failure counts remain explicit.

After the requested window the unit stops and drains callbacks, then emits
`captured`. The controller sends `release-input` only after all readers finish.
Shared buffer-period restoration and unit disposal therefore cannot interrupt a
peer still measuring. Commands have a 20-second deadline, a 32-byte bound, exact
allowed values and cancellation/EOF handling. The normal diagnostic mode keeps
its prior behavior.

Portable `probe::CaptureTrace` stores fixed-capacity zero-span fragments and
callback anomalies without samples, locks or heap work. Spans split at callback
boundaries and include captured-frame offset, exact-zero channel mask, input host
time, callback receipt time and render flags. Callback anomalies retain both
sample-frame and host-time jumps, including invalid timestamps. Metadata is read
only after the callback gate drains, then serialized on control with decimal
timestamp strings. Overflow or invalid blocks fail the diagnostic.

Release/sanitizer boundary, marker and protocol tests pass; backend-disabled
portable checks pass. The signed private diagnostic locates the baseline's zero
span and all 16 callback gaps inside actual visibility edits, while the unrelated
reader, no-edit baseline and fresh reader stay clean. This localizes an existing
failure and does not accept visibility continuity. The installed apps and routing
renderer remained unchanged in that diagnostic acceptance. The mode is included
in the later signed producer-settling update below. Evidence:
`artifacts/input-trace-delivery-proof.json`.

## Visibility reconfiguration boundary, 21 October

The first exact-mapping producer hold was rejected and rolled back after native
input tests. Synchronized re-evaluation also found returned zeros: 120 samples
on the unchanged baseline and 84 with that first candidate. That hold ended its
completion grace on the first IO resume; a single layout approval can contain
additional pause/resume cycles.

The revised renderer retains an already-active sink through those cycles.
Preparation starts retention only after all staging/allocation succeeds, only
for a playback-visibility edit that reuses the exact capture mapping and input
format. A pending transaction is bounded to ten seconds; commit or abort changes
it to a 500 ms settling deadline, without extending an already-expired hold.
The first resumed callback no longer clears that deadline. Retired mappings and
previously idle sinks are excluded, and format changes retain fresh storage.
Normal source demand, original-playback policy and monitor polling use the same
sink request. If a client actually stops in that settling window, source teardown
or original-playback unmuting can consequently wait for the remaining grace and
the normal control poll. No render-thread allocation or lock is added.

Two signed private trials have zero returned zero samples and no capture-miss
counter increments across eight edits each, with clean baseline, unrelated and
fresh-reader controls. They still fail continuity: 16 and 15 forward callback
gaps remain. This is partial sample recovery, not uninterrupted reconfiguration.
The installed normal-session repeat also has no returned zero samples or capture
miss increments across 1,263,936 edited frames, but retains 15 forward gaps.
Evidence: `artifacts/visibility-profile-comparison.json` and
`artifacts/visibility-settling-delivery-proof.json`.

Output stream-layout changes still use native host approval. Apple's contract
allows the host to stop IO and restart it after approval; changing property
notifications alone cannot lawfully replace that operation. The latest actual
test keeps an unrelated recording continuous while the edited input has callback
gaps. Its live latencies and mapping generations remain stable. Future diagnosis
must distinguish host pause/padding, input-client startup and transport misses.
The older input-only mode lacks a coordinated capture barrier and per-zero-span
timestamps, so its aggregate counts cannot locate every silence. The newer private
mode above adds those observations without installing a routing change. Evidence:
`artifacts/visibility-rollback-proof.json` and its bound SDK/manual research.

## Known-silent source metering, 21 October

Portable `AudioMeter::publishSilence(frames)` publishes zero peak/RMS for a known
silent block while retaining its positive valid frame count. `clear()` uses zero
frames for idle/invalid observations. Both use the existing serial-producer,
atomic-payload, bounded-seqlock observation contract without changing memory
ordering or allocating on render.

`AudioScene` uses this path only for offline or disabled sources, after all their
scratch planes are cleared. Online sources retain the ordinary sample scan:
`missingFrames == frames` can mean one channel is missing in every frame while
other channels contain valid audio. Tests cover that case, returning audio,
silent/active/idle transitions, concurrent observations and no callback heap
events. Native installed checks confirm valid 64-channel silent source meters
and uninterrupted recording during expansion/removal of 31 wide devices. The
two runs have 3 and 0 late publications respectively, with none during their
full-graph holds; this does not establish sustained worst-case capacity.
Evidence: `artifacts/silent-meter-delivery-proof.json` and its repeated run.

## Unused monitoring and exact-rate output, 21 October

The macOS monitor adapter returns the existing public sink directly when the
device is disabled or has no enabled monitor. No monitor bus/history or physical
host is allocated in that path. Staging never changes an older active monitor;
the ordinary committed-scene poll closes endpoints after their bindings are
removed. Enabled monitors, including muted or unavailable monitors, keep the
existing bus, demand, acquisition and recovery paths. Portable monitor DSP is
unchanged.

Portable `CausalResampler` retains the exact one-sample delay for equal integer
rates by copying its already sanitized interleaved ring history. Startup and
reset padding remain zero, partial blocks and wraps preserve exact samples, and
timestamps/invalid-input admission are unchanged. Unequal rates retain the
existing band-limited conversion. No shared-memory protocol, atomic ordering,
buffer size, scheduling reservation or timing budget changes in this update.

`artifacts/capacity-transition-delivery-proof.json` binds the installed candidate
to an existing 64-channel recording across addition/removal of 31 active wide
devices, its actual failed baseline/profile, exact-sample regressions, full
conversion matrix and original-state restoration. The recording passes without
zeroed samples or callback-cadence errors. Late renderer publications remain
reported; this is not acceptance of sustained CPU margin or 32 wide readers.

## Capacity work and measurement boundaries, 21 October

The macOS device picker and hardware-settings adapter now discover only the
metadata they consume. Discovery excludes LoopZ virtual endpoints before reading
channel metadata and never asks every device for every channel label. The typed
inventory projection, channel counts, current buffer and supported buffer bounds
remain unchanged. Reconciliation does not revalidate or write a saved buffer that
already equals the fresh device value. Explicit buffer changes retain their
freshness, identity, range and read-back checks. Discovery opens no audio IO.

The portable routing compiler omits sources that cannot contribute audio while
retaining original source order and validating every declared input buffer. It
keeps the SingZ channel-map, gain, mix and safety-limiter processors. Unity device
gain is redundant only when the limiter supplies the required nonfinite handling;
without a limiter the unity gain processor remains. An unused internal DSP meter
is removed; `AudioScene` still publishes all public pre/post-gain meters. These
changes allocate and compile on control, with no new render-thread allocation,
lock or destruction. They do not change timing budgets or permission policy.

The opt-in `--routing-io-check --capture-protocol stdio-v1` diagnostic separates
client preparation, strict capture and writer release. The controller starts the
capture window only after all clients and graph timing settle. Every sample in
that window is checked, without a warm-up exemption. Each writer stays open until
every reader reports capture complete; independent writer shutdown cannot be
mistaken for a downstream dropout. Reports include first/last zero frame and
callback size, sequence, source-frame and host-time cadence checks. Protocol
commands have bounded waits and are accepted only after both endpoint identities
and existing microphone authorization pass the normal virtual-only admission.

The prior unsynchronized test and genuine pre-optimization control/render load
failures remain retained. A short capacity window cannot establish long-run
stability, full worst-case source/monitor load or physical audio latency.

## Installed source process replacement

`artifacts/app-relaunch-policy-delivery-proof.json` verifies actual source-process
replacement with the fixed recording/control tap design. Two original public
clients preserve device/stream IDs and clock origins across the owned source's
exit/relaunch. The saved six-device fixture is retained, and original-playback
muting returns after reacquisition. A second uniquely identified application
continues a 28-second waveform recording with no missing-frame growth or capture
reacquisition, independently checking source selection and provider isolation.

Internal mappings have a different lifetime from public devices. Timing may adapt
after recording clients close, so idle mapping generations are not evidence of a
client disconnect. The final fixture checks mappings while clients remain open;
native observers independently check public identities and clocks for their full
lifetime. The original overly broad post-teardown assertion remains in the test
evidence. Source-absent silence is measured explicitly rather than described as
continuous recovered audio. No installed binary change was needed for this gate.

## App metering demand and original-playback ownership

The installed source policy separates preview from actual public/monitor demand.
Preview activates metering through enabled virtual dependencies without imposing
an app's mute-original policy. Actual client demand, including retained demand
from staged native mappings, combines mute requests across all current logical
routes sharing that app provider. Uncommitted bindings do not acquire capture.
Disabled devices do not become active through the preview mask.

Each application recording tap stays unmuted with a fixed description and one
unchanged capture aggregate. A second private mute-when-tapped tap has a prepared
private aggregate and a callback that retains no samples and accesses no owner
state. Its IO is started only when original playback must be muted and stopped
when the final mute request ends. Neither tap description changes during a
policy transition. Multi-stream application groups update owners together and
attempt rollback after a partial failure; the registry then gates admission,
closes the group and follows normal reacquisition. The mute reader is stopped
before capture/tap destruction.

`artifacts/app-playback-policy-delivery-proof.json` binds ten real installed
phases to exact app bytes, two continuously open virtual recording clients and
one shared capture acquisition. Live-description edits were rejected by actual
signal acceptance after they caused gaps; an unread control tap also failed to
apply muting on this host. Their failed reports remain alongside the final
passing lifecycle implementation. The extra private tap/aggregate has not yet
completed wide multi-output/resource or crash/relaunch acceptance.

## Saved-document recovery feedback

`artifacts/document-recovery-delivery-proof.json` binds current installed
backup recovery to its tested desktop bytes. The Engine's existing file watcher
validates both document files, preserves a running desired graph after failed
loads, and refuses editor writes until pending external content is readable.
A supported valid backup may recover an unreadable primary; an unsupported
newer schema cannot silently select an older backup. Recovery does not rewrite
the original files. Actual virtual IO remains continuous during the tested
running-file failures and repair, with the whole saved fixture graph retained.

The interface consumes the authenticated `restoration.recoveredBackup` flag.
It warns that routing can be older, but suppresses that guidance for disconnected
cached state, absent routing or a current document error. It neither edits
routing nor initiates recovery writes. The latest interface-only delivery keeps
the exact Engine installed by the setup/publication update below.

## Current setup and publication boundaries

The installed desktop and Engine are bound by
`artifacts/setup-publication-delivery-proof.json`. The earlier app identities
below remain historical. Native setup inspection uses fixed paths in the macOS
desktop adapter. A separate serial queue performs filesystem/signature checks
and publishes owned immutable results through the transport queue, keeping
meter polling responsive. The page cannot supply paths or executable commands.

Portable `RealtimePublication` owns an immutable node per graph version. A
short sequentially consistent entry guard protects acquisition; the borrower
pins that node before leaving entry. After observing no entering readers, the
single control owner can reclaim retired nodes whose own reader count is zero.
Readers of the current node cannot retain unrelated retired versions. A reader
that could have loaded an older node is either still entering or already pinned
on that node. Allocation/destruction remain on control, commit uses reserved
capacity, and genuinely pinned old nodes retain bounded backpressure.

Deterministic overlapping readers, concurrent readers and ThreadSanitizer pass.
Installed virtual IO also stays continuous across 20 scene replacements. These
checks establish correctness in the tested cases; they do not resolve the
original physical-input miss or establish a new physical latency measurement.

LoopZ separates a portable C++ routing/audio core from the macOS driver,
transport, capture, monitor and desktop adapters. The full-product requirements
and acceptance gaps are tracked in [the product acceptance record](product-roadmap.md).
The original stereo milestone is historical evidence rather than the completion
boundary.

The existing LoopZ HAL slot now hosts the multidevice routing driver and broker
(`artifacts/routing-migration-installed-proof.json`). Newer signed workspace
Engines have passed virtual IO, silent physical output and multichannel physical
input checks. The signed desktop/Engine pair is now installed with account-owned
app updates and adaptive physical-input timing; current installation and measured
scope are recorded in `artifacts/input-timing-delivery-proof.json`. Later
sections retain component milestones and their original scope, which must not
be read as current installed-product acceptance.

The current app-only update also deploys verified application timing, matching
tap timing retention and audio workgroup scheduling. Its installed hashes,
default-profile recordings and restoration evidence are recorded in
`artifacts/default-timing-delivery-proof.json`; earlier installation references
below retain their historical scope.

The latest interface-only delivery adds transitive source health feedback.
Its traversal follows enabled, connected signal edges through virtual devices
and reports partial upstream loss without muting surviving channel meters.
Completed device checks are shared within each traversal, and an active-path
set bounds malformed cycles. A missing upstream monitor is not a failure of
the virtual input signal. The warning links to the source owner and clears on
fresh recovery telemetry; it never edits routing or requests permission.
`artifacts/upstream-feedback-delivery-proof.json` verifies the installed UI
against normal/sanitized WebKit and recorded native fault snapshots while
preserving the exact running Engine and installed audio components. The earlier
full-pair GUI hash is historical after this interface-only update.

## Module boundaries

Physical-input timing follows the same boundary. The portable `inputTimingBudget`
function converts typed input-rate/buffer/latency facts into render-frame history
requirements, including the full symmetric sinc wing, publication lead and
scheduling reserve. No platform queries run in that function or the audio render
callback. The macOS `PhysicalInputTiming` adapter reads process-specific Core Audio
facts during preparation and rejects facts that change while being inspected.
Unavailable facts retain a conservative budget. Its bounded cache retains
identities referenced by desired or committed documents so a rejected edit cannot
discard an active input's verified requirement.

Scene planning and prepared rendering use the same per-binding requirements;
hardware changes between those stages reject preparation and preserve the
committed graph. Application and pass-through histories keep their own budgets,
and virtual dependencies inherit upstream alignment. Explicit diagnostic
`--source-delay-frames` profiles disable automatic physical-input overrides.

Application timing is now automatic in normal sessions. The portable
`ApplicationInputTiming` planner requires verified timing for every contributing
stream and converts it through the common budget function. The macOS capture
adapter queries private tap rate/period/latency on control after valid callback
delivery. Matching reopened routes may reuse verified history only after checking
their actual format. Unknown formats remain conservative; unsupported timing
reacquires only the affected provider. Active public or monitor demand, including
virtual dependencies, pins the committed app floor against automatic shrinkage.
An editor preview alone permits optimization before recording begins. Explicit
global diagnostic budgets disable automatic app timing unless the user explicitly
requests a contradictory combination, which is rejected.

Runtime diagnostics have two different counter lifetimes. Source
`missingFrames` and device `renderedFrames` belong to the prepared `AudioScene`
and restart when live edits publish a new scene. Transport read/miss counters
belong to the mapping generation and survive scene-only replacement. Cross-edit
continuity acceptance therefore keeps the same recording client open and
compares transport counters only when its generation is unchanged. A negative
source-counter difference across edits is not lost audio or a valid underrun
delta. Installed two-app relaunch acceptance demonstrates this distinction in
`artifacts/installed-app-relaunch-proof.json`.

A third counter lifetime is now available: a shared provider's
`missingReadFrames` survives scene replacement and reconnects until that provider
is discarded. It counts unavailable frames per logical read, including admission
gates. It complements scene and transport counters; natural zero-valued samples
are not counted as unavailable frames. The macOS renderer's publication-deadline
counter measures completed renders against their publication frame, without
rounding misses to a whole render block. Its independently clocked realtime
thread owns an Apple audio work interval, joins it for the thread's lifetime and
starts/finishes each work cycle through the realtime-safe workgroup API. Creation
and release remain on control. Workgroup status/errors and deadline diagnostics
are exposed through authenticated snapshots; no Apple API enters the portable
graph, timing budget or source-history interfaces.

Optional portable `AudioHistory` read diagnostics now distinguish unavailable
anchors, unavailable clock mappings, insufficient resampling support, offline
sources, invalid requests and generation changes. Each missing output frame is
assigned once per read. Separate failed source-sample lookup counters classify
unpublished, expired, racing and otherwise absent storage; filter taps are not
output frames, so those counters must not be added to the output-frame totals.
Diagnostics are disabled by default and allocate only at construction. Their
atomic lifetime counters support independent readers and survive stop/reopen;
live snapshots are independent observations, while final evidence joins readers.
The macOS capture-only diagnostic opts in through `CaptureSourceOptions` and
compares a post-warm-up baseline with joined-reader totals. It neither publishes
HAL devices nor opens physical outputs or modifies buffers. This is diagnostic
instrumentation, not a change to production queue sizing or clock acceptance.

Physical-source diagnostics now also travel through the production Engine's
ordinary status path. Portable `SourceEndpoint::diagnostics()` returns an
optional immutable control-domain snapshot; default endpoints return no payload
and allocate no diagnostics. `SourceConnectionStatus` carries the snapshot to
the native CLI and authenticated editor codec. Live history counters are atomic
observations. Callback-owned delivery data is omitted while the input is open
and becomes available only after close joins callbacks. Closing bounds delivery
at callback entry before AudioUnit shutdown, preserving any earlier explicit
diagnostic cutoff. Exact counters and signed nanosecond ages remain strings
at the JavaScript boundary; the bounded event list contains no audio samples.
History counters follow the endpoint lifetime, while delivery observations
restart at each capture open and must be interpreted with the source attempt.

The private production Engine passes a ninety-second, two-input preview run
through the installed driver and authenticated XPC, with joined delivery data
after release. The normal installed apps remain unchanged. The current restored
session and exact scope are recorded in
`artifacts/full-source-diagnostics-proof.json`. The interface updater's earlier
PID-preservation evidence refers to delivery time; these authorized diagnostic
runs subsequently restart and restore the canonical Engine.

The native device-recovery diagnostic exercises the same source and monitor
owners against a real process-private aggregate over physical hardware. It
destroys/recreates only its owned UID and waits for asynchronous disappearance;
normal inventory events and runtime polling perform recovery. A four-device
graph retains its definitions and silences only the absent branch. An
independent physical source and monitor retain their acquisitions and counters.
The virtual-driver configuration link stays unpublished, isolating this native
backend test from installed routing. The optional native monitor paths are
muted and use a final output guard that clears every sample, even after a failed
renderer. No rate/period or exclusive-mode request can pass that boundary.
Apple's [private aggregate flag](https://developer.apple.com/documentation/coreaudio/kaudioaggregatedeviceisprivatekey)
limits visibility to the creator process. This fixture verifies native handle
replacement, not physical USB firmware behavior or sleep/wake. Exact scope and
three observations are recorded in `artifacts/native-device-recovery-proof.json`.

The pass-through visibility candidate distinguishes host-approved changes on a
reused capture mapping from uncommitted input storage. After host approval, a
visibility-only device prefix may keep serving capture while a peer's approval
or this prefix's rollback approval remains pending. This exception requires
identical mapping identity/generation, channel count, sample rate and input
latency. Fresh mappings and input timing/format changes keep the existing
configuration mute until full graph commit. The candidate passes isolated
native driver and sanitizer regressions; it is not yet installed. The separate
host stream-layout pause is still an unresolved continuity gate. See
`artifacts/visibility-input-prefix-proof.json`.

Capture and monitor recovery sample the supplied monotonic control clock at
each retry/acquisition/completion boundary. Later endpoints do not inherit time
spent opening an earlier endpoint in the same poll. Observations never move
backward within the poll, and an individually expired acquisition remains
unadmitted. Monitor callback-stall grace begins only after successful admission.
All of this runs on the control owner; render callbacks acquire no new clocks,
locks or allocations. Calls into native providers remain synchronous. The
signed app candidate and isolated regression scope are recorded in
`artifacts/recovery-clock-proof.json`; it is not yet installed.

The experimental `MinimumPhaseKernel` prepares a finite causal coefficient
table by real-cepstrum spectral factorization on control. Its FFT follows the
SingZ legacy implementation without modifying vendor sources. `AudioResampler`
can explicitly select that table; its default constructor preserves the existing
linear-phase behavior. Minimum-phase reads use twice the original kernel radius
as past support and zero future support, and keep filtering at integer positions
to avoid switching nonlinear phase in and out. Tables are immutable, and the
audio read loop keeps its bounded stack workspace and nonallocating provider
interface. No native capture/history currently selects the experimental mode.
Changing-clock cutoff transitions, mixed-source alignment, performance and native
latency integration remain open in `artifacts/minimum-phase-prototype-proof.json`.

The first portability refactor separates the reusable C++20 core from the
macOS product backend. The baseline before this refactor is commit `92ce955`.

| Layer | Location | Responsibility |
| --- | --- | --- |
| Portable core | `native/core`, `native/include/loopz` | Routing graph and device document, source history, timestamp interpolation, atomic timeline, marker analysis |
| macOS backend | `native/platform/macos` | Core Audio capture/driver, Mach clock and scheduling, XPC transport, consent, inventory and process lifecycle |
| Dependency composition | `cmake/zcore` | Portable zdsp runtime and shared device interfaces from the pinned submodule |
| Validation | `tests` | Standard C++ core tests; separate POSIX process test and macOS driver/installer tests |

`CaptureBuffer::append` accepts samples, frame positions and nanosecond host
anchors supplied by the backend. `sample` and `MixingGraph::render` operate
without OS queries. The backend converts native timestamps into that common
time domain and owns callback/thread lifetimes. The core does not choose
Core Audio device UIDs, query permissions, create XPC connections or schedule
Mach threads. It does not assume that a source history capacity is a queued
delay. These are the existing concrete boundaries, without speculative
Windows/Linux transport implementations.

The Darwin `SharedAudio` layout, protocol constants and code-identity bridge
are private backend contracts under `include/loopz/macos`. Moving them does
not alter their byte layout, protocol or runtime ownership. Future backends
must provide their own IPC and virtual-device contracts; standard C++ atomics
alone do not establish a portable process-shared ABI.

`LOOPZ_BUILD_MACOS_BACKEND=OFF` configures the core without Objective-C++ or
Apple audio frameworks. Core headers cannot include backend/OS headers; a
boundary test rejects those dependencies. Tests use standard C++ math and
compiler-specific warning/assertion flags. The POSIX fork/mmap test is built
only on UNIX. Vendor manifests use slash-normalized keys, and Git preserves
hash-locked vendor bytes on Windows. The CI matrix covers macOS, Linux and
Windows, but only local macOS runs have been executed in this workspace.

Native lifecycle and command-line orchestration remain in the macOS backend.
`EngineRuntime` owns document routing and provider recovery; the stereo
compatibility path remains available.
The portable document now represents multiple named devices, application/input/
virtual/pass-through bindings, monitor wiring and source/device controls. It
validates identities, counts, connections and virtual-source cycles, including
disabled cycles. Resizing output pairs preserves unrelated source/monitor
wiring, updates dependent virtual sources and adds only new pass-through wires.
Failed document edits leave the original unchanged. The macOS configuration
layer serializes the versioned document as typed JSON, writes atomic private
files and retains the last valid backup. Recovery reports the damaged primary;
unsupported future versions are preserved in both primary and backup files.
The `loopz-config` tool can create, read, validate, rename, resize, enable,
disable and delete saved devices without opening audio or requesting consent.
Deletion rejects devices referenced by other stored sources. These edits currently
affect saved configuration only. Controller restoration, backend activation
transactions and Windows/Linux audio backends remain future work.

`RoutingGraph` prepares 1–64 output channels and up to 32 sources off the audio
thread, using actual zdsp routing/mix/gain/limiter/meter nodes. More than sixteen
external sources use prepared subgraphs to respect the pinned runner's bus
limit. Device gain and limiting occur after the group sum. Identity routing
uses direct edges or gain nodes, avoiding a quadratic dense channel matrix.
Other wiring uses the SDK matrix node. Output buffers carry explicit capacities;
invalid render inputs fail to silence within that capacity. The old three-bus
`MixingGraph` delegates to this implementation, retaining its stereo API.
These are offline core capabilities; the installed protocol/driver still expose
the original stereo device.

## Processes and ownership

`LoopZAudio.driver` publishes a stable Core Audio UID. Its virtual clock derives
from Mach host time and works without any hardware monitor. The driver does
not call Core Audio client APIs, capture applications or own the DSP graph.
Its realtime IO operations perform bounded atomic reads/writes only.

`LoopZ Engine.app` runs as the active console user. It owns microphone consent,
system-audio consent, private process taps, microphone AUHAL capture and the
zdsp graph. Launching this bundle through launchd is a separate gate from
launching its executable from a terminal. Closing a later control UI must not
terminate this helper. Login startup and operation before login are distinct;
only operation in an interactive user session is in scope.

A small root-owned launchd broker exposes `com.lexasoft.loopz.bridge`. It only
creates memory and authenticates connections. It captures no audio, invokes
no shell commands and processes no arbitrary filesystem paths. Its listener
requires Apple's coreaudiod or isolated Core Audio Driver Service identity, or the exact installed helper cdhash.
Only the active console user may attach as helper. There is one helper and one
driver attachment. Helper disconnection and fast user switching invalidate the session by restarting
the broker with fresh memory. Production Developer ID requirements replace
the pinned development cdhash during distribution packaging.

## Installation and activation requirement

The finished utility must install and update without restarting `coreaudiod`,
rebooting, changing default devices or interrupting unrelated audio clients.
The prototype's one-time global Core Audio restart was a development action;
it does not satisfy this gate. Transactional helper/DSP updates preserve the loaded driver. The updater
checks package compatibility, rolls back failed replacement, and measures
service identity, default device selections and an independent silent hardware
stream before committing. Fresh driver activation still needs an independently verified design.
Guarded isolated-driver replacement has passed on macOS 26.6.2; it is an
OS-specific experiment, not a supported activation API. No automatic global-reset fallback belongs
in the product installer.

Research the lifecycle of a minimal resident driver, replaceable helper and
versioned protocol before extending installation code. A candidate must pass
fresh-install, helper-update and driver-update checks while another app's
hardware audio stream remains active. Separately observe the Core Audio
service process identity and the unrelated stream's continuity. Keeping the
process alive alone is insufficient proof of uninterrupted audio.

Read-only inspection of installed Loopback 2.4.10 found a compiled
`Contents/Resources/arkinstaller`, `ARK.driver` 13.1.3, a per-user `arkaudiod`
agent and a privileged escrow helper. Its installer explicitly accepts a
hot-swap flag. In the inspected arm64 binary, `checkin:action:hotswap:` skips
`_resetCoreaudiod` when that flag is true. The fallback reset method invokes
`/usr/bin/killall`; hot-swap eligibility checks escrow-agent viability, package
availability, matching installer/installed build versions and absence of
virtual-device runners. These are static control-flow observations, not proof
of which branch executed during any particular installation. No Rogue Amoeba
helper was executed or modified. The exact activation mechanism is still an
open research item. Local inspection metadata is retained under
`artifacts/loopback-helper-inspection.json`.

## Driver protocol

Protocol 1 negotiates a fixed byte size, magic and version before sharing
memory through XPC. `AudioServerPlugIn_MachServices` declares the broker name.
Control connection setup, allocations, mapping and teardown happen off the
driver's callback domain. XPC must work in the installed Core Audio sandbox;
a terminal-only connection is insufficient evidence.

Two separate time-addressed rings carry helper-to-driver capture and
driver-to-helper mixed public playback. Every slot contains lock-free atomic
sample payloads, an absolute frame tag and a publication revision. Readers validate both sides of
their copied sample and never advance a shared cursor. Missing, overwritten
or interrupted samples produce silence. The callback never spins or retries.
The helper stops publishing when its broker connection dies. The driver
requires a current helper heartbeat and clears output after 250 ms without it.
On driver reconnection, mappings are retired only after callback readers exit.

This representation assumes Darwin's lock-free process-shared atomics and is
tested across processes on the supported architecture. It is not a portable
shared-library ABI. The first version fixes stereo float samples and 32768
frames of history per direction. Multiple readers address the same frames;
they do not compete for FIFO data. Public output and private helper injection
are separate to prevent recursive pass-through.

### Per-device transport preparation

`device_transport.cpp`, `transport_pool.cpp` and `transport_message.mm` implement
the separate protocol-2 building blocks for multiple devices. They are now
connected to the installed broker/HAL through the existing-slot migration.
Newer conversion candidates remain separate from that installed baseline.
The subsequent existing-slot driver update includes the mapping-handoff fix;
actual continuous-client recovery is recorded below. Protocol 1 compatibility
code remains separate from this per-device transport.

Each enabled device gets its own shared mapping, with independent capture and
playback timelines, 32768 frames of history, 2–64 interleaved channels, a format
generation, a random 128-bit mapping identity, heartbeat state and missing-frame
counters. Mapping identity also distinguishes a fresh broker's regions if its
generation counter restarts. Allocation scales with
the configured channel count. The 128-byte header and payload offsets are
checked before attachment; mapping sizes are aligned to 64 KiB so actual XPC
page rounding preserves the negotiated size. Unsupported page sizes fail
explicitly. Atomic sample payloads and per-frame tags/revisions reject partial
or overwritten frames without a reader cursor or callback retry. Nonfinite
input samples become zero. A stale or offline heartbeat silences reads. Exact-time reads also reject every
future heartbeat. Callers holding a block-start clock snapshot can explicitly
allow at most 50 ms of concurrent heartbeat publication: another callback can
advance its heartbeat after that snapshot without making valid data disappear.
This only validates liveness and does not add audio delay or alter frame tags;
far-future heartbeats remain rejected, even if a caller asks for a larger allowance.
Frame ranges exclude the reserved invalid tag and reject overflow.

The control-thread pool validates the complete document and allocates all
replacement mappings before committing an edit. Rename, reordering, wiring and
mute edits retain mappings. Format changes and disable/removal retire only the
affected mappings to silence; re-enable uses a fresh generation with the same
stable UID. Retirement also sets a persistent generation flag so a late online
or heartbeat write cannot revive old audio. Callers pin region ownership outside the callback and release it
only after borrowers stop. The XPC codec exports and imports actual shared
memory with typed, bounded metadata and move-only mapping ownership. It must
be called after role authentication; it does not authorize an XPC peer.

The pool now exposes separate `prepare` and `commit` operations. Preparation
preserves every active mapping; the controller exports the tentative snapshot
and waits for the driver's Committed result before retiring mappings. A successful
submission can still mean Pending host approval. Aborted, foreign,
stale and already-consumed preparations cannot change the active pool. Successful
preparations reserve unique generations even when later abandoned.

Tests cover 2/8/64 channels at both rates, independent directions, wraparound,
guarded failure silence, ordinary and aligned heap operations in audio methods,
concurrent frame publication, all 32 device slots, failed edit preservation and
mapping retirement. Two independent reader processes validate both device
timelines while each writes its own playback direction. Separate tests map
real XPC memory objects and verify aliases, Unicode metadata, mapping lifetime
and rejection of malformed snapshots. These establish local transport behavior,
not installed multichannel audio, authenticated service integration or latency.
Dynamic HAL objects and their format/rollback coordinator are implemented in
the separate preview driver below; authenticated service integration is next.

### Dynamic native driver

`driver/routing` separates the native interface, properties, device/IO state
and configuration transactions. Its `LoopZRoutingAudio.driver` bundle is built separately and
excluded from the installer. It consumes decoded device transport snapshots
through an internal entry point; no public writable configuration property or
unauthenticated service is exposed. The authenticated broker connection remains
to be implemented.

The driver publishes up to 32 device objects with independent clocks, stable
UID translation, paired channels through 64 and an optional public output
stream. Names can change while IO continues. Device removal notifies the host
outside control locks, preserves queryable removal state during those notices
and silences old callbacks until attached clients unregister. Re-enable assigns
new native object IDs while retaining the configured UID. Native IDs are never
reused during the driver process lifetime.

Audio callbacks borrow an immutable table of device records with bounded lookup;
reference counting, table allocation and reclamation stay on control threads.
The table history and removed-device retention are bounded. Device clocks have
coherent, bounded snapshot reads, and starting another client on an already
running device preserves its clock. The preview uses the SDK-compliant 16384
frame zero-timestamp period. This is a clock-reporting interval, not a measured
signal delay; it has not been latency-tested in the installed HAL. Its direct
transport has no added read queue or source alignment metadata yet.

Native tests load the actual bundle factory and exercise device discovery,
properties, 8/64-channel capture/playback, two readers, independent clock seeds,
100 renames during IO, safe deletion, re-enable, partial property buffers and
all 32 device slots. Host notifications re-enter property queries, including
the removed device's alive state. The same tests pass sanitizers. They use a
fake host and do not establish installed publication or client compatibility.

Changing an existing device's rate, channel count, output visibility or input
latency requests host clearance with `RequestDeviceConfigurationChange`. All forward,
rollback and failure-isolation catalogs are prepared before the first request.
Only the matching `PerformDeviceConfigurationChange` callback publishes each
device's new format. Registered clients remain attached through a shared device
lifecycle; clock seeds change without resetting unrelated device clocks. Audio
from an applied candidate stays muted until every requested change has passed.

Replacing only a shared-memory mapping does not change the public format. The
driver stages that replacement separately and publishes it when the entire
transaction commits, without a HAL configuration request or device-list notice.
It retains device/stream IDs, registered clients, and the clock origin and seed.
The clock is copied under the control mutex at commit, so a genuine client
stop/start during another device's pending format approval is preserved. Old
callbacks retain their mapping through the existing reader lifetime and return
silence after it is muted or retired. Losing the service closes the audio gate
without removing devices; restoring an unchanged graph can reopen that gate
without resetting clients' clocks. An aborted mixed transaction never publishes
its staged mapping replacements or names.

`artifacts/driver-handoff-proof.json` records isolated tests for repeated service
loss with active 2/8/64-channel clients, 48/44.1 kHz clocks, concurrent input
callbacks, and mixed mapping/format commit and rejection. The later existing-slot
update installed this driver. Actual Apple-hosted continuous-client Engine crash
recovery now passes with a privately staged signed Engine; evidence is
`artifacts/continuous-recovery-installed-proof.json`. Driver-host loss and
physical-source/monitor recovery remain separate acceptance gates.

Submissions have explicit Pending, RollingBack, Committed, Aborted and
Quarantined results, with a bounded history of 32 outcomes. The controller waits
for Committed before calling `DeviceTransportPool::commit`. Host requests run
on a serial control queue; a separate supervisor enforces a ten-second deadline
for each approval, including rollback. Stale action tokens are rejected, and
late or failed requests cannot revive an aborted transaction. Cancellation
starts rollback and cannot interrupt an already-running rollback.

If a later approval fails, prior format changes roll back through host clearance
in reverse order. If rollback itself is rejected or times out, only the remaining
unsafe devices are withdrawn and their mappings retired to silence. Already
restored and unrelated devices continue. Recreating a withdrawn device uses a
new native object with the same configured UID; retired same-format mappings
are replaced by the pool. The supervisor also reclaims unused tables after
readers finish, without requiring another configuration edit.

Tests cover deferred and inline approvals, cancellation, stale callbacks,
submission failures, forward and rollback timeouts, partial rollback isolation
and recovery. A 32-device update passes all approvals; another approves 31,
rejects the last and restores all 31 in reverse order. An independent callback
keeps reading valid audio while other devices change format and roll back.
Unchanged native descriptions avoid table swaps and host notifications.

These remain fake-host tests. Authenticated broker connection/recovery and
client-initiated rate forwarding and derived input latency now have isolated
fixtures. Actual installed format/timing transitions remain to be verified.
The preview is not part of the production installer yet.

## Clocks and conversion

The macOS backend converts Mach absolute time to nanosecond anchors before
passing samples into the portable core. Its virtual device has its own clock
at 44.1 or 48 kHz. Each source retains its sample counter and host timestamp;
the core maps requested host time into source samples and measures rate to
account for long-term drift. The default helper lookback is 2048 frames and
the default independent driver read queue is 1024 frames. The controlled
app-only experiment uses a 192-frame lookback and zero additional read queue.
Requested frame counts are distinct from measured signal latency; current
measurements and exact settings are in [the latency report](latency-comparison.md).

Each hardware monitor uses its own callback host timestamp and rate. It reads
the published virtual timeline through interpolation, with an additional queue
margin. No monitor clock drives the virtual device. No aggregate drift
compensation is applied on top of this explicit conversion.

The stereo capture adapter and multichannel histories now share portable
bandlimited conversion with offline spectral tests. Application capture in the
full document runtime also uses this stereo adapter. Release readiness across
physical clocks remains a separate gate.
A source clock discontinuity invalidates the reader generation and supplies
silence for missing history. It no longer permanently fails capture because a
cumulative discontinuity counter became nonzero. The current helper rebuilds
its renderer automatically for virtual rate changes, retaining a healthy bridge
mapping throughout the transition. Runtime multi-device/channel transactions
still require the persistent protocol-2 controller.

## Sources and safety

Application capture selects a bundle identity. The document runtime enumerates
all output devices and streams for that identity, with at most 32 capture
streams and 64 numbered channels per device. Each eligible float32 stream uses
its own native tap, clock/history and prepared channel plan. Numbered pairs fold to stereo
at unity through SingZ's DSP graph; mono is duplicated. For speaker layouts,
Audio Format Services supplies a standard input-major stereo matrix on the
control thread. Its nonzero coefficients become ordinary portable channel
connections before callbacks start. The render callback performs no format
query, allocation or automatic normalization. Standard 5.1 and 7.1 front channels
retain unity; center/surround weights and LFE omission follow Apple's matrix.
Per-stream plans slice the device-wide speaker matrix using the stream's
starting channel. The portable capture combiner reads all histories at the
same host timestamp and sums at unity. Per-frame coverage counts the union of
missing support while retaining valid contributions; zero-valued audio is valid
silence. Clock-anchor caches reset with a newly published capture group.
Unsupported layouts/coefficients fail source acquisition with a retryable
diagnostic in the document runtime. The legacy stereo compatibility mode still
has a system mixdown fallback, whose timing and gain remain an acceptance gap.
Unknown/failed output queries never become an empty route. On macOS 26, Core Audio can restore processes by bundle ID. The helper also polls audio process identities and rebinds after exit/relaunch
on older systems; actual macOS 14.2 verification remains a compatibility gate. Browser/helper-process coverage must be
verified; a single process is not assumed to represent an entire application.

The workspace selector now separates portable membership policy
(`application_processes.cpp`) from macOS process ownership collection
(`application_ownership.mm`). A process can match an exact bundle identity, a
kernel executable path contained in the selected running app bundle, or a
verified responsible process instance. Bundle-name prefixes never imply
membership. PID plus kernel start generation identifies each instance; capture
health compares those instances as well as Core Audio object IDs. Ownership
evidence for selected processes is rechecked before publishing a snapshot, so
unrelated process churn does not itself invalidate a healthy selection. LoopZ
Engine processes are excluded from expanded parent cohorts.

Responsible-process lookup uses an optional, undocumented libquarantine SPI,
loaded only from the system library. It is isolated in the macOS adapter;
unavailable lookup preserves exact/contained matches but cannot establish
ownership of shared system helpers. This remains a compatibility limitation,
not a portable API guarantee. No process bundle contents are read and selection
does not create taps, open devices or request recording permission.

`artifacts/app-ownership-proof.json` records metadata-only checks on this host:
Chrome's audio helpers are attributed to Chrome, while Zoom's shared WebKit
process is attributed to Zoom and excluded from Safari. These checks establish
selection, not successful audio capture, muting, helper relaunch continuity or
minimum-OS compatibility. Generated-signal parent/helper capture acceptance
remains required. This workspace change is separate from the frozen, pending
administrator driver update.

The owned parent/helper fixture in
`tests/platform/macos/application_family_fixture.mm` now exercises real nested
app bundles with distinct identifiers. Its metadata-only mode exposed a path
alias mismatch: AppKit retained `/var/...` while libproc returned
`/private/var/...`. The collector now canonicalizes the executable and bundle
paths before comparing them; the process-instance checks remain required.
`artifacts/app-family-proof.json` records parent stability, helper selection,
helper stop/restart/new generation and cleanup without opening audio IO.
The fixture's separate audio mode is restricted to two verified LoopZ virtual
outputs and gives each helper a distinct tone pair. That mode has been compiled
but has not yet established captured signal, mute behavior or latency.
Original playback muting is opt-in. One source session owns its mute policy;
conflicting route-level mute requests will not be implemented independently.

Microphone capture duplicates mono to stereo, or selects an interface's first
two channels. Capture is never started by inventory or test discovery.
Automatic tests do not play through speakers. `--monitor-muted` exercises
hardware output while writing zeros after reading the mix. Direct self-capture
and monitoring into LoopZ are rejected. External application monitor loops
need a separate user-facing policy; a limiter does not prevent feedback.

## Sources

- [Apple process taps](https://developer.apple.com/documentation/coreaudio/capturing-system-audio-with-core-audio-taps)
- [Apple virtual driver sample](https://developer.apple.com/documentation/coreaudio/creating-an-audio-server-driver-plug-in)
- [Apple driver sandbox and Mach services](https://developer.apple.com/library/archive/qa/qa1811/_index.html)
- Pinned SingZ ADR 0003 in the source checkout governs clock-domain separation.

### Realtime latency revision

The macOS virtual graph runs on a dedicated time-constrained thread with an
independent Audio Work Interval. HAL health queries and telemetry stay on the
control thread. Default rendering uses 256 frames; experimental 64/128-frame
calls require app-only device-stream capture. The maximum prepared zdsp block
stays 256 frames and capture history still accepts 4096-frame callbacks.
Driver metadata follows the selected source lookback while its independent
read queue keeps its existing 256-frame grid. Smaller app-only blocks compute
32 frames before their output timestamp; default blocks use a 64-frame lead.
The publication deadline is the current block's first output timestamp, and
interrupted waits recheck the clock before reading source history.

The minimum source margin uses the latest coherent clock snapshot available
to that reader. Reusing an older anchor can underestimate available history;
zero missing source frames and positive publication headroom are separately
required. A narrow positive margin is experimental evidence, not a robust
default or a substitute for the sustained validation gate.

A render reader owns its last coherent source-clock snapshot. It makes only two
publication attempts and can reuse that snapshot during a normal capture
append; the history remains indexed in the source sample clock. A generation
change invalidates the cached snapshot before discontinuity samples are
published, and is checked again after reading the block. There are no render
locks, logging calls, HAL queries or heap allocations.


## Desired graph and automatic recovery

`GraphRecovery` in the portable core holds the entire desired connection DAG.
Availability and retry state are separate from intent: failure never deletes a
node or rewrites the saved document. Backends define resource prerequisites;
optional app/hardware inputs must not be dependencies that stop an otherwise
healthy virtual render/output. Dependent resources are invalidated together
and released in reverse dependency order; independent resources remain ready.

Each acquisition has a unique token. Lost prerequisites, replaced intent, timed
out attempts and duplicate completions cannot publish stale connections. The
backend must dispose of any acquisition whose completion is rejected. Pending
attempts expire after 10 seconds of monotonic control time; this is an
acceptance deadline, not a mechanism to interrupt a blocking OS call. Retries
continue indefinitely at 100, 200, 400, 800, 1600, 3200 and at most 5000 ms.
Polling can add scheduling delay. Five seconds of health resets escalation;
brief flapping preserves it. Inventory/wake notifications can accelerate
waiting retries without duplicating in-flight work. Replacing desired intent
requires the owner to release prior resources before submitting the new DAG.

The document Engine now connects this retry policy to macOS workspace wake,
session activation, app launch/exit, audio process inventory, device inventory
and default-device notifications. `RecoveryEvents` owns the platform listeners
outside the portable graph. Callbacks only set an independent atomic mailbox;
the control loop performs retries. Inventory bursts coalesce to at most four
deliveries per second; wake and audio-service resets bypass the delay. Missing
audio listener registrations retry every five seconds, with normal runtime
polling retained as the fallback. Diagnostic status reports registration count
and errors. Installation validation does not construct these observers.

An audio-service reset is different from an endpoint arrival: the Engine joins
rendering, closes captures/monitors, invalidates its authenticated connection and
reconstructs the same desired document against the new service session. It does
not signal or restart Core Audio. The observer re-establishes its own listeners.
A reset arriving during batch consumption or reattachment remains actionable;
queued callbacks retain only their mailbox and cannot use a destroyed runtime.
This follows the installed Apple SDK's `kAudioHardwarePropertyServiceRestarted`
contract that clients must re-establish cached state and listeners. Workspace
wake subscription uses the [documented workspace notification center](https://developer.apple.com/documentation/appkit/nsworkspace/didwakenotification).

`artifacts/recovery-events-proof.json` separates injected reset/lifecycle and
concurrency tests from actual six-listener registration on this Mac. Runtime
fixtures verify unchanged healthy acquisitions after ordinary retry events and
complete saved-intent restoration after repeated simulated service resets.
Optimized, address/undefined and thread-instrumented checks pass. Real sleep,
unplug, audio-service reset delivery and deployed continuous-client recovery
remain unverified. No system service was restarted for these checks, and this
workspace change is outside the frozen pending administrator package.

The current stereo helper uses a dedicated macOS `EngineConnections` owner:
application capture, microphone capture and monitor output have independent
owners/retry states. Missing sources contribute zeros, while existing sources,
pass-through and the virtual renderer continue. Capture buffers and callback
contexts remain alive across reopening; format/selection data change only
after the affected HAL producer has stopped. Healthy checks cover app process
identity, app output route/format, producing-app callback stalls, microphone
UID/default selection, device liveness/rate, microphone authorization/stalls,
and monitor state/format/stalls. An idle application's lack of callbacks alone
is not a failure. Microphone recovery only checks existing authorization;
requesting permission remains an explicit setup command.

A bridge interruption recreates the connection and the whole requested stereo
session. Virtual rate changes and missed render-clock deadlines reconstruct the
render session against the same live bridge rather than resetting the broker.
Render threads join and monitor callbacks stop before old mapping release.
A timed validation that ends with missing requested endpoints fails instead of
accepting a reduced graph as fully restored. The default unbounded run keeps
retrying until explicitly stopped.

The portable suite exercises 32 device subgraphs, reverse-order invalidation,
independent source survival, one hour of absent hardware, flapping, wake retry,
stale callbacks, deadlines and invalid desired changes. A separate test host
compiles the actual engine/control sources with a private fault-injecting
bridge. It tests failed attachments, a live 48-to-44.1 kHz change, connection
loss, a forced process scheduling gap and three permanently missing endpoints.
It never registers a service or publishes audio to installed devices.

This code is not yet deployed. Physical unplug/replug, real sleep/wake,
permission revocation and installed-driver interruption require installed
validation after the current stability run. Restarting the helper after its
process dies still requires persistent background supervision. The configuration
controller below connects saved document intent to native transaction outcomes;
authenticated service wiring and renderer/capture publication remain separate
gates for full audio-graph recovery.


## Configuration restoration controller

`ConfigurationController` is the control-thread owner of desired and last
acknowledged documents, per-device transport ownership and one outstanding
native configuration transaction. It loads a saved document (including all
disabled devices, sources, controls, monitors and virtual dependencies), prepares
the entire enabled device snapshot, submits it, and commits the transport pool
only after an exact `Committed` acknowledgement. A controller with no desired
document never submits an implicit empty snapshot that could delete live devices.

An explicit save validates the complete document and backend formats, persists
it atomically, then replaces in-memory intent. A failed save or invalid edit
leaves the prior intent intact. Identical canonical documents are no-ops.
Reload reports backup recovery and preserves unknown future versions, including
future schemas with different top-level fields; their version is checked before
applying the supported version's field rules. Runtime failure never writes a partial graph
to disk or changes desired intent.

A newer edit requests cancellation of an older transaction and waits for its
terminal phase. Rejected cancellation is retried. A racing older commit is
recorded only as observed state, never as the current desired document, and the
newer intent is submitted next. Rejection, rollback and quarantine retain the
full desired document and retry indefinitely with bounded backoff. Retired
active mappings also trigger restoration without another editor command; only
changed or retired mappings are recreated.

The driver link has an ordered, exclusive-session contract. Every authenticated
connection incarnation has a distinct nonzero identity. Replies are checked
against that identity after each call, so an old session cannot commit into a
replacement. Reconnection first queries the driver's latest revision and settles
any predecessor's pending approval before reasserting the desired graph with a
newer revision. A thrown submit is uncertain rather than a rejection: staged
mappings remain pinned until ordered status resolves whether it was accepted.
Transient query failures neither release those mappings nor start overlapping
submissions. Unexplained missing history invalidates the session. Losing a
session retires tentative mappings while preserving healthy active mappings.

The coordinator uses the native pool and native driver's actual transaction
entry points in fake-host integration tests. These cover host rejection followed
by automatic retry, superseded edits/stale approvals, failed rollback with
quarantine, same-mapping output visibility changes, format changes, continuous
unrelated IO, and controller reconstruction while clients stay attached. Unit
tests additionally cover a complete 32-device desired document, disconnected
sessions, uncertain replies, failed cancellation, corrupted/future files and
failed persistence. Core owner-reset/disconnect transitions allocate nothing
and preserve token uniqueness.

The authenticated XPC adapter below now implements `DriverConfigurationLink`.
The optional `GraphActivation` owner now prepares a complete audio scene before
native submission and publishes it only after that exact revision commits.
Preparation failure retries without contacting the driver. Rejection, rollback,
quarantine and session loss discard tentative scenes without publishing them.
An uncertain submit keeps its prepared scene pinned until ordered status resolves
it. A restored native configuration and DSP scene do not prove that every
external source or monitor is live. `EngineRuntime` now integrates the shared
capture owners and physical monitors; deployment and background supervision
remain separate gates.


## Complete scene rendering and recovery publication

`AudioScene` is a portable, prepared renderer for the complete device document.
It builds one SingZ zdsp routing graph per enabled device and renders the virtual
dependency DAG in topological order on one internal clock. A public client or
monitor sink requests a device; demand propagates upstream. A virtual source uses
the upstream block directly when origin delays match, or its retained render
history to compensate the difference. It does not repeat rate conversion or add
a fixed delay at each hop. Per-device nominal rates are endpoint concerns.
Undemanded graphs idle and clear meters. Disabled settings remain in the document.

An external provider remains bound while unavailable. Its missing frames become
silence, source status records the loss, and healthy inputs and unrelated devices
continue. Returning data resumes through the same graph and binding. A failed
public sink cannot prevent downstream virtual dependencies from using its DSP
output. Nonfinite source samples are zeroed. Source meters precede gain/mute;
device meters follow the graph and limiter. Meter publication uses atomic
payloads and bounded snapshot reads, independent of the render owner.

`RealtimePublication<T>` separates preparation, publication and reclamation.
One control owner stages an update and reserves a bounded retirement slot before
any native mutation. Borrowing only changes a lock-free reader count and loads a
pointer. Commit publishes a prepared pointer without allocation or reclaiming an
old object. Only the control owner reclaims retired objects, after old readers
have left. A stalled reader applies backpressure before a new native request;
it cannot cause unbounded retirement, audio-thread destruction or early unmapping.
The owner outlives staged updates and joins every reader before destruction.
Only one audio caller may render mutable scene/sink/cursor state.

The macOS `SceneRenderer` supplies transport-backed pass-through sources and
capture sinks, with independent non-destructive readers and heartbeat/retirement
checks. It keeps output filter history for an unchanged mapping across
graph replacements. Output endpoint conversion uses integer clock-domain
boundaries and a prepared portable causal converter, preserving contiguous
timestamps across blocks. Matched rates retain one internal-frame delay;
converted rates include the complete filter support in their delay. Missing
playback frames silence their output; stale or retired mappings cannot revive by
rewriting online flags. The scheduler supplies current host time once per block.
No clock, HAL, XPC, allocation or logging calls run inside these adapters.

Public input reads now use the portable `AudioResampler`, shared with capture
and monitor histories. Both capture and playback directions use the same
bandlimited kernel, bounded tile storage, finite containment and missing-frame
rules. The macOS adapter pins its mapping and supplies one-frame reads from the
authenticated transport; lease and retirement checks remain at that boundary.
It rechecks availability after conversion so a retired or expired mapping cannot
publish the earlier part of a now-invalid block. The converter's clock snapshot
and scratch state belong to that call; no shared mutable resampling cursor is
introduced.

`AudioResampler` also supports strided output for interleaved buffers, with
bounded frame counts and zeroed unavailable channel tails. Read requests do not
shift timestamps or change configured queues. Converted public inputs require
filter history on both sides of the requested time, so minimum live queue
profiles still require validation. `artifacts/public-input-proof.json` records
native spectral, direction, width, underflow and processing-cost checks, plus
the shared-reader, graph recovery and sanitizer results. These are private
memory/fake-host fixtures; no installed or physical IO is established by them.
Public output now uses `CausalResampler`, a serial render owner with bounded
interleaved history and output storage allocated during preparation. It reuses
`AudioResampler` and zero-pads a new stream before its first input, preserving
the initial impulse even at absolute frame zero. Discontinuities and idle reset
the history. Its source-clock delay is one frame for matched rates, otherwise
`ceil(96 / (0.94 * min(1, endpointRate/renderRate))) + 1` frames. This prevents
future-input reads. The delay is included in the native metadata below; it is
113 frames (2.354 ms) at 48-to-44.1 kHz and 104 frames (2.358 ms) in reverse.
`artifacts/causal-output-proof.json` records offline evidence; it is not installed.
The renderer's sink decorator is the boundary for monitor ownership. The portable
monitor bus decorator described below establishes demand for configured monitors;
it does not open hardware. Without that decorator an enabled monitor in the
saved document alone does not acquire an endpoint.
Source providers manage actual device/tap acquisition and recovery off the
audio thread. The full routing driver and document Engine passed existing-slot
activation and multichannel IO (`artifacts/installed-routing-io-proof.json`).
Later scheduling, background and history-conversion candidates have separate
deployment evidence and remaining gates; fresh-slot discovery is unresolved.

### Mix timing compensation

Every enabled mix uses the greatest configured input delay, including its
virtual dependencies. At render frame `T`, external and pass-through readers
request source time `T - L`, where `L` is that mix's delay in internal frames.
A virtual input whose upstream delay is `U` reads the upstream output at
`T - (L - U)`. Equal-delay virtual links still use the current upstream block.
This gives the inputs to a mix a common origin without accumulating another
source delay at every virtual hop. Mute, gain and temporary provider loss do not
change the configured delay or move healthy branches to a different origin.

The serial render owner alone accesses compensation samples. A history is
allocated only for an upstream device that has a consumer with greater delay.
Each history reserves 16,640 planar frames per channel: the maximum 16,384-frame
compensation plus the largest render block. It uses contiguous interval bounds
and two bounded copies per plane across a ring wrap, without locks, allocation,
OS work or shared-pointer operations in the callback. Missing history and times
before frame zero produce silence. Idle periods, invalid blocks and scheduling
discontinuities invalidate the available interval, preventing stale replay.

`SceneHistoryStore` shares matching histories across prepared scenes on the
same serial render owner. Its key includes device ID, channels, internal rate
and upstream origin delay. Preparation and rejected candidates never mutate
the active history; ordinary edits retain queued samples. A longer consumer can
use the already reserved history without reallocating an unchanged producer.
Changing that producer's clock/layout/origin creates a separate history and
primes it before recovery is considered complete. The cache holds weak entries;
ownership and reclamation stay with prepared/active/retired scenes on control.

`AudioScene::timingReady()` reports whether demanded compensation intervals are
available. `EngineRuntime` waits for it and a fresh block from the current render
owner before reporting restoration, including after stop/start recovery. It does
not certify external capture availability, non-silent content or acoustic phase;
those remain separate provider and live-measurement checks.

`artifacts/path-timing-proof.json` records the current isolated evidence:
correlated clock signals over mixed paths and virtual chains, partial blocks,
2/8/64 channels, both public-rate clock examples, ring wraps, maximum delay,
underflow, scheduling/demand discontinuities, provider return, prepared/aborted
edits and native renderer staging. Heap tracking and sanitizer results cover the
callback/state ownership contracts. This is configured timestamp alignment,
not a latency reduction measurement. Standard HAL latency metadata now accounts
for the graph and public converter, as described below. Physical/tap timestamp
biases and actual host scheduling still need validation before installed claims.

### Core Audio input latency metadata

`planSceneTiming` derives the DAG render order and per-device internal delay
without opening providers or allocating audio histories. Both `AudioScene` and
the macOS renderer use this plan. For an enabled device with delay `L`, the
transport reports `ceil((L + D) * endpointRate / renderRate)` input frames, where
`D` is the prepared causal converter delay in render frames. Matched-rate output
keeps `D = 1`; converted output includes its full filter support. Rounding upward
avoids understating a fractional endpoint frame. Enabled empty
or pass-free graphs still account for that converter delay; disabled devices
have no public endpoint. Source availability, mute and gain do not change delay.

The value appears once, as input-scoped `kAudioDevicePropertyLatency`. Stream
latency is zero, because Core Audio adds device and stream delays. Output
latency and safety offsets remain zero: this metadata describes signal origin,
not an extra shift of the driver's timestamp-to-ring lookup. Reported processing
delay is distinct from host scheduling, a monitor's hardware queue and measured
capture/acoustic latency. The property is read-only and uses the endpoint's
sample rate, rather than the internal render rate.

The controller requests timing metadata before preparing mappings. The renderer
rejects a prepared descriptor that does not match its plan. A changed input
delay receives a fresh region, even if rate and channel count are unchanged,
so old-origin samples cannot be exposed with new metadata. Unchanged devices
retain their mappings. The driver also rejects a delay change that aliases the
old region. A timing change uses the existing host approval/rollback coordinator;
the new mapping remains configuration-muted until the full native transaction
commits. The helper publishes the matching DSP scene after that acknowledgement.
Each approved forward or rollback step sends an input-scoped latency notification
outside the native state mutex, allowing reentrant host property queries.

Snapshot schema 3 adds the mandatory, bounded `inputLatencyFrames` descriptor.
This is separate from the unchanged protocol-2 shared-memory header and service
envelope. Unknown/old snapshot schemas, missing/wrong-type metadata and values
above 98,928 endpoint frames are rejected. That bound covers the maximum
16,384-frame lookback plus the 104-frame upsampling converter delay at 8 kHz,
rescaled to 48 kHz. This relaxed bound requires the matching service/driver
candidate for extreme timing profiles; the installed package retains its prior
bound and has not been changed.
All three new service/driver/helper pieces must be deployed together.

`artifacts/hal-latency-proof.json` records the implementation checks. They cover
clock conversion, full-scene/native metadata agreement through authenticated XPC,
mapping isolation, deferred approval, rejection, rollback, notifications and an
unaffected concurrent reader. These fixtures use a simulated host. Real installed
client scheduling, DAW compensation and matched acoustic/Loopback measurements
remain acceptance work; metadata alone does not reduce physical latency.

### Independent monitor rendering

`MonitorBuses` prepares a shared common-clock `MonitorBus` for each device ID and
logical output layout. It retains the same bus through public sample-rate changes,
transport replacement and gain/wiring edits while a scene owns it. Creating or
discarding a prepared sink does not stop the committed bus. A changed channel
layout gets separate history, so an old reader cannot interpret a new layout.

`MonitoringSink` publishes the post-device mix to both public capture and that
history. A public capture failure cannot suppress monitor publication. Any enabled
monitor requests the graph, including a muted monitor or one whose physical device
is temporarily missing. This demand propagates through virtual-device dependencies
and keeps capture desired until the monitor is explicitly disabled. An undemanded
scene idles its bus and invalidates history; a returning producer cannot expose
samples from before that stop.

Each `MonitorProgram` owns an actual SingZ zdsp routing graph, its own read cursor,
monitor gain/mute/wiring and post-control meter. Hardware callbacks read the shared
history at their presentation time minus an explicit internal-frame queue. They
may use different output rates; neither monitor consumes another's data or sets
the virtual render clock. Callbacks larger than the 256-frame DSP quantum are split
with timestamp offsets, without allocation. A narrow physical profile preserves
available outputs, zeros unavailable logical meter channels and retains the saved
map for the full profile's return. Extra physical output planes are zeroed.

Missing history produces a valid silent callback with missing-frame telemetry,
rather than a hardware failure that would repeatedly reopen an otherwise healthy
device. Invalid callback shapes, clock rates or timestamp arithmetic fail to
silence. `AudioMeter` now provides the same bounded, atomic snapshot publication
for scene and monitor meters. A monitor snapshot covers its last DSP chunk, rather
than claiming a callback-wide RMS aggregate.

Portable tests cover two independent monitor clocks at 48/44.1 kHz, changing-signal
interpolation across chunk boundaries, 64 logical channels, 8192-frame callbacks,
partial profiles, history stop/restart, aborted preparation and public sink failure.
Concurrent producer/readers and meter observers run with ordinary heap tracking
and under address/undefined/thread sanitizers. These core checks are offline DSP
and lifecycle evidence; physical output ownership has the separate checks below.
The monitor bus now uses the bandlimited `AudioHistory` reader described below.
Installed runtime validation, changing-clock quality and actual audible monitor
latency remain required.
The initial monitor queue defaults to 1024 internal frames in addition to the
scene's source lookback; it is not evidence of a 3–5 ms physical path.

### Physical monitor ownership and committed activation

`MonitorOutputs` supplies the renderer's sink decorator. Each enabled monitor has
an independently owned SingZ `AudioHost`, keyed by device ID, monitor ID and saved
output selector. Construction and scene preparation acquire no hardware. A sink
reserves and prepares a monitor slot before the native transaction is submitted;
`AudioScene::activate()` publishes those slots immediately before the acknowledged
scene is published. Rejected or abandoned preparation cannot affect the old gain,
wiring, output unit or source history. Committed update objects may remain alive
without blocking later preparation or releasing a newer reservation.

Hardware callbacks borrow the active slot and its prepared `MonitorProgram`
through bounded realtime publication. No shared ownership changes, graph creation,
device enumeration, locks or reclamation run in those callbacks. Separate program
publication within a slot lets control reprepare for a physical profile change
even while a different slot is awaiting native approval. A changed program
prepared at a now-stale rate supplies silence until control corrects its format;
an identical pending selection shares the live program and follows that recovery
without restoring stale DSP. A healthy output unit need not reopen for either
correction. Gain, mute and wiring edits retain that unit as well.

Only monitors in the committed scene acquire outputs. Opens are shared,
output-only, with empty input selection and zero requested sample rate/buffer
period. They cannot acquire microphone permission or change a shared period.
The negotiated channel count can be narrower than the saved layout; connected
and complete are separate observations. Default-output changes, missing UIDs,
rate/channel changes, provider failure, invalid timestamp callbacks and two-second
callback stalls close only the affected unit. Each saved route retries forever
with the bounded recovery backoff. Self-monitoring into either LoopZ UID family is
rejected, including when the default changes to LoopZ. Admission follows successful
deadline validation; callbacks during a start that later expires stay silent.
Stop withdraws admission and joins the provider's callbacks before reclamation.

Physical output queues have an explicit floor plus a period-derived minimum:
the larger of the requested lookback and two negotiated output periods converted
to internal frames plus the 256-frame graph quantum. The actual queue is reported
per monitor. At the tested 512-frame periods this is 1280 internal frames for the
48 kHz output and 1371 for the 44.1 kHz output. This conservative initial scheduling
margin is not an acoustic latency measurement or a low-latency tuning result.

Injected provider tests exercise simultaneous 48/44.1 kHz outputs, independent
failure/retry, default/profile changes, partial layouts, mute/disable, expired
acquisition, stalled/invalid callbacks, rejected edits and concurrent publication.
The renderer integration also verifies that pending configuration cannot open
hardware and that repeated committed gain edits leave the healthy unit open.
These fixtures run under address/undefined/thread sanitizers.

`loopz-monitor-probe` separately exercises real, always-muted output-only streams.
Its render thread is independent from control/device polling. The 12-second check
on Zen Quadro and built-in speakers passed with zero missing frames, render
failures, reanchors or late render blocks. Rates, periods, default device snapshots
and the Core Audio PID remained unchanged. Earlier single-thread probe failures
are retained in the evidence; they exposed the need to isolate render scheduling
and account for each output period. See `artifacts/monitor-ownership-proof.json`.
Actual unplug/sleep/wake, audible content, physical latency, sustained new-runtime
operation and integration into the installed persistent engine remain open gates.

Isolated tests exercise 32 devices/64 channels, reverse document order, demand
propagation, missing/recovered inputs, malformed providers, failure silence,
real meters, concurrent observations, cursor continuity, publication backpressure
and zero ordinary heap activity during rendering/borrowing. macOS tests cover
2/8/64-channel endpoint conversion at both public rates, integer boundary coverage,
stale leases, sticky retirement and independent readers. A controller integration
test checks preparation failure, rollback, stale-session replies, unchanged
mapping preservation and automatic restoration after retirement. An authenticated
XPC/fake-host test sends actual native playback through the prepared zdsp scene
and back through native recording callbacks before/after format change and
service disconnection. These are code/fixture proofs, not installed HAL,
physical-clock, microphone or acoustic latency measurements.


## Shared capture sources and multichannel input history

`SourceRegistry` owns control-domain acquisition policy independently of the
saved document and the render graph. Preparation plans each external identity's
largest logical channel layout across the entire document before resolving any
one route. Application/input identities share a provider across devices; each
logical binding has its own clock cursor. A provider cannot be aliased across
independently owned identities. Factories and graph preparation allocate state
but do not open hardware or mute application playback.

The committed scene publishes a coherent 32-bit device-demand mask. The registry
uses it to acquire only active external sources, including dependencies of an
active downstream virtual device. A newly committed scene retains existing
capture until its first complete demand update, avoiding a transient close/unmute
between configuration acknowledgement and the next render block. While native
format approval is pending, demand from already-moved tentative mappings is
matched back to the committed device identities. It preserves only the existing
scene's captures; an uncommitted new source is never opened. Native callbacks
renew their client lease while configuration-muted, but public reads still return
silence and playback writes remain blocked until the full transaction commits.
Original app
playback mute is the OR of requests from active bindings sharing that tap. Source
mute does not replace the independent original-playback policy. The final user
releasing a source closes its capture; an unused prepared/abandoned scene never
opens it.

Every source has the portable retry coordinator's bounded indefinite backoff.
A failed open, throwing backend, failed health check or expired acquisition closes
only that provider. Admission uses an epoch rather than a boolean: a read spanning
a close and fast reopen is rejected even if availability is true again at its
end. The registry admits a newly opened source only after its recovery token and
deadline match. A connected but narrower hardware profile is distinct from full
restoration; required channels come from current demanded bindings, not the
largest retained allocation. Losing one source does not replace the scene or
interrupt healthy captures.

A healthy provider can accept a control-domain `updatePolicy` without changing
its stream or reader admission epoch. The macOS input owner already captures
all available channels within its prepared logical layout, so a second reader
increasing required channels does not need an AUHAL restart. It updates required
completeness while retaining the same history, including a narrower partial
hardware profile. Providers that cannot apply a policy return false without
side effects, and the registry performs its normal reopen. Failed health checks
and original-playback mute changes still follow source-local recovery.

`AudioHistory` provides a fixed 1–64-channel logical layout, a bounded atomic
sample history, per-frame publication tags, a copied measured source clock and
independent non-destructive readers. Present channels stay audible when a
hardware profile narrows; missing tail channels are zero and count as missing.
Old/overwritten or concurrently replaced frames fail to silence. A source-rate,
channel-profile, sample-position or host-time discontinuity changes the clock
generation. Stop invalidates cached clocks; a restart may reuse a hardware sample
position without exposing the previous generation. Finite negative hardware
sample positions are valid clock origins.

History reads now use a prepared Kaiser-windowed sinc kernel for rate conversion
and fractional clock positions. The windowed-sinc design follows SingZ's
`zcore/legacy/resample.cpp`, while keeping its ordinary-thread fixed-ratio vector
streamer out of realtime timestamp reads. The unmodified vendor snapshot remains
locked. One immutable process-wide table is prepared on control; callbacks use
table interpolation without trigonometry, heap operations or a shared reader
cursor. A 32-frame tile loads each atomic source frame once and accumulates all
output channels in 16 KiB of double-precision stack storage. Downsampling scales
the filter support with the rate ratio. Equal-rate, integer-aligned requests
retain the direct path, including nanosecond clock-quantization tolerance.

Clock-rate measurement now uses two overlapping half-second observations,
rather than the entire capture lifetime. After initial acquisition, the window
is roughly 0.5–1 second plus callback-period quantization, with constant producer
work and storage. A nominal-rate/profile change, stop or nonmonotonic host stamp
resets the estimator with the history generation. The reader admits measured
source rates within the existing 0.5% drift tolerance around supported endpoint
limits, so an 8 kHz clock running slightly slow or a 192 kHz clock running slightly
fast is not rejected. Nominal endpoint/output formats remain 8–192 kHz.

`artifacts/changing-clock-proof.json` records the old estimator's regression and
new settling checks: +200, −200 and +100 ppm steps at nominal 8/48/192 kHz,
through both multichannel history and the stereo adapter. With 10 ms input
callbacks, settled rate error is below 0.002 ppm by the tested 1.1-second point,
and settled stereo-tone SNR exceeds 99 dB. All sampled transition blocks remain
available and finite; those SNR figures exclude the settling intervals. The
recorded maximum sample error during startup or an abrupt transition is about 0.060 at
8 kHz, 0.012 at 48 kHz and 0.003 at 192 kHz for a 0.25-amplitude tone and a
1024-frame lookback. This improves bounded rate recovery but does not certify
phase continuity or release-quality ASRC through abrupt/continuously varying
clocks. Physical timestamp jitter, transition phase quality and actual low-delay
queues remain acceptance work. These checks neither open hardware nor change
source/monitor queues, signal timestamps or native latency metadata.


A subsequent candidate now maps historical host times with retained callback
anchors in portable `SampleClockHistory`. Each prepared history has at most
`capacityFrames + 1` clock records, covering even one-sample input callbacks.
The producer publishes one stamped atomic record per callback; readers locate
an interval with bounded binary search and never consume another reader's data.
Reset makes old snapshots unusable. Frame labels and absolute host times remain
integers, with fractional output-grid time retained separately.

A read splits at actual clock interval boundaries. Stable spans can coalesce
only after every intervening anchor agrees with a fitted clock within 1.01 ns;
validation is capped at 256 anchors. A real rate change or curvature that exceeds
that budget retains the original intervals. This preserves processing cost for
wide downsampling without averaging away the clock transition. Interval rates
within integer-nanosecond rounding of nominal retain aligned direct reads.
The newest unclosed interval still extrapolates from its prior interval (or
initial rate); it cannot know a future callback timestamp. Queues and native
latency metadata are unchanged. The clock record array adds approximately
0.75 MiB at the default 32,768-frame history capacity, excluding allocator and
owner overhead.

`artifacts/clock-phase-proof.json` records the regression and this newer evidence.
The previous roughly 0.060 maximum sample error in sampled 8 kHz clock steps is
now below 0.00001 in the tightened fixture. Six dense high-frequency clock-step
traces and a per-callback sinusoidally varying clock trace keep SNR above 80 dB,
maximum error below 0.0002 and sample-delta residual below 0.0003. These traces
include the transition rather than discarding a settling interval. Clock-ring
wraparound, old snapshots during publication, reset, partial underflow and long
integer timestamp precision also have fixtures and sanitizer checks.

The first per-interval implementation exceeded one output period in the
64-channel 192-to-8 kHz component benchmark. Stable-span validation/coalescing
corrected that regression; both nominal and +200 ppm warmed component timings
are preserved in the proof. These are computation-cost measurements, not
end-to-end latency or whole-graph deadline guarantees. This historical-source
candidate did not yet estimate the physical output clock. The newer estimator
below addresses that path in offline checks; actual timestamp jitter, minimum
live queues and broad physical/installed acceptance remain open. This does not
establish a release-quality ASRC or a new Loopback comparison.

The newer output-clock candidate uses a portable `OutputClock` at each physical
monitor endpoint. Overlapping 25–50 ms hardware timestamp observations estimate
its actual rate; this is observation history, not an audio queue. Each callback
retains its actual first-sample host timestamp, and the monitor reads subsequent
samples at the measured rate rather than the negotiated nominal format. Both
conversion clocks admit the existing 0.5% drift bound, including at nominal
8/192 kHz. Native format negotiation and latency properties remain unchanged.

The estimator belongs to the endpoint, so a committed wiring/gain edit preserves
it. Stop/reopen, source-frame gaps, nonmonotonic timestamps, explicit callback
discontinuities and stream-generation changes reset it. Scheduler arrival times
are not used to estimate a hardware clock: a non-hardware timestamp falls back
to the nominal rate and resets the estimate. The monitor status and diagnostic
probe expose the rate actually supplied to the reader. Constant state and work
require no callback allocation or additional audio delay.

`artifacts/output-clock-proof.json` records synthetic source-history/monitor
comparisons, quantized timestamp and clock reversal fixtures, and native fake
backend lifecycle checks. Audio quality for a settled fixed drift is measured
separately from rate-estimator settling and reset behavior. Actual monitor
hardware, abrupt-clock audio transients, noisy hardware timestamps and minimum
live queues remain acceptance work; no new Loopback latency result follows from
these workspace tests.

A signed Release workspace candidate now also has live evidence in
`artifacts/output-clock-live-proof.json`. Two simultaneous, always-muted outputs
at 48/44.1 kHz and a separate all-16-channel Antelope output check each ran for
12 seconds with zero missing frames or render errors. Hardware timestamp
progression supplies the measured output rates. Existing output profiles,
defaults, installed binaries and the Core Audio process were preserved. These
silent checks establish stream/timing coverage, not audible phase quality or
physical signal latency. Their monitor queues were 1280/1371 internal frames;
minimum physical-monitor queues are still open.

The same signed Engine's newer conversion code ran three concurrent virtual
clients for 30 seconds at 2/8/64 channels, including the 8-channel virtual chain.
The explicit 64-frame graph/client, 128-frame pass lookback and 64-frame render
lead measured 4.75/4.75/5.25 ms marker delays, with zero silent samples after
warmup, timestamp/render failures or graph late/skipped/reanchor increments.
These are virtual timestamp markers, not app capture or acoustic latency.
A transient catalog read failed during removal in the first harness run;
independent cleanup was confirmed, and a bounded fail-closed snapshot retry plus
a subsequent three-second lifecycle check passed. Original reports are retained.
The subsequent driver update and continuous-client Engine crash checks now
close the mapping-handoff acceptance gap for these virtual routes; see
`artifacts/continuous-recovery-installed-proof.json`.

The symmetric filter preserves the requested signal timestamp. Its support is
`96 / (0.94 * min(1, outputRate/sourceRate))` source frames on either side, rounded
upward. Callers must have that history available; insufficient support produces
reported missing audio rather than a hidden delay or a linear fallback. Existing
source and monitor queues provide lookback, but their smallest usable profiles
need live validation with callback scheduling and source acquisition. Partial
profiles preserve available planes, old generations remain silent and extreme
finite input cannot overflow the float output. Sinc ringing around steps is
allowed; channel coherence and spectral checks replace linear convexity bounds.

`artifacts/sinc-history-proof.json` records passband/alias tests, 2/8/64-channel
reads, fixed drift, long timestamps, callback partitioning, underflow/recovery,
heap tracking, sanitizer checks and offline processing cost. That history-only
candidate was followed by the shared public-input reader described above.
Public output uses the causal converter described above. Stereo `CaptureBuffer`
now delegates to `AudioHistory` rather than maintaining a second ring, clock
estimator and linear interpolator. Its legacy reader cache and diagnostic
counters remain available to application capture and the feasibility Engine.
Matched, aligned reads retain the direct path; converted reads require both
filter wings, with explicit missing-frame reporting and no timestamp shift.
The existing capture queues are unchanged.

`CaptureBuffer::stop()` invalidates its history generation and diagnostic anchor.
The macOS app-tap and stereo microphone owners close SingZ callback admission,
stop the stream and wait off RT for admitted callbacks before disposing resources
and resetting history. Reopening at the same host/sample timestamps cannot
expose the previous generation. The full-runtime multichannel input owner already
uses the same SingZ gate and portable history. No capture teardown work runs on
the graph render thread.

`artifacts/stereo-history-proof.json` records stereo passband/alias, reader
isolation, stop/restart, bounds, callback heap and concurrency checks. Its
spectral reference uses the actual integer-nanosecond callback anchor, including
its rounding, rather than an ideal unrounded sample clock. These conversion
candidates are not installed and do not establish physical-clock quality, a new
latency measurement or a full ASRC release claim.

The macOS adapters reuse the existing application tap and add a multichannel
input-only AUHAL owner. Input construction does not open a device or request
consent. Recovery checks existing microphone authorization and never requests it.
Explicit UIDs and a saved default-input binding are supported; default changes,
rate/channel changes, disappearance, permission loss, render errors and callback
stalls trigger source-local recovery. LoopZ UIDs are rejected as physical inputs;
internal links use the document DAG and its cycle checks. The new input owner
preserves the physical device's shared rate and buffer size. Its callback buffers
are prepared before start, and SingZ's callback admission gate drains producers
before any format/buffer teardown. Available hardware channels are captured in
physical order; the portable graph owns user channel selection and mono fan-out.

`SceneRenderer` can now stage against a shared registry and poll its committed
source owners off RT. The authenticated native-driver fixture mixes a managed
synthetic capture with actual public playback, verifies recording callbacks,
loses/restores that source while pass-through continues, changes format and
replaces its driver service. Portable fixtures cover multiple independent app
and input owners, shared mute policy, wider/narrower active bindings, stale reads,
clock drift, generation invalidation and partial channel capability. The macOS
construction test validates unopened 1/2/8/64-channel adapters and feedback guards.
The signed workspace candidate now has live AUHAL evidence below; permission
revocation, real browser/helper-process attribution, unplug/wake and deployed
source recovery still need live acceptance. Isolated multi-app tap ownership and
application exit/relaunch now have the live evidence below. The native fixture also
holds the second format approval after moving the first mapping, ages out the
initial lease, and verifies that a configuration-muted callback preserves capture
without exposing public audio or reopening the provider. These adapters are now
connected to `EngineRuntime`, but are not yet the normal installed helper path.


`artifacts/capture-live-proof.json` records real Antelope 16-channel and Quad
Cortex 8-channel input capture plus a shared Antelope stereo reader. The first
concurrent run exposed an unnecessary shared-source reopen when demand grew.
After the policy-retention fix, stereo ran for 20 seconds while the wide routes
were added for 12 seconds and removed. Antelope acquisition attempts remained
one, and stereo's missing-frame counter did not increase across either change.
All three physical histories had zero additional missing frames in ten observed
steady reports; graph late/skipped/reanchor and graph/sink failures were zero.

The optional `--status-levels true` CLI report reuses the editor's bounded
meter/counter snapshot on control. Audio samples are not recorded. All temporary
device outputs were muted and verified silent; capture levels were observed
before mute. The test used a 1024-frame source lookback (21.33 ms at 48 kHz),
not a minimum-latency profile. Startup underflows are retained separately from
after-warmup counters. This establishes callback/history and shared-owner
continuity, not an injected physical channel-isolation test, acoustic latency,
unplug/wake or long-session acceptance. Microphone access was reused without a
request; device profiles, defaults, installed files and Core Audio were preserved.

## Application startup and live capture

Before creating a new process tap, the macOS adapter now requires at least one
selected process with running output and a nonempty output route. An app can
publish its process object before its output clock is ready; creating a mixdown
tap at that point caused a slow initial acquisition and expired retry tokens in
an actual rehearsal. Deferring discovery avoids that clockless tap startup.
Already established taps can remain configured while an app pauses. Eligible
stereo streams retain the device-stream path. Discrete multichannel streams now
use native capture and a prepared portable stereo fold; supported speaker layouts
use a prepared standard stereo matrix. Output-layout changes are detected along
with stream-format changes and trigger the existing source-restoration path.
The document owner now discovers all selected output devices and streams and
publishes a bounded immutable capture group. Close joins its producers and
stops current histories; retired groups are reclaimed only on the control
thread after render borrowers release them. Membership, identity, stream,
format or layout loss enters the existing source retry path. Other applications
keep their independent owners and clocks.

`--routing-signal-check` is an opt-in, virtual-only Engine diagnostic. It reuses
SingZ's host, the verified LoopZ model/transport guard, existing microphone
status and the portable bounded streaming signal checker. It never requests a
grant, writes audio to a physical device or saves samples. Tone checks include
channel amplitude/crosstalk, host-time markers, queue/timestamp faults and exact
zero-sample detection at 48 kHz; silence checks verify original-playback mute.
Four seconds of fixed analysis storage support longer continuous readers.

`artifacts/app-live-proof.json` records two actual generator-app identities and
five virtual devices. The initial, failed run is retained: its long reader began
before capture was ready and included the slow startup. A guarded rehearsal with
an intentionally delayed 1.5-second output clock restored in about 3.5 seconds,
then passed 35 seconds of strict continuous capture through original-mute edits.
A separate lifecycle run passed 50 seconds from the unrelated app while the
first app's original mute was enabled/cleared and that app exited/relaunched.
Both shared captures became silent during absence and recovered fresh markers
from a new process ID without changing their saved routes. The unrelated source's
acquisition counter was unchanged after readiness. Affected-app mute edits still
replace that app's tap; these checks do not claim glitchless policy replacement
for the affected source.

These tests used stereo 48 kHz virtual output sinks, a 1024-frame source lookback
and 512-frame readers. App markers measured about 42–43 ms, at 1 ms search
resolution. They are not acoustic latency, a minimum app profile, an external
Loopback comparison or broad browser/helper/multichannel-mixdown acceptance.
Physical output streams were never opened. Installed files, existing device
profiles, defaults and the Core Audio process were preserved.

## Authenticated routing service

Protocol 2 now has a separately named broker (`loopz-routing-broker`, Mach
service `com.lexasoft.loopz.routing.bridge`), helper-side `RoutingDriverLink`
adapter and reconnecting driver-side `RoutingDriverService`. The installed
legacy protocol-1 support remains alongside it, while the existing HAL slot has
been migrated to protocol 2. Root broker startup requires a pinned
engine code requirement and the system launchd domain. The driver role is fixed
to Apple's concrete Core Audio host identities and the `_coreaudiod`/root UID
gate; the helper role requires the active console UID. The OR-listener pin is
narrowed to a concrete role after a no-memory hello, before attachment. There
is exactly one helper owner and one driver owner. Both clients also verify the
server's code requirement and UID.

Each pairing change increments a broker epoch. Each client's paired session
gets a never-reused local token, including after broker restarts or driver
replacement. Unpaired/error states publish zero immediately; late RPC replies
cannot resurrect a prior session. Ordered apply/status/cancel requests are
forwarded off the audio callback. The broker times out a relay after two
seconds; client waits are bounded at three seconds per request. A timeout has
an uncertain outcome, so the configuration controller retains tentative maps
and resolves acceptance with ordered status. Replies from old pairings are
rejected. A negative response also updates authenticated pairing metadata
before it reaches the controller.

The broker retains validated mapping leases, bounded to 128, and bounds peers
and outstanding relays to 16 each. A timed-out request retains its slot until
the driver replies or its connection dies, even if the helper is replaced;
repeated timeouts cannot grow an unbounded driver queue. A flood test holds
driver processing, times out 16 requests, checks prompt rejection of the next,
and verifies recovery after the driver resumes. Broker events and replies enter
one explicit serial dispatch queue before any state access. Different devices cannot share the same
mapping identity; identity reassignment to another device is rejected. Helper
loss or console change irreversibly retires every mapping in its lease and
clears online flags. Driver loss preserves healthy helper mappings for reuse.
Retired leases are pruned on the control queue. No global audio-service restart
or default-device mutation is involved.

The preview driver's normal Apple-host initialization starts asynchronous
service connection. A global lock-free availability gate silences reads and
stops playback writes while unpaired. Native registered objects and client
lifetimes survive reconnection, and ordinary per-device format approval still
applies. Service callbacks and trust loading run on control queues. A factory
loaded by a non-Apple test/tool does not start the production Mach connection;
its internal control entry points act only inside that process.

The helper's installed endpoint pin is read from
`/Library/Application Support/LoopZ/routing-service.json`, with fields
`protocol: 2` and `brokerRequirement`. Every directory component and the file
must be root-owned, non-symlinked and not group/world writable. Missing or
untrusted metadata fails closed. The driver uses its own sealed bundle resource
instead of this external file, because Apple's documented audio-host sandbox
limits file access to the plug-in bundle and system libraries/frameworks.
`bundledRoutingServiceEndpoint` resolves the caller's loaded image with `dladdr`,
requires a driver-bundle layout and routing-driver signing identifier, validates
the bundle's strict code signature and checks the exact JSON bytes with
`SecCodeValidateFileResource`. Missing, unsealed, modified, linked, oversized or
malformed pins fail closed, with no external-file fallback. The pin is read once
per successful loaded-image connection setup; the peer independently retries
broker loss. Stable Developer ID broker requirements let ordinary same-identity
broker updates reconnect without changing the driver's trust resource.

The driver plist declares exactly `com.lexasoft.loopz.routing.bridge` in
`AudioServerPlugIn_MachServices`. `prepare_routing_install.py`, invoked through
`stage_signed.py --routing`, signs all seven routing components with one Developer
ID team and Hardened Runtime, writes matching helper and bundled broker pins,
then signs the driver after sealing its resource. A certificate-signed native
checker loads that staged image and validates the same driver loader without
invoking its factory or making an audio/service connection. Only after that
check does staging publish a new package directory with a complete file manifest.
Existing packages and source build products are preserved. Generated root/user
launchd plists are inert files; staging never registers them or changes capture
permissions. The login-start choice remains an installation/editor requirement.

`artifacts/bundle-trust-proof.json` records real signed staging and 19 isolated
loader/OS rejection cases, plus package-failure and service/driver regression
checks. A source/SDK preflight first identified the two former activation gaps;
that historical evidence remains in `artifacts/production-activation-preflight.json`.
The new bundle and trust path are not installed. The initial activation/rollback
transaction described below is prepared; real Apple-host validation remains
required. The feasibility installer rejects this separately versioned payload
rather than silently mixing its protocols.

### Guarded initial routing activation

`install_routing.py` verifies the complete manifest, fixed publisher/team and
component identifiers, Hardened Runtime, expected entitlements, sealed broker
pins and exact launchd job policy. A reviewed manifest digest is required for
installation. Root work uses a private verified snapshot under the system sticky
temporary directory; no root process executes the mutable staged checker. The
installer and system Python preflight are documented in `docs/install.md`.

This initial migration requires the installed Engine and its user job to be
stopped and the new routing catalog empty. It preserves the legacy driver,
broker, probe, checker and daemon, including hashes/inodes. A separately named
routing checker inventories public UID, rate, channel counts and input latency.
It also runs an output-only silent continuity client. The transaction stages
all sibling replacements, retains originals until verification, starts the new
root broker and bootstraps a uniquely named temporary console-user helper job.

`EngineRuntimeOptions::installationValidation` prevents capture and physical
monitor construction even for a complete saved graph. External providers are
in-memory unavailable sources, and the physical monitor decorator is omitted.
Routing, timing, public endpoint publication and normal rendering remain active.
The CLI accepts this policy only explicitly and reports it alongside authenticated
session and committed per-device metadata. This mode is never saved into the
normal user agent or the user's routing document.

The install probe creates its own private pass-through document. Success requires
an authenticated native acknowledgement, a real Core Audio device with matching
UID/rate/channels/latency, successful withdrawal, unchanged audio defaults and
Core Audio process identity, unchanged protected legacy files and a passing
continuous physical-output callback. It never invokes a global audio reset,
creates the user's document or registers login startup. A failed transaction
attempts independent job cleanup, broker stop, file restoration, prior-broker
restoration and runtime verification. Repair failures retain adjacent backups
and are reported explicitly. A new driver image may remain loaded after file
rollback even when public objects have been withdrawn; unloading is not claimed.

`artifacts/routing-install-proof.json` records the current prepared evidence:
fault injection, real output-guard dry run, signed payload audit and read-only
preflight. Initial activation still needs administrator authorization. Updating
an active production graph, replacing a changed loaded driver, persistent
background/login integration and real client IO remain separate unfinished
acceptance gates; this initial installer is not full product completion.

Actual XPC tests verify code-signature role rejection, server signature/UID
rejection, exclusive ownership, lease retirement, driver reconnection, simulated
console changes, uncertain/late replies and driver loss during an operation.
Another test starts a temporary uniquely named user-domain launchd broker and
separate driver/helper fixtures, verifies 2/8/64-channel memory transfer, then
replaces helper and driver processes. Its job is removed in cleanup. The native
factory suite also exercises the actual driver configuration/IO entry points
through the relay, including silence during disconnection and format recovery.
These tests use fixture identities/UIDs; they do not prove installed root-domain
activation or the real Apple-host startup path. They require an ordinary user
session: the Codex tool sandbox blocks macOS's CodeSigningHelper authentication.
Authentication is never disabled to accommodate that restriction.

## Persistent document runtime

`EngineRuntime` owns the configuration controller, shared capture registry,
physical monitor owners, scene publication and render thread. One control owner
runs discovery, file reads, authenticated requests, graph preparation and provider
acquisition. A separate thread renders the committed scene. Blocking control work
does not hold a render lock or stop unchanged capture/monitor callbacks. Shutdown
joins the render thread, stops/joins output callbacks and capture providers, then
reclaims controller and scene resources in that order. Explicit stop/start keeps
the desired document and restores its providers; it does not submit an empty graph.

The portable `RenderSchedule` renders contiguous blocks on a common host clock,
with explicit lead and a ceiling-rounded next wake time. Early/interrupted waits
cannot consume the next block. A tick bounds catch-up to four blocks, retaining
timestamps after short stalls; a delay beyond the configured history budget
re-anchors once instead of replaying an arbitrarily old queue. The native thread
uses Mach waits, records late/skipped frames and published frame position, and
attempts realtime scheduling while reporting whether the policy was accepted.
Wait/clock failures retry the render owner without resetting an authenticated
driver session or closing healthy providers. Real sleep/wake and latency remain
installed acceptance gates, rather than consequences of the synthetic clock test.

Only a native-acknowledged scene activates capture and monitor ownership. Public
transport loss cannot suppress a configured monitor's shared mix. Unrelated
providers continue while missing branches retry, and recovery is complete only
when the desired native revision is applied, a committed scene has rendered and
all demanded provider/channel capabilities are acquired. Per-source/device meters
and missing-frame observations remain separate signal-flow telemetry; legitimate
silence does not count as device loss. The monitor queue minimum uses the actual
configured render quantum, including 32- and 64-frame scenes. Scene preparation,
scheduling and monitor ownership accept 32/64/128/256 frames; their existing
defaults remain unchanged. Zero render lead is an explicit diagnostic profile,
not an implicit reduction of every route's safety queue.

Unchanged live monitor selections share their existing program publication
across scene replacements. Reuse requires the same bus identity, input channels,
monitor identity/selector, output channels, gain/mute/enable state, connections,
queue floor and render quantum. Public device names, endpoint rates/latencies
and upstream edits do not change that internal-clock monitor DSP. Only control
code copies or destroys its shared owners; the serial physical callback borrows
the selected program through the existing bounded publication mechanism. Reader
cursors, meters and counters therefore survive unrelated commits and aborts.
Physical profile recovery corrects the shared program off the audio thread, so
an identical pending selection cannot later restore stale output-rate DSP. A
real monitor edit still prepares a separate program and publishes it only when
the scene commits.

Scene-local rendering counters can restart when public IO timing is reconciled.
The mixed diagnostic measures persistent physical monitor callbacks, history
coverage and input acquisition attempts throughout that transition, independently
of the controller's temporary applying/restored state. It also requires the full
graph to settle within five seconds and sustain a fresh steady interval. An
offline/stalled/reset monitor, additional missing frames, changed Engine identity
or physical input reacquisition fails immediately. This keeps timing metadata
replacement from hiding an actual audio loss or becoming a false loss report.

Application capture can request 32/64/128 frames on its own private tap
aggregate. The setting is immutable in a source factory and reused for every
stream and subsequent acquisition. It never changes a selected physical output's
shared period. The default request remains 128 frames. An opt-in
`--capture-delivery-diagnostics true` mode records actual callback frame counts,
arrival gaps, entry age of the first sample, and age of the block's exclusive
end when its history publication completes. The end timestamp uses nominal
sample rate; signed negative ages preserve future timestamps.

Portable `AudioDeliveryTiming` has one serial writer. Its normal warm-up is one
second; observation performs no allocation, logging, OS query or atomic retry.
The macOS callback supplies monotonic times only when diagnostics are enabled.
`CaptureBuffer::append` now returns whether history publication succeeded;
rejected publications have a separate counter and cannot masquerade as ready
audio. Control reads/resets the plain timing counters only after callback
admission closes and in-flight callbacks join, then emits a bounded
`capture-delivery` summary. A summary with zero checked callbacks has no measured
steady timing, even if acquisition supplied warm-up callbacks. These diagnostics
do not change timestamps, queue sizes, filters or missing-frame policy, and do
not establish a future latency bound.

The signed Engine also exposes an opt-in `--virtual-input-check` diagnostic for
input-only clients, including source-only LoopZ devices and the prepared Loopback
comparison endpoint. It reuses the standalone probe recorder and portable marker
checker; the embedded entry point accepts only bounded read UID, duration,
period and frequency arguments. A virtual transport with at least two input
channels is required before acquisition. Existing permission is checked without
requesting consent. Output IO is disabled, and physical-device period changes
are outside this command's scope.

The native period guard preserves its original value, verifies requests and
restores only its own still-current selection; concurrent different settings are
not overwritten. Input callbacks use SingZ callback admission, and signal
cancellation closes admission and joins callbacks before reporting counters or
restoring the period. Cancelled observations cannot claim a passing timed run.
Reports include actual callback frame bounds, silent-sample checks, timestamp
marker/receipt observations and a restoration acknowledgement. The generator
fixture separately records actual callback sizes, so comparison matching does
not rely solely on a requested buffer property.

A missing configuration is a waiting state, not an implicit empty replacement.
The runtime can create/save its first document explicitly. It watches primary and
backup file identity, size and modification/change times, so unchanged files do
not overwrite in-memory edits. Loading protects against replacement during the
read; another watch tick resolves a concurrent change. Invalid/future documents
preserve the last valid desired graph and the original file. Backup recovery keeps
its existing storage semantics. File failures stay visible while the process and
healthy routing continue.

`InstalledDriverLink` periodically reloads the fixed root-owned service pin.
Missing/untrusted metadata waits with bounded retries and no unpinned fallback.
Pin replacement or loss drops the old authenticated peer. Its wrapper session
numbers never repeat even when a replacement peer starts counting from one, and
asynchronous peer loss is visible immediately to post-request session checks.
The normal authenticated link separately retries driver/broker availability.

The existing engine has an opt-in `--routing-document <path>` mode using this
runtime, with optional render/source/monitor timing arguments. It runs until
SIGINT/SIGTERM (or an explicit diagnostic `--seconds` limit), processes the native
run loop, emits structured status, and accepts SIGUSR1 to accelerate recovery.
The default installed legacy mode remains unchanged. This mode does not install
a login agent, change a default device, request privacy consent during recovery
or register an untrusted service. Root-domain deployment, an explicit login-start
choice, supervised restart and the editor command service remain required.

Lifecycle fixtures verify virtual dependency demand, shared input ownership,
independent source loss/restore, healthy monitor continuity with retired public
maps, blocked control, deferred acquisition and stop/start restoration. File
fixtures cover initial absence, explicit first save, unchanged in-memory edits,
external replacement, backup recovery and future-version preservation. An actual
authenticated XPC fixture runs three synthetic-source devices (2/8 channels),
checks shared capture and gain edits without reopen, then replaces the driver
service and verifies full restoration. Its simulated HAL heartbeat runs
independently from control, matching the native IO ownership rule. These tests
run with address/undefined/thread instrumentation without suppressed diagnostics.
A real process smoke check also verifies capture-free startup/file detection,
service waiting, retry signals and clean shutdown with no installed service pin.
Current evidence is `artifacts/persistent-runtime-proof.json`; it does not certify
installed protocol-2 activation, real multichannel source callbacks or sustained
new-runtime performance.

## Public pass-through timing observations

The document runtime prepares a separate pass-through delay floor for each
public device. The portable scene planner accepts these explicit floors and
propagates them through virtual-device dependencies; it makes no platform calls.
The macOS control owner supplies a pinned floor snapshot to scene preparation,
and advertised native input latency must match that exact prepared plan.
Changed timing goes through the existing native acknowledgement transaction
before scene publication, without changing saved routing intent. An unrelated
endpoint retains its mapping and a borrowed previous scene stays immutable.

The initial single-process/two-output rehearsal exposed holes in an unrelated
512-frame client's original pass-through at the old fixed 128-frame lookback.
A diagnostic 1024-frame queue removed that failure. A subsequent automatic
policy also passed the rehearsal, but its helper-side buffer query still saw
512 frames when a writer reported 32 or 64 frames. Those results establish the
fallback profile's continuity, not observation of every client's buffer size.

The newer driver therefore advertises read-only custom property `lztm` as a
retained CFPropertyList dictionary, with `version`, `session`,
`writePeriodFrames` and `running`. Its WriteMix callback records the larger of
the actual frame count and the nominal period supplied by
`AudioServerPlugInIOCycleInfo`. The largest observed value persists while any
client runs, including transport replacement; first-client start after all
clients stopped begins a new session. Input reads do not claim playback timing.
The callback uses an atomic observation and one conditional compare/exchange,
with no retry loop, allocation, property notification or IPC. Dictionary
allocation and queries occur on control. The shared audio mapping ABI is
unchanged. A legacy driver, or a new session without a playback observation,
uses the explicitly unmeasured 512-frame fallback. A caller-local buffer query
is no longer used as proof of another process's period.

For an observed period P converted into render frames, the queue floor is
`ceil(P + max(P, renderLead)) + blockFrames + inputFilterWing`, or the requested
pass-through floor if larger. The wing is zero at matched rates. A measured
first session can replace the idle fallback; a growing running session keeps
its largest queue until a new observed session or idle state. Missing an idle
poll cannot carry the previous queue into a different reported session.
Unsupported periods, malformed dictionaries and unknown versions keep the
failure visible through ordinary control recovery.

`artifacts/public-pass-timing-proof.json` separates the passing live fallback
rehearsals from the newer driver/session unit and isolated-host evidence. The
new telemetry driver is not installed. Its actual Apple-host observation,
minimum queues, 3–5 ms app profile and matched Loopback comparison remain open.

## Client sample-rate requests

The preview driver advertises 44.1 and 48 kHz on each device and its PCM streams.
Nominal-rate and virtual/physical-stream format setters validate the complete
requested format and enqueue a control request. They never replace live IO
mappings or change the running format directly. The engine polls requests at
most ten times per second, records the requested rate in the complete desired
document, and uses the existing prepare/submit/HAL-approval/commit transaction.
A native rejection leaves the saved intent available for automatic retry.
Physical input and monitor rates remain independently owned.

Each driver lifetime creates a random UUID origin and monotonic sequence tokens.
Only the latest request for each device is retained, including a request for its
current rate that supersedes an earlier pending change. The queue is bounded by
the 32-device catalog. An acknowledgement clears only an exact device, origin,
sequence and rate match; stale replies cannot discard a newer request. Shared
device lifecycle state retains requests through mapping/format replacement and
rollback. Deleting a device discards its obsolete request.

The authenticated helper/broker/driver channel carries bounded, validated rate
queries and acknowledgements under the same signed identities, console ownership,
pairing epoch, outstanding-request limit and deadlines as configuration changes.
There is no unauthenticated alternative or realtime-thread RPC.

Document version 2 adds an opaque per-device `formatRequestReceipt`. Existing
version 1 documents still load; accepting a rate request upgrades the document.
The receipt and rate are saved atomically before acknowledgement. If an ACK is
lost and the helper restarts, replaying that token cannot overwrite a newer
editor choice. Editor updates preserve this receipt; disabled devices can retire
a pending request without enabling the device or changing its saved rate. Failed
saves, unsupported future schemas and replies from an expired pairing leave the
request unacknowledged. An ACK means that intent has been accepted, while the
native configuration result remains authoritative for format activation.

Current evidence is `artifacts/client-rate-proof.json`. Fixtures exercise the
actual plugin setters, authenticated XPC, runtime reconciliation and native host
approval/rejection, alongside durable-save/restart and malformed-input checks.
This feature is not installed, and these tests do not establish real client
compatibility, reported engine latency or acoustic timing.

### Shared IO reference ownership candidate

HAL client registration now retains a reference count for IO/timing starts.
Additional starts preserve the current clock and timing session; one stop releases
one reference. Host-confirmed client removal releases its outstanding references
without disturbing other clients. An idle registration does not keep IO running.
The first running reference and the final release notify the host about
`kAudioDevicePropertyDeviceIsRunning`, with the state mutex released so reentrant
property queries remain safe. The public callback-timing dictionary is unchanged.

This candidate addresses lifecycle assumptions found while preparing mixed-source
recovery acceptance. An Apple-hosted shared-client startup failure is reproducible
with an additional prepared idle output client, but the proposed correction has
not yet been installed or demonstrated to resolve that failure. Native and
sanitizer coverage, source archives and the guarded update preflight are in
`artifacts/shared-client-driver-update-proof.json`.


## Global buffers and live graphics, 21 October 2026

The desktop now offers one immediate hardware-buffer selection for every enabled physical input and monitor in the saved routing graph. The Engine deduplicates endpoint UIDs, resolves default selectors, validates connected targets before saving, and retains accepted intent after a temporary write failure. Version-1 device preferences remain readable; version 2 stores the global routing buffer. A reconnect or newly used endpoint inherits that global choice. Unused endpoints are excluded. Device status distinguishes a saved choice from confirmed hardware state.

Monitor outputs now detect shared hardware-period changes and reopen their own stream to refresh its negotiated format. The normal monitor queue retains its conservative 1024-frame floor in addition to two hardware periods plus one render quantum. A lower-floor live trial dropped monitor frames and was rejected. An accepted hardware-buffer batch explicitly reopens owned monitors, since a provider can retain its old negotiated period after the shared hardware property changes. The selector labels per-buffer target time using actual endpoint sample rates, rather than claiming end-to-end latency.

Live meters use contained GPU transforms, with a 35 ms attack and 80 ms release; their layout width is never animated. Routing cards keep SingZ materials without live backdrop blur. Meter values behind a modal remain visually frozen until it closes, while Engine status and dialog controls continue updating. Wire geometry is measured on routing/layout/availability changes, excluding changing diagnostic counters. Drag cancellation likewise depends on actual wiring changes rather than inventory refresh object identities.

The linear resampling coefficient lookup is inline in the portable core. Its filter, coefficients, support and numerical checks are unchanged. Release and sanitizer resampling/Engine tests, global-device settings recovery tests, WebKit settings/graphics checks and source/monitor drag regression checks cover the update. The detailed measurements are retained under `artifacts/global-buffer-*` and `artifacts/balanced-*`; short CPU samples are not a sustained-load guarantee or a physical latency measurement.
