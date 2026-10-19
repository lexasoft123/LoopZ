# Measured LoopZ and Loopback latency

The smaller-block LoopZ app-only experiment measured **4 ms at both 48 kHz
and 44.1 kHz** in twenty-second rehearsals on this arm64 Mac running macOS
26.6.2. These are measured digital signal delays, not advertised device
properties or acoustic microphone latency. Marker search resolution is 1 ms.

## Smaller-block app-only experiment

| Source/input rate | Measured delay, both clients | Minimum source availability margin | Minimum publication slack |
| --- | --- | --- | --- |
| 48 kHz | 4 ms | 1.185 ms | 0.622 ms |
| 44.1 kHz | 4 ms | 1.160 ms | 0.672 ms |

Each client validated nineteen marker windows with zero active audio,
timestamp, scheduling or publication errors. These runs used actual 64-frame
zdsp render calls, a 192-frame source lookback, a 32-frame computation lead,
driver timestamp period 128, and no added driver read queue. Selected
device-stream capture delivered 48-frame private aggregate buffers at both
rates. Queries of that aggregate's input latency and safety offset succeeded
and returned zero; these properties do not independently measure signal delay.

The lookback itself is 4 ms at 48 kHz and 4.35 ms at 44.1 kHz. The rounded
marker results are consistent with the 1 ms measurement grid. Input callback
receipt offsets are recorded separately and must not be mistaken for zero
delivery overhead. These app-only runs disable microphone, monitor, tone,
mixdown and pass-through. They do not establish 4 ms acoustic monitoring or
latency inside an external calling/recording app.

The shared ABI and capture history capacity remain unchanged. Driver build 3
only extends source-lookback metadata to 64-frame increments and identifies
that build; the independent queue grid and conservative defaults are retained.
The guarded isolated-host reload passed without changing Core Audio or default
devices, with an independent hardware continuity pass. A repeated 256-frame
lookback rehearsal passed after reload before advancing to 192 frames.

The first longer small-block attempt was interrupted at approximately 334
seconds when a concurrent build removed the probe used by later client churn
launches. It is failed evidence, not a 600-second pass. The controller now
snapshots its probe and source app into each run directory and verifies their
signatures; source bundles are sealed locally. Actual snapshot executable
hashes are recorded alongside the original build inputs. The interrupted run
has no active checkpoint, so its final 420 missing driver frames cannot be
assigned to the recording interval or cleanup from the available evidence.

## Previous matched Loopback comparison

The latest matched comparison with installed Loopback used the larger settings
below. It is a separate reference: the new 4 ms experiment uses a 128-frame
source-device timestamp period rather than the matched 1024-frame setting.

| Virtual input and source rate | LoopZ, 768-frame source buffer | Loopback 2.4.10 / ARK 13.1.3 |
| --- | --- | --- |
| 48 kHz | 16 ms | 15–16 ms |
| 44.1 kHz | 17–18 ms | 17 ms |

Both apps used only the selected application, unity source/device volume,
muted original playback, disabled pass-through, and no microphone or monitor.
Each rate used two independent recording clients for twenty seconds, with
nineteen valid marker windows per client. Both source fixtures matched the
recording rate. Marker search resolution is 1 ms. The small differences near
that resolution do not establish a general performance advantage for either app.

LoopZ used device-stream capture, a 768-frame source lookback, zero added driver
read queue, and a 1024-frame timestamp period. The source output used the same
LoopZ device/timestamp-period setting for the fresh Loopback measurements.
These are explicit rehearsal settings: the helper's conservative default
lookback remains 2048 frames, and these shorter settings are not a certified
product default.

## What removed the delay

A matched 48 kHz A/B test retained source lookback 1536, driver period 128,
zero read queue, microphone gain 0.01, and a muted hardware monitor:

| Capture mode | Actual capture buffer | Aggregate input safety-offset result | Measured delay |
| --- | --- | --- | --- |
| General stereo mixdown | 128 frames | 1024 frames | 54 ms |
| Selected device stream | 48 frames | 0 frames | 32 ms |

The offset is queried from the private aggregate, not the tap. These binaries
did not record query success, so the reported zero is not proof of a verified
zero offset. The measured A/B improvement does not depend on that diagnostic.

Selecting the device stream removed approximately 22 ms in this test. Reducing
the remaining source lookback to 768 frames then measured 16 ms at 48 kHz.
The real SingZ zdsp mix, safety limiter, and meters remained enabled.

The stream mode is selected only when successful process-output queries identify
one device with exactly one eligible stereo float32 output stream. Multiple
outputs or ineligible formats retain general mixdown. Failed enumeration is
never treated as an empty route. Confirmed route or stream-format changes stop
this feasibility run; the old mode remains explicitly selectable with
`--app-mixdown`.

Process-list checks retry three fresh whole snapshots on the control thread,
with 10 ms and 20 ms backoff. Persistent failures remain fatal and include the
failed object, property, and Core Audio status. Scoped autorelease pools reclaim
temporary objects during these checks. The audio render thread does not run
these queries or waits.

## Delivery cadence and comparison fixture caveat

The fresh Loopback fixture was silent in attempts using the LoopZ source device's
128-frame timestamp period. Restoring its previous 1024-frame period restored
the carrier. The mechanism has not been isolated, so this is a fixture
compatibility observation, not a general claim about Loopback. Silent attempts
were retained as failed evidence and excluded from the latency table.

