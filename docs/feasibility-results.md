# Feasibility evidence

The native prototype routes stereo audio through the installed LoopZ device on
this arm64 Mac running macOS 26.6.2. It reuses the pinned SingZ zcore audio host
and actual zdsp mixing/limiting graph. The complete routing interface and
SingZ design-system integration belong to the next phase.

## Shared capture ownership and interface prototype (2026-10-19)

Shared capture ownership is now connected to full-scene preparation and the
native-driver/XPC fixture. Independent application/device identities recover
separately; the same identity is captured once across routes, with independent
reader cursors and combined app-original mute policy. A fixed logical layout
survives narrower hardware profiles. The new portable history handles 1–64
channels, independent clocks, generation changes and failure silence. The macOS
multichannel AUHAL adapter is built and checks permission without requesting it.
Its actual hardware callback path has not yet been exercised in the installed
helper. No capture or protected permission acquisition occurs in the unopened
adapter tests.

OpenDesign Cloud's design workflow completed using Local Codex. The interactive
workspace and implementation handoff reuse SingZ's real CSS/token system and
include source/monitor controls, channel wiring, pending configuration and
source/helper/driver recovery states. The prototype explicitly simulates audio;
it is not an engine-connected application. Review also aligned all volume
controls with native gain 0–1 (silence through 0 dB). The source bundle and
screenshots are retained in `artifacts/interface-design/` and the run record in
`artifacts/opendesign-interface-run.json`. Installed binaries remain unchanged.

Validation evidence for this change is recorded in
`artifacts/source-runtime-proof.json`; the earlier scene-only evidence below
remains historical evidence for its recorded source hashes. Persistent render
scheduling, multiple monitor ownership, real input/monitor clocks, installed
activation and production UI integration remain separate gates.

## Full-scene rendering and restoration code (2026-10-19)

The new portable `AudioScene` retains the full desired device/source graph,
renders virtual dependencies on a common clock, and silences missing sources
without discarding their bindings. Returning providers resume through the same
scene. Demand propagates upstream, real source/device meters remain observable,
and a failed endpoint does not interrupt other graph branches.

The macOS configuration controller now stages the entire DSP scene before a
native request. Exact successful acknowledgement publishes it; failed preparation,
rollback and stale-session replies preserve the previous scene. Bounded graph
retirement waits safely for outstanding readers. New transport adapters support
2/8/64 channels, endpoint clock conversion, independent playback readers, heartbeat
silence and sticky mapping retirement. Unchanged mappings retain converter state.

All 20 regular tests pass, including actual playback → zdsp scene → recording
callbacks through the authenticated XPC/native driver fixture before and after
service replacement and a format change. The portable-only build passes 8 tests.
Validation records: `artifacts/graph-renderer-macos-validation.log` and
`artifacts/graph-renderer-core-validation.log`. Sanitizer evidence and exact
source hashes are recorded separately in `artifacts/graph-renderer-proof.json`.

This code is not deployed. The installed helper, driver and two-hour baseline
remain unchanged. Multiple physical source/monitor ownership, persistent runtime
scheduling, installed root/Apple-host activation, real unplug/replug and sleep/wake
still require implementation or live verification. The current endpoint converter
is linear and adds one internal frame for causal interpolation; these tests do
not establish installed or acoustic latency.

## Two-hour mixed-source gate passed (2026-10-19)

The previously installed signed stereo helper passed 7200.952 seconds at 48 kHz,
with application capture, the built-in microphone at gain 0.01, a muted 44.1 kHz
hardware monitor, two simultaneous recording clients and 719 additional client
checks. Both clients passed 7199 marker windows: 21–22 ms and 22 ms measured
app-route delay, zero quiet/callback/nonfinite/timestamp/queue errors and zero
bad marker windows. All post-warmup and post-checkpoint engine error increments
are zero. The microphone and monitor delivered continuous timestamped history.

Settings: 256-frame graph blocks, 1024-frame source lookback, 64-frame render
lead, driver clock period 128 and zero additional driver read queue. Minimum
publication slack was 1235.5 microseconds; maximum observed graph execution was
89.833 microseconds. Monitor coverage was 317641984 frames versus approximately
317642139 expected, within the 4096-frame tolerance. Observed RSS max-minus-min
ranges were 8128 KiB for the engine and at most 3312 KiB for the source/clients;
these are residency ranges, not a monotonic growth or leak proof.

