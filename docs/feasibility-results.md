# Feasibility evidence

The native prototype routes stereo audio through the installed LoopZ device on
this arm64 Mac running macOS 26.6.2. It reuses the pinned SingZ zcore audio host
and actual zdsp mixing/limiting graph. The complete routing interface and
SingZ design-system integration belong to the next phase.

## Current latency results

The smaller-block app-only experiment measured 4 ms at both sample rates in
twenty-second rehearsals, with two independent clients, zero active errors,
and positive source/publication margins. It uses actual 64-frame render calls,
192-frame source lookback, driver period 128 and zero added read queue.
Measurement resolution is 1 ms. This is an explicit app-only experiment;
microphone, monitoring and conservative defaults are unchanged.

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
The full two-hour stability gate remains unmet. See
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