At the matched 1024-frame period, LoopZ input callbacks contained 384 frames;
Loopback callbacks contained 512. Callback receipt offsets relative to input
sample timestamps are reported separately in the artifacts. The 128-frame
LoopZ setting previously produced 48-frame input callbacks and roughly 1 ms
receipt offsets. Signal timestamp delay and callback delivery time are distinct.

## Microphone mix and longer validation

The twenty-second 768-frame microphone/monitor rehearsal also measured 16 ms at
48 kHz and passed continuity checks, but its minimum microphone availability
margin was only 0.308 ms. It is not the preferred microphone setting. The next
candidate uses 1024 frames to retain more margin while keeping all sources
aligned to the same host timeline. Final-helper twenty-second checks passed
with the microphone and muted monitor active: 21 ms at 48 kHz and 23 ms at
44.1 kHz, zero active counter errors, and minimum publication headroom of
1.281 ms and 1.405 ms respectively. These figures still measure the app marker
inside the mixed route; acoustic microphone latency is unmeasured.

The final helper then passed a 600-second 48 kHz microphone/monitor regression.
Both continuous clients validated 599 marker windows at a steady 21 ms. All
59 additional one-second client starts/exits passed. Active source, driver,
monitor, scheduling, and publication-deadline error increments were zero;
post-checkpoint increments were also zero. Minimum publication headroom was
1.223 ms and minimum microphone availability margin was 2.297 ms. Observed
resident-memory ranges stayed below the 16 MiB per-process rehearsal limit.
The helper exited successfully and all three test agents were stopped.

A control-thread checkpoint after both recording clients finish captures active
counter deltas before helper shutdown. Later increments are reported separately
as `postCheckpointCounterIncrements` and are not automatically explained as
shutdown effects. Successful final helper shutdown remains mandatory.

One 44.1 kHz rehearsal measured 23 ms with the 1024-frame microphone setting,
but failed its strict gate because two driver frames were missing during the
active interval. Both recording clients passed; that did not override the
driver error. The follow-up helper schedules each block 64 frames before its
first output timestamp, leaving its source/output timestamps unchanged. Its
workgroup publication deadline is the current block's first sample, distinct
from the next wake; interrupted waits recheck the clock. New checks require
positive publication slack and zero missed publication deadlines with no read
queue. Repeated short checks passed at both rates with this correction,
including the matched app-only comparison in the table above. The failed run
remains at `artifacts/soak-1792357333/result.json`.

An earlier two-hour attempt failed around 420 seconds and does not satisfy the
sustained gate. The new ten-minute pass exceeds that failure point but does not
replace the required two-hour run or broader hardware/OS validation. Conservative
default settings have not been promoted to the shorter rehearsal settings.

## Method

The source generates 1100 Hz left and 2200 Hz right carriers plus independent
host-time-coded markers from its Core Audio output timestamps. Recording clients
correlate markers against their input host timestamps. A pass requires carrier
and channel identity, marker correlation, bounded signal delay, and timestamp
and sample continuity. No raw microphone recordings are saved. This excludes
acoustic propagation, speaker presentation, and processing in external calling
or recording apps.

The temporary Loopback comparison device was restored to 48 kHz and disabled
when its measurements completed. Its fixture was stopped. The original Loopback
device remained off and its routing was preserved. Helper-only updates preserved
the installed driver, Core Audio service identity, and all default devices;
independent silent hardware continuity checks passed.

## Earlier measurements

| Stage | 48 kHz | 44.1 kHz |
| --- | --- | --- |
| Original 4096-frame source + 1024-frame driver queue | 129 ms | 140–141 ms |
| Realtime helper, 1536-frame source + 1024-frame queue | 76 ms | 82 ms |
| Same source, zero added driver queue, mixdown capture | 54 ms | 59 ms |
| Latest matched app-only stream capture, 768-frame source | 16 ms | 17–18 ms |

The original Loopback run measured 16/17 ms, but used a 48 kHz source for both
recording rates. The new comparison above fixes that rate mismatch.

The driver timing controls were installed through an OS-specific isolated-host
experiment, preserving Core Audio and default devices. This is not proof of
fresh installation without restart or of that reload path on earlier macOS
releases. BlackHole's distinction between history capacity and actual queued
delay is documented in [the source review](blackhole-review.md).

## Evidence

The ignored local artifacts retain complete counters, rates, executable hashes,
selected capture mode, and driver settings:

- `artifacts/latency-stream-48000-1536-result.json`
- `artifacts/latency-mixdown-ab-48000-1536-result.json`
- `artifacts/latency-stream-48000-768-mic-result.json`
- `artifacts/latency-matched-48000-768-app-only-result.json`
- `artifacts/latency-matched-44100-768-app-only-result.json`
- `artifacts/latency-final-48000-768-lead-matched-app-only-result.json`
- `artifacts/latency-final-44100-768-lead-matched-app-only-result.json`
- `artifacts/latency-final-48000-1024-lead-mic-monitor-result.json`
- `artifacts/latency-final-44100-1024-lead-mic-monitor-result.json`
- `artifacts/latency-final-48000-1024-lead-mic-monitor-churn-result.json`
- `artifacts/loopback-recheck-1792357145/result.json`
- `artifacts/latency-checkpoint-helper-install-result.json`
- `artifacts/latency-pool-helper-install-result.json`
- `artifacts/latency-render-lead-helper-install-result.json`
- `artifacts/latency-recheck-cleanup-result.json`
- `artifacts/latency-comparison-latest.json`

Earlier installation/recovery evidence remains in
`artifacts/driver-host-recovery-result.json` and
`artifacts/driver-timing-update-result.json`. Historical latency results and
failed rehearsals are retained alongside the newer runs.