Immutable result: `artifacts/two-hour-mixed-source-1792388601-result.json`.
Summary and exact executable hashes: `artifacts/two-hour-mixed-source-proof.json`.
The measurement uses digital Mach-time markers at 1 ms search resolution; it
does not measure acoustic microphone latency or external application processing.
It validates the recorded installed baseline, not the newer recovery/service
builds. Their installed and broader compatibility gates remain separate.

## Stable signing validation

LoopZ now stages Developer ID-signed engines using SingZ's disposable keychain
workflow and its encrypted certificate-password store. The helper carries
Hardened Runtime, a secure timestamp, the native microphone entitlement and a
stable Apple-team designated requirement. Two actual signing runs completed
without a Keychain authorization prompt; a changed-version temporary copy had a
different code hash and the same designated requirement. Temporary keychains
were removed and the original search list was preserved. Exact helper/checker
broker pins and full package hashes still verify. Evidence:
`artifacts/singz-signing-proof.json`.

Two signed helper updates passed installation, preserving the resident driver
inode, Core Audio identity and all three default device selections. During the
latest replacement an unrelated hardware stream delivered 73,728 frames at
48 kHz with zero timestamp errors. Evidence:
`artifacts/routing-helper-install-result.json`.

The first switch from ad-hoc signing required fresh privacy authorization.
App-only checks then passed at both rates at 4 ms. After the next signed update,
which adds the generic routing core and foreground permission diagnostics,
app-only capture again passes at both rates at 4 ms without another permission
request. The installed helper reports microphone authorization as authorized
from the ordinary user session. Microphone plus muted-monitor checks also pass
at both rates: 21 ms at 48 kHz and 23 ms at 44.1 kHz, with two clients and zero
active error increments. No further privacy prompt was required for these runs.
Evidence: `artifacts/routing-regression-suite.json`. This establishes app-capture
grant continuity for this update; it is not a general guarantee for every
privacy service or signing change. Thirteen local CTest suites pass, including
configuration recovery and signing-isolation, credential-redaction and cleanup
checks at that stage. The later two-hour mixed-source pass is recorded above;
its tested executable hashes remain unchanged.

## Automatic graph recovery (next build)

The portable desired-connection coordinator and current stereo engine now retry
failed resources without deleting requested routes. Sources and monitor output
reconnect independently; bridge loss rebuilds the requested session. A virtual
rate change or render-clock scheduling gap rebuilds the renderer while retaining
a live bridge, avoiding repeated broker resets. App process/route/format/stall,
microphone UID/default/rate/authorization/stall and monitor state/format/stall
checks run on the control thread. Missing source inputs render silence.
Microphone recovery does not request a new privacy grant.

All 15 macOS CTest suites pass. The portable build, with the macOS backend
explicitly disabled, passes all 7 suites. Recovery policy tests additionally
pass AddressSanitizer and UndefinedBehaviorSanitizer with instrumentation
verified on both implementation and test translation units. They cover a
32-device dependency graph, repeated service loss, an hour of unavailable
hardware, backoff, stale attempts, deadlines and invalid replacement intent.

The production engine/control sources are also compiled into a private-memory
fault test host. Failed bridge attachments, a live 48-to-44.1 kHz transition,
connection loss, a forced scheduling gap and permanently missing app/input/
monitor endpoints all pass. These tests do not open audio endpoints or connect
to the installed broker. They prove control behavior, not physical disconnect
or real system sleep recovery. Evidence: `artifacts/graph-recovery-proof.json`
and the three `artifacts/graph-recovery-*-validation.log` files.

The recovery build is not installed. Installed helper/broker/driver hashes are
unchanged; the completed two-hour stability pass tests the previous build. Full saved-document restoration, helper-process crash
supervision, protocol-2 service integration, real unplug/replug, sleep/wake and
permission-revocation acceptance remain pending.

## Desired-document configuration controller (local integration)

A new configuration controller now owns desired and acknowledged documents, the
per-device transport pool and one outstanding native transaction. It restores
the complete enabled configuration from a saved document, retains disabled
settings and retries rejected/aborted/quarantined changes without editing the
saved intent. It resolves uncertain submit replies through ordered status,
preserves staged mappings across transient query errors, retries failed
cancellation and rejects replies from a changed session. A failed save or reload
preserves prior intent. Future schemas are recognized before version-one field
validation, including future files with added or removed root fields.

All 16 macOS CTest suites pass; the portable-only build passes all 7 suites.
The controller unit suite covers a complete 32-device document with app/input/
virtual bindings, disabled settings and monitor controls, as well as file
reconstruction, corruption, future schemas and persistence failure. The native
factory integration suite now drives the actual configuration/IO entry points
from this controller: rejection followed by automatic recovery, superseded
approval, failed rollback/quarantine, format and output-visibility transitions,
continuous unrelated IO, and controller reconstruction with surviving clients.
The host is a test host, not installed Core Audio publication.

The controller is a library integrated with the driver through a test link.
The later authenticated XPC adapter is described below. The persistent user
agent and prepared DSP/capture publication remain required. These results establish native
configuration reconciliation, not full installed audio-graph restoration.
Evidence: `artifacts/configuration-controller-proof.json` and the corresponding
release, core-only and sanitizer validation logs. The completed two-hour pass uses the unchanged previously installed helper/driver.

## Authenticated service integration (not deployed)

Protocol-2 helper/broker/driver messages now use role-bound code requirements,
UID gates, exclusive ownership and pairing identities. The configuration
controller uses an ordered XPC adapter; the preview driver has an asynchronous
connection and a realtime silence gate for lost ownership. Helper disconnect
retires its mapping lease; driver disconnect preserves healthy helper mappings.
The new broker uses a separate Mach service and does not replace the installed
feasibility broker. Its production Apple-host startup reads a protected
root-owned broker pin and fails closed if that metadata is absent/untrusted.

All 18 macOS CTest suites pass in the normal user session. The local XPC suite
covers signature/UID rejection, ownership, lost connections, late/uncertain
replies, driver loss during an operation and lease retirement. A separate
three-process test registers one temporary uniquely named user-domain launchd
fixture, transfers 2/8/64-channel mappings, and replaces helper and driver
processes; its job is removed afterward. The actual native factory/configuration/
IO suite also passes through this relay, including driver-disconnect silence
and recovery. These are fixture code identities/UIDs, not installed root-domain
or physical/audio-client acceptance. The tool sandbox cannot validate these
XPC peers because it blocks CodeSigningHelper; the checks remain enforced and
are run outside that sandbox.

Evidence: `artifacts/routing-service-proof.json` and the associated release and
sanitizer logs. The persistent signed user agent, full DSP/capture runtime,
installer metadata and real Apple-host activation remain pending. Installed
helper/broker/driver hashes are unchanged; their two-hour stability pass is
recorded above.

## Current latency results

The new portable routing graph and device document have separate offline
validation. They cover 64-channel samples, 32-source grouping, channel fan-out
and summing, independent controls, capacity-bounded failure silence, typed source
bindings, cross-device cycle rejection and output resizing. The existing stereo
DSP tests also pass through the new implementation. The stereo compatibility
wrapper is now installed and has passed fresh 4 ms app-only checks at both
rates. The native driver and transport remain stereo; the wider graph and
multi-device model are offline capabilities.

Versioned JSON storage now passes round-trip, strict schema, atomic reader,
private file mode, corrupt-primary backup and future-version preservation
checks. A configuration-only CLI persists creation, naming and channel edits
across invocations. It does not yet activate those devices or restore routing
through a persistent controller.

Separate protocol-2 transport modules now allocate a private shared mapping for
each enabled device and support 2–64 channels in pairs. Tests cover all 32
device slots, independent directions and generations, failure silence, old
mapping retirement and two independent reader processes across ring wraparound.
The actual XPC shared-memory codec passes local alias/lifetime and malformed
message tests. Both new suites also pass AddressSanitizer and
UndefinedBehaviorSanitizer. Evidence:
`artifacts/device-transport-macos-validation.log` and
`artifacts/device-transport-sanitized-validation.log`. These modules have no
installed service connection or dynamic HAL publication yet; the long-running
installed route still uses the unchanged protocol-1 stereo driver.

The separate `LoopZRoutingAudio.driver` preview now passes tests through its
actual plug-in factory and IO callbacks with a fake host: 32-device discovery,
8/64-channel input/output, non-destructive readers, independent clock seeds,
100 concurrent renames, input-only devices, removal with attached clients,
re-enable and bounded property buffers. Transport preparation waits for driver
acceptance before retiring old regions; rejected edits preserve active audio.
Review fixes retain removed objects through host notices, distinguish mapping
identity across generation-counter resets and avoid teardown racing callbacks.
The native driver, transport and message suites also pass AddressSanitizer and
UndefinedBehaviorSanitizer. Evidence:
`artifacts/routing-driver-macos-validation.log` and
`artifacts/routing-driver-sanitized-validation.log`.

The preview now supports host-approved rate/channel/output/mapping transitions
with explicit pending results, reverse rollback, cancellation and ten-second
deadlines. Failed rollback withdraws only devices still in an unsafe format;
unrelated and already-restored devices remain active. Tests include 32-device
commit, 31 reverse rollbacks after a final rejection, live unrelated audio,
stale callbacks and same-UID recovery. Unchanged descriptions avoid native
notifications and table replacement. Evidence:
`artifacts/routing-configuration-macos-validation.log`,
`artifacts/routing-configuration-sanitized-validation.log` and
`artifacts/routing-configuration-thread-sanitized-validation.log`.

The current memory-sanitizer build explicitly instruments C, C++ and Objective-C++;
its native driver, transport, XPC codec and configuration-store suites pass.
Earlier native runs applied sanitizer flags only to C++, so those older results
provided narrower coverage. The concurrency run instruments both C++ and
Objective-C++ and passes ThreadSanitizer.

This preview remains outside the installer. Its clock period and direct-transport
latency properties have no installed latency measurement. Authenticated service
connection, client rate forwarding, engine integration and real client
publication/format transitions remain required.

An offline 48 kHz benchmark of 64-frame, one-to-one wiring measured median
processing times of 5.709 microseconds for three stereo sources, 227.083
microseconds for sixteen 64-channel sources, and 488.083 microseconds for
thirty-two 64-channel sources. Avoiding dense matrices for identity wiring
reduced the last case from 7104.334 microseconds. These are CPU timings over
100 observed blocks after ten warmup blocks, not end-to-end audio latency,
scheduler guarantees or installed multi-source capacity. Evidence:
`artifacts/routing-benchmark-before.jsonl` and `routing-benchmark-after.jsonl`.

The smaller-block app-only experiment measured 4 ms at both sample rates in
twenty-second rehearsals, with two independent clients, zero active errors,
and positive source/publication margins. It uses actual 64-frame render calls,
192-frame source lookback, driver period 128 and zero added read queue.
Measurement resolution is 1 ms. This is an explicit app-only experiment;
microphone, monitoring and conservative defaults are unchanged.

The 48 kHz small-block route also passed a 600-second regression at 4–5 ms:
599 valid marker windows per client, 59 additional client checks, zero active
and post-checkpoint errors, minimum source margin 0.914 ms and publication
slack 0.591 ms. This remains an app-only experiment. The later two-hour
mixed-source pass above uses different settings and does not extend that 4–5 ms
claim to microphone or monitor latency.

The earlier 600-second measurements use the baseline helper recorded by
executable hash in the artifacts. The new generic routing helper has separate
short installed results and the later two-hour installed pass described above.
The earlier helper's microphone/monitor retries passed at both rates,
measuring 21/23 ms with zero active errors. System Settings showed access was
already enabled; no permission settings or helper code changed for the retries.
The earlier permission-request timeout remains failed setup evidence with an
unisolated cause. Test agents are stopped and prior idle timing and 48 kHz
restored; Core Audio and all three default devices are unchanged.

The previous matched app-only rehearsals measured 16 ms at 48 kHz and 17–18 ms at
44.1 kHz, versus a fresh Loopback measurement of 15–16 ms and 17 ms. Both use
matching source rates and two independent clients. These are explicit
768-frame low-latency rehearsal settings, not a certified default. Stream
capture reduced measured delay in a matched A/B test from 54 ms to 32 ms,
before shortening the source buffer further.
The microphone mix passed short checks with a more conservative 1024-frame
buffer: 21 ms at 48 kHz and 23 ms at 44.1 kHz. A 64-frame computation lead
removed the publication-boundary misses observed with a zero driver queue.
The preceding helper passed the ten-minute 48 kHz microphone/monitor regression:
599 valid marker windows per continuous client, a steady 21 ms, 59 additional
client checks, zero active errors, and positive publication headroom throughout.
The helper and test sources stopped cleanly. Core Audio and default device
identities remained unchanged. This does not satisfy the two-hour gate.

Process-list retries tolerate brief client churn without treating unknown
routes as empty. A recording checkpoint separates active counters from later
cleanup, and control-loop autorelease pools bound temporary object lifetimes.
The later two-hour mixed-source gate passed for the recorded baseline above. See
[the current comparison and exact settings](latency-comparison.md).

The following sections retain earlier observations and staged experiments.
Their older latency figures and pending-state descriptions describe those
earlier stages; the comparison above records the current measurements.

## Installed observations

- Two independent Core Audio clients received the expected stereo signal at
  48 kHz and 44.1 kHz without callback failures. At 48 kHz both observed 0.1
  carrier amplitude per channel and zero measured cross-channel carrier.
- After the helper was killed, both already-open clients continued receiving
  callbacks and observed silence beyond the 250 ms heartbeat deadline. The
  observations use callback block timestamps; they do not locate the exact
  final nonzero sample.
- The authorized built-in microphone reached the virtual input with app and
  synthetic sources disabled. Recorded RMS was 0.00272578 per channel; the
  mono microphone is duplicated to stereo.
- Public virtual-output pass-through delivered the separate test source's
  stereo carriers alongside the helper's distinct synthetic carriers.
- The installed helper exercised an independent 44.1 kHz built-in speaker
  callback while the virtual device ran at 48 kHz. Physical output was muted.
- App-tap callbacks and expected mixed output were observed. That original
  fixture also used public pass-through, so it does not independently prove
  nonzero app-tap delivery. The subsequent isolated 20-second run disabled
  pass-through and passed both clients: each reported nineteen valid marker
  windows, 129 ms digital delay on a 1 ms search grid, zero timestamp errors
  and no source/driver/monitor misses after warmup. This establishes nonzero
  delivery through the selected app tap.

The raw measurements are under the local ignored `artifacts/` directory:
`two-consumers-result.json`, `rate-and-monitor-results.json`,
`failure-timing-result.json`, `microphone-result.json`, and
`pass-through-result.json`. Earlier runs did not capture per-test binary
hashes. The new sustained controller records tested executable hashes and the
host version at the start of each run.

## Automated checks

Five suites pass: real zdsp graph and timestamp-conversion tests; pinned vendor
integrity; the actual driver loaded by a test host; the timestamped probe's latency/channel/discontinuity checks at both rates;
and installer failure/rollback tests. Earlier core checks also
passed AddressSanitizer and UndefinedBehaviorSanitizer. A separate build from
a source copy verified that sibling SingZ checkouts are unnecessary.

The new probe holds four fixed one-second analysis windows. Synthetic tests
include microphone-level interference, swapped channels, absent timestamped
markers, a 20 ms delay jump and an input sample-counter discontinuity.

## Remaining gates

Transactional updates of the helper, DSP and broker now preserve the identical
resident driver. Preflight, complete-bundle compatibility, rollback, installer
locking, independent hardware continuity and authenticated bridge checks are
implemented. The new native hardware guard passed an explicit-stop rehearsal
and correctly failed a timeout rehearsal on the Zen Quadro at 48 kHz. The
privileged live update passed: Core Audio retained PID 29793 and its start time;
the driver directory inode remained 215872486; all three default-device UIDs
were identical. An independent hardware stream on the Zen Quadro at 48 kHz
rendered 131584 frames over 2.743 seconds with zero timestamp/sample-counter
errors during replacement. The real driver reattached to protocol 1 without
capturing audio. Evidence: `artifacts/restart-free-update-result.json`.

Fresh HAL activation and live replacement of a changed driver binary remain
unverified and unsupported respectively; they are not covered by the hot-update
claim. The installer rejects an incompatible resident driver before replacing
files and never invokes a global audio reset.

The isolated timestamped rehearsals passed at both rates. Both clients observed
129 ms at 48 kHz and 140–141 ms at 44.1 kHz, using a 1 ms marker search grid.
Each rate produced nineteen complete valid marker windows per client, zero
sample-counter errors and zero increments in post-warmup source, driver and
monitor errors. Evidence: `artifacts/latency-48000-result.json` and
`artifacts/latency-44100-result.json`. The device was restored to 48 kHz.
The attempted two-hour run failed around 420 seconds: the control/render thread
missed 2937 frames, the driver missed 3072 frames, and the recording client
reported 2688 quiet frames. It was stopped and cleaned up; the sustained gate
was not met. Evidence: `artifacts/soak-1792353278`. A five-second earlier
audio-presence rehearsal is not a clock or latency result. The timestamped run requires the updated installed helper
and its matching authentication configuration.

Microphone acoustic latency and physical monitor presentation latency have not
been measured. macOS 14.2, Intel, general calling-app compatibility, permission
revocation, multiuser policy and Developer ID distribution also need separate
validation. The minimum deployment target is not a tested support promise.

Astra reviewed the architecture and the installed evidence. Its findings led
to stronger source attribution, timestamped delay measurements, monitor
continuity checks, post-warmup error baselines and bounded cleanup. Full UI and
multiple virtual devices should wait for review of the remaining native gates.

## Loopback latency comparison

The installed Loopback 2.4.10 comparison measured 16 ms at 48 kHz and 17 ms
at 44.1 kHz using the same marker algorithm. LoopZ's 129 ms and 140–141 ms
results are about eight times greater. Most of the difference is explained by
the prototype's fixed alignment/driver margins. Setups differed in the extra
microphone/monitor paths and the comparison source's output rate at 44.1 kHz.
See [the comparison method and evidence](latency-comparison.md).

## Installed helper latency revision

The helper now renders on a dedicated thread with a Mach time-constraint policy
and its own Audio Work Interval. Device queries, bridge supervision and logging
remain on the control thread. The private application-tap aggregate requests
128-frame buffers, without changing a physical device's shared buffer size.
Source lookback defaults to 2048 frames rather than 4096, with a bounded
`--latency-frames` rehearsal option. The engine records minimum available-source
margins after warmup so smaller settings can be rejected from actual capture
availability.

Each render reader caches a coherent capture-clock anchor; two bounded snapshot
attempts can fall back to that anchor when a capture callback is preempted while
publishing. Source discontinuities invalidate caches through a generation check.
The core suite now exercises overlapping small-buffer appends and reads, and
checks that a cached anchor cannot replay history after a discontinuity. All
five suites pass. The signed helper-only update passed its live installation and hardware
continuity checks. The 2048-frame setting measured 86–87 ms at 48 kHz.
Short 1536-frame rehearsals measured 76 ms at 48 kHz and 82 ms at 44.1 kHz,
with two clean clients and zero post-warmup error increments. These twenty-second
checks do not establish sustained reliability; the long test has not restarted.
See `docs/latency-comparison.md` for exact scope and evidence.

The preserved driver still reports its original 5120-frame input latency and
uses its original 1024-frame queue. Therefore the revised helper's measured
signal delay must be distinguished from that stale advertised property. Correct
dynamic latency reporting and driver replacement remain native release gates.

A second, staged driver revision provides independent timing controls and
accurate source-delay metadata. Its isolated-host reload is an OS-specific
experiment, not a supported general installer path. Astra cleared its current
checks after fixes for interruption rollback, exact mapped image verification,
process generation checks and exact configuration validation. The privileged SIGTERM attempt did not terminate the host and rolled back all
files successfully. The original driver remains loaded. A separately approved
known-good, LoopZ-only forced recovery probe is pending completion; this does
not yet establish a changed-driver update path. BlackHole's
pinned source and installer review are documented in `docs/blackhole-review.md`.

## Latest installed driver evidence

Known-good isolated-host recovery and changed-driver activation passed on this
macOS release, with authenticated replacement hosts, exact mapped driver images,
unchanged Core Audio/default devices and successful independent hardware guards.
The new driver timing controls now match the helper's configured source lookback
during rehearsals. Removing the added read queue measured 54 ms at 48 kHz and
59 ms at 44.1 kHz, two clients per rate, nineteen valid windows per client and
zero post-warmup error increments. These are twenty-second rehearsals. The
long test remains pending after selection of the final capture configuration.
A reviewed device-stream capture optimization is staged for an identical-
settings comparison with mixdown; its helper update awaits macOS administrator
authentication. Exact evidence and scope are in `docs/latency-comparison.md`.
