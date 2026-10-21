# Measured LoopZ and Loopback latency

## Installed graph processing headroom, 21 October

The installed Engine reduces sample-containment counting overhead
without changing buffers or timing policy. Seven interleaved matching-binary
rounds measure 32 independent 64-channel graphs with limiters at 486 versus 162
microseconds per 64-frame block. The complete portable 32-device scene with
meters improves from 979 to 650 microseconds with online silent sources, and
866 to 540 with offline sources. Every measured scene sample and meter is
checked. These are offline CPU benchmarks, not total audio latency or proof of
dropout-free native routing. Private and installed mixed fixtures each
pass six simultaneous strict 30-second recordings with no missing samples or
added renderer deadline misses. Both 32-independent-wide attempts fail before
the recording window; the metadata observer times out even with four-device
batches. Full-width acceptance and new wide-load callback measurements remain
open. No new physical or matched Loopback latency result is claimed. Evidence:
`artifacts/graph-containment-delivery-proof.json`.

## Separate callback observations installed, 21 October

The installed driver separates maximum accepted WriteMix frames from
maximum nominal IO period while preserving the existing routing timing policy.
Portable and sanitizer checks plus both fake-host driver variants pass. The
earlier compatibility check reported the new property unavailable on the former
driver without opening audio or requesting permission.
Deployment has passed and the new property is verified on a capture-free live
endpoint. Full-wide queries still time out, so there are no new raw wide-load
values or physical latency measurements.
Evidence: `artifacts/graph-containment-delivery-proof.json`.

## Input read cost and unresolved wide-load transitions, 21 October

The installed app update batches aligned equal-rate public reads. Matching
binaries in seven interleaved rounds measure the 64-channel/64-frame input adapter
at 6.208 versus 3.625 microseconds median-of-medians, about 1.71 times faster.
Buffers, render lead/cadence, source delay and driver/broker are unchanged.
This is CPU headroom, not a measured reduction in end-to-end latency.

Private and installed six-client mixed regressions each pass thirty strict
seconds with no missing samples, cadence errors or added renderer misses. Full
32-independent-wide stability is not accepted: an early private trial has 32
clean native recordings and a retained post-stop verifier failure, while later
repeats fail. Six period-growth/cadence failures correlate with live latency
reconfiguration; the final candidate has one zero-filled reader without a cadence
error. Distinguishing actual WriteMix frames from nominal IO periods is planned.
No larger buffer or hidden sample exemption is used to obtain a pass.

No new physical, analog or matched Loopback latency measurement is made. Zen and
Quad Cortex remain at 512 frames; the older 3–5 ms total-latency target is not
established by these results. Evidence:
`artifacts/public-block-input-delivery-proof.json`.

## Timing observation and full-wide limits, 21 October

The latest installed update uses a single graph timing plan and asynchronous
native public timing observation. Private and installed mixed-graph regressions
each pass six simultaneous thirty-second recordings at actual 64-frame
callbacks, with no missing samples or added renderer deadline misses. This is
virtual routing acceptance at the tested load, not a new physical latency result.

A separate private 32-independent-64-channel trial now reaches strict recording
and completes control calls within 108 ms. It still fails audio acceptance:
ten clients contain zero samples and the renderer adds 6,443 deadline misses.
The recording timestamps and callback cadence remain intact. This trial does
not establish a dropout-free full-width configuration, a latency improvement,
or a matched Loopback comparison. Hardware buffers remain 512; no new analog or
microphone measurement is made. Evidence:
`artifacts/timing-observer-delivery-proof.json`.

## Earlier discovery and mixed-load acceptance, 21 October

The latest installed app pair moves read-only hardware discovery off the command
loop. Private and installed runs each capture six simultaneous 2/8/64-channel
paths through a 32-device graph for 30 strict seconds at actual 64-frame callbacks.
All recordings have zero missing samples or cadence/timestamp errors; neither run
adds a renderer deadline miss. Physical buffers remain 512. These are virtual
load and responsiveness checks, not new microphone/analog latency or matched
Loopback measurements. The independent 32 × 64-channel recording workload has not
passed preparation. Evidence: `artifacts/background-discovery-delivery-proof.json`.

## Visibility interruption timing, 21 October

The later producer-settling update keeps an already-active producer warm across
repeated HAL pause/resume cycles. Two synchronized private trials return zero
zero-filled samples and add no driver capture misses, versus 120 returned zero
samples on the matched baseline and 84 on the earlier hold candidate. All
no-edit, unrelated and fresh-reader controls are clean. Continuity still fails:
the revised trials have 16 and 15 forward callback gaps, with maximum host jumps
about 279 and 283 ms. No change to steady physical latency is established.
The installed normal-session repeat has zero returned zero samples across
1,263,936 edited frames and no capture-miss increments, but retains 15 forward
gaps with maximum host jump about 239.533 ms. These finite runs do not establish
gapless reconfiguration. Evidence: `artifacts/visibility-profile-comparison.json`
and `artifacts/visibility-settling-delivery-proof.json`.

A synchronized private input-only diagnostic now distinguishes preparation from
the measured window and retains timestamped zero spans/callback gaps. The routing
Engine and installed apps remained unchanged in that earlier diagnostic run.
With actual 64-frame callbacks and
48 kHz virtual audio, the edited input has 16 forward gaps during eight output
visibility edits, with a maximum host-time jump of about 290.989 ms. One resumed
callback contains 16 zero frames on both channels, a 0.333 ms span. The unrelated
28-second recording, no-edit baseline and fresh-reader check have no zeros or
callback errors.

This is interruption timing, not steady microphone/analog latency or a matched
Loopback result. Both virtual routes retain their 1,088-frame scene delay and
mapping generation. Physical buffers remain 512 and no physical IO is opened.
The first edit reports 15 driver capture misses alongside the 16 zero frames;
the observations alone do not prove the provenance of each zero sample.
Visibility continuity remains failed. Evidence:
`artifacts/input-trace-delivery-proof.json`.

## Existing wide recording through graph expansion, 21 October

The latest silent-meter optimization repeats this workload twice with clean
30-second recordings of 1,440,512 and 1,440,448 frames. Late publications fall
from the parent's 1,065 to 3 and 0; neither run adds misses during its full-graph
hold. The live root delay is 192 frames in all three runs. Callback size stays
64, hardware buffers stay 512, and source timing/scheduling policy is unchanged.
This is virtual routing and CPU-load evidence, not a new physical latency result.
Evidence: `artifacts/silent-meter-delivery-proof.json` and
`artifacts/silent-meter-repeat-delivery-proof.json`.

The retained parent result follows:

The parent app pair passes a strict 30-second, 64-channel recording through adding
and removing 31 active 64-channel devices. The unchanged client captures
1,440,512 frames at actual 64-frame callbacks, with zero zeroed/nonfinite samples,
timestamp/callback cadence errors or render failures. Its original failed
baseline has 48,504,256 zeroed samples during the added load, while callback
cadence remains intact. Both runs are retained in
`artifacts/capacity-transition-delivery-proof.json`.

The changes remove unused monitor history and general resampler work at equal
rates, preserving the existing one-sample output delay. They do not increase
buffer sizes or timing reserves. Adaptive public timing can differ between
startup negotiations, so these runs are not a matched latency comparison.
Physical buffers remain 512; no physical input or output is opened here.

There are still 1,065 renderer late publications through capture completion,
with maximum reported lateness 953,584 ns, despite the passing recording. Wide
CPU margin, long-run and simultaneous wide-consumer acceptance remain open.
The offline conversion benchmark's approximately 3.1 times speedup applies
only to that equal-rate conversion case. No new microphone, analog, Loopback
comparison or 3–5 ms total recording claim follows from this result.

## Capacity window and timing scope, 21 October

The installed optimized pair passes a virtual-only 32-device graph with six
simultaneous 30-second recordings across 2-, 8- and 64-channel paths. Actual
callbacks are 64 frames. Every client records at least 1,440,000 frames with
zero zeroed/nonfinite samples, timestamp/callback cadence errors or render
failures. Contributing source miss counters and publication deadline misses
remain unchanged during the strict window. There are 27 lifetime deadline
misses before that window, during startup; these remain part of the report.

This fixture deliberately includes eight-device chains and retained timing
floors for silent pass-through sources. Its timestamp marker delays are
24.5–25.5 ms. These are stress-fixture virtual-path measurements, not microphone
latency, an eightfold per-hop cost, or a new Loopback comparison. Physical buffers
stay at 512. Device discovery and an offline DSP case are faster, but no new
physical latency reduction is claimed.

The final diagnostic uses a start barrier after stable client/graph preparation
and a writer-release barrier after every reader finishes. It checks every sample
inside the 30-second window, without a warm-up exemption. This avoids classifying
an earlier writer's deliberate shutdown as downstream sample loss. Earlier
unsynchronized failures, genuine pre-optimization render/control overload and
an attempt begun during timing negotiation remain retained. Evidence:
`artifacts/capacity-optimization-delivery-proof.json`. The original Quad Cortex
dropout, long-run/worst-case load and reliable 3–5 ms total recording remain open.

## Source relaunch and unrelated-route continuity, 20 October

The installed muted app fan-out now passes an owned source exit/relaunch with
two recording clients kept open. Source readiness returns about 2.112 seconds
after the relaunch request. Each affected client measures about 3.455 seconds
of silence including intentional app absence and reacquisition; these are
recovery intervals, not steady audio latency. Public device/stream IDs and clock
origins remain unchanged, and baseline/recovered marker windows pass.

A distinct peer app records continuously for 28 seconds with 64-frame callbacks,
zero zeroed samples, quiet frames, timestamp errors or bad signal windows.
Its provider acquisition and missing-frame counters do not increase. Evidence:
`artifacts/app-relaunch-policy-delivery-proof.json`. This is virtual-only source
recovery and isolation acceptance, not a new microphone/analog latency result,
wide real-app compatibility check or proof of the 3–5 ms recording target.

## App original-playback switching continuity, 20 October

The current installed update fixes preview-only metering muting original app
playback. An initial in-place recording-tap description update still produced
4,740 zero samples and 2,240 quiet frames in the first 12-second client, despite
one provider acquisition and zero timestamp errors. This failure remains in
`artifacts/app-playback-policy-installed-result.json`.

The accepted implementation uses fixed recording/mute tap descriptions and
switches a separate private mute reader's IO. Two simultaneous 12-second,
48 kHz app recordings with 64-frame callbacks pass post-warmup policy switches
with zero zeroed samples, quiet frames, timestamp errors, callback failures or
bad signal windows. Both minimum measured tone amplitudes are 0.1. Capture is
acquired once. The original app output is a separate owned virtual device;
no physical output is opened and no new microphone/analog latency is measured.
Evidence: `artifacts/app-playback-policy-delivery-proof.json`. Wider physical,
load, original Quad Cortex dropout and reliable 3–5 ms total recording gates
remain open.

## Latest virtual continuity check, 20 October

The current installed setup/publication update passes a 12-second virtual-only
recording at 64-frame input/output callbacks through 20 graph replacements.
It reports zero zeroed samples, timestamp errors, source missing frames and
new publication deadline misses in the checked intervals, with correlation
above 0.9999. The timestamp marker delay is 6.25 ms. No physical input or output
is opened, so this is not a new microphone, analog or Loopback comparison.
Evidence: `artifacts/setup-publication-delivery-proof.json`. Original hardware
buffers return to 512 frames. The physical latency estimates below retain their
original scope; reliable 3–5 ms total recording remains unproven.

## Normal app update installed, 20 October

The signed app-only update is installed. Normal routing now enables verified
application timing without opt-in arguments and retains linear-phase physical
input. Unknown/partial capture facts keep conservative history; an externally
requested scene holds its committed app timing floor until idle. No saved mix
or driver schema changes. Explicit global diagnostic budgets retain their
original behavior; `--automatic-application-timing false` disables this policy
for diagnostics. An explicit true selection still rejects a conflicting global
override in either argument order.

Actual installed-session checks use the canonical 64-frame renderer and lead,
the Zen microphone on input 3, an owned native app and an app-only virtual
dependency. Microphone and app occupy separate scene channels; portable phase
and summing tests remain separate evidence. There are no private Engine launch
or opt-in flags in the installed job.

| Hardware/client period | App history | Scene history | Hardware-adjusted microphone window medians |
| --- | --- | --- | --- |
| 32 frames | 448 frames | 512 frames | 12.5709–12.5730 ms |
| 64 frames | 448 frames | 512 frames | 12.9158–12.9180 ms |

Both six-second installed recordings pass waveform, callback, timestamp and
silent-block checks. Solo microphone, microphone/app scene and the app-only
dependency each have zero steady source/driver miss deltas. The stereo app
marker passes at both periods. Reported physical device/stream delay is included;
these are not analog-calibrated round-trip measurements. The preceding private
candidate's minute and 40-second lifecycle checks retain their own exact hashes.

A separate private normal-profile two-app fixture records an untouched source
continuously through two peer app relaunches and edits, with a bounded
low-priority CPU worker launched. Verified app histories stay at 448 frames,
the untouched provider's attempt count is unchanged across observation, and
lifetime provider, driver and publication deadline deltas remain zero. Initial
source retries before generators start are retained and not misclassified as
reconnects during recording.

The app pair updates as the console account without sudo. Exact installed
bundle hashes/signatures, unchanged driver, original Core Audio process and
default devices, preserved broker/login choice and restored canonical session
are verified. Test fixtures, settings/backups and undo state are removed, the
Zen buffer returns to 512 and the desktop is reopened. Evidence:
`artifacts/default-timing-delivery-proof.json`. Broader app/load/physical recovery
and full product acceptance remain open, as does reliable 3–5 ms total latency.
The older private/opt-in status below is historical and superseded for normal
linear-phase application timing by this installation.

## Private fast automatic mixed recordings, 20 October

The current signed diagnostic Engine now passes a fresh minute at each
32/64-frame Zen and recording-client period with automatic physical and
application timing both enabled. It uses 32-frame graph blocks and publication
lead, rather than the installed session's 64-frame blocks/lead. One shared
microphone provider simultaneously serves a solo minimum-phase route and a
linear-phase microphone/application mix. A third public recording client checks
the mix's application channels through a downstream virtual device.

| Hardware/client period | Solo hardware-adjusted window medians | Mixed hardware-adjusted window medians | Solo / mixed history |
| --- | --- | --- | --- |
| 32 frames | 7.970–7.978 ms | 9.919–9.928 ms | 288 / 384 frames |
| 64 frames | 8.978–8.979 ms | 10.912–10.914 ms | 320 / 416 frames |

The verified application history is 352 frames in both runs. Every microphone
client receives 2,880,000 samples with actual requested-size callbacks; four
waveform windows span seconds 1, 20, 40 and 58. Each approximately 56-second
steady counter interval has zero source/driver missing-frame deltas, zero
physical-history failures in every cause-specific category, zero failed
sample lookups, no provider reacquisitions and no new publication deadline
misses. The render workgroup remains joined. Timestamp, callback, nonfinite
and silent-block checks pass, as does the dependent stereo application signal.
Solo microphone recordings have no post-warmup zero samples. Mixed recordings
can contain isolated zero values; those are retained separately from missing
audio and are not called dropouts when availability counters remain clean.

These estimates include the Zen's reported 74-frame input-device delay; they
are not analog-calibrated latency. Waveform matching is low-passed at 6 kHz;
wideband converter tests are separate. The selected history budgets are
automatic, not a forced 192-frame profile. Neither run opens a physical output
or saves raw audio. The canonical installed Engine/desktop, exact empty
document/backups/settings, original 512-frame hardware periods, audio defaults,
driver/broker and existing permission state are restored and verified. Evidence:
`artifacts/adaptive-fast-mixed-proof.json` binds both measurements, the signed
private Engine and the portable filter proof below.

This is a private profile, not an app update or a fresh Loopback comparison.
It improves on the earlier mixed minimum-phase candidate's conservative
1024-frame application history, but does not prove 3–5 ms total recording,
continuous graph edits/fault recovery or broader application/load acceptance.

## Experimental input filter: independent clock matrix, 20 October

The portable minimum-phase input tests now cover all 25 source/output pairs
from 8, 44.1, 48, 96 and 192 kHz. Each clock independently takes -0.5%, nominal
and +0.5% rates. Contiguous source counters accompany alternating one-sample
host-time corrections. Two tones, three fractional positions, both correction
directions and 32/64-frame reads exercise interval splitting. Across 2,700 reads
and 129,600 output frames per build, all missing-frame categories and failed
sample lookups remain zero; instrumented append/read calls allocate nothing
and output guards remain intact. Scalar timestamp interpolation and a
frequency-domain reference bound the waveform error independently of the
production resampler's traversal; maximum error is 0.290 of the tolerance.

A separate steady-drift frequency sweep samples 15,606 responses in 102
downsampling cases, starting 50 Hz above the measured destination Nyquist
frequency. Worst sampled stopband gain is -91.401 dB, below the unchanged
-90 dB threshold. These checks pass in Release, address/undefined sanitizers
and a locally built core with the macOS backend disabled; the timing and
mixed-filter regressions pass alongside them (three groups per build).
Evidence: `artifacts/minimum-phase-independent-clocks-proof.json`, with source
snapshot, executable hashes and full test logs.

This is synthetic signal/clock evidence, not a hardware latency measurement
or a Windows/Linux run. The sampled stopband sweep covers steady drift, not
the entire aliasing envelope during abrupt local clock corrections. Dynamic
output-clock cutoff transitions, broad realtime load and full lifecycle
acceptance remain open before default promotion. The normal installed input
filter remains linear phase.

## Quad Cortex continuity and Engine recovery, 20 October

Installed eight-channel Quad Cortex meters now pass an actual-desktop preview
check with no automatic source connections. A four-device wired graph also
recovers after a verified Engine-only forced exit, preserving open virtual IO
clients, device/stream IDs and clock origin. Graph restoration is observed at
0.732 seconds; the continuous marker client measures 520 ms of silence during
the fault, with zero timestamp errors and clean signal before and after it.
The marker's 6.75 ms delay is a virtual path measurement, not microphone or
analog round-trip latency. Both physical providers' meters resume after recovery.

One earlier installed Quad Cortex check records 512 unexpected missing history
frames after warm-up. Subsequent passing installed checks and 30-second private
captures do not establish its cause or fix it. Those captures retain the
hardware's original 512-frame period and select 960 frames of linear-phase
history. Startup priming misses are counted separately from steady observation;
no physical output, hardware buffer change or permission prompt is involved.
Evidence: `artifacts/quad-cortex-recovery-proof.json`.

The next private capture-only probe adds optional cause-specific read counters.
At the unchanged 512-frame Quad Cortex period, a 30-second run with 111 control
snapshots has 1,343,744 requested history frames after its two-second baseline,
with zero missing output frames in every category and zero failed sample
lookups. Capture sample counters remain continuous, and publication deadlines
are met. Startup observations identify offline, unmapped-history and filter
support misses separately. These counters remain disabled in normal sessions;
they do not alter queue sizing or admit additional clock corrections. Release,
sanitizer and backend-disabled regressions distinguish premature/expired reads
from unsupported clock intervals, preserve valid silence, and verify independent
reader accounting without callback allocations. The earlier 512-frame defect
is retained as unresolved. Evidence:
`artifacts/input-history-diagnostics-proof.json`.

A separate 30-second repeat launches one owned low-priority CPU worker, bounded
to 35 seconds and joined before final state verification. Its 1,343,616
post-baseline history frames also have zero missing frames, failed sample
lookups, sample-counter discontinuities or publication deadline misses. This
is a bounded repeat, not broad-load or long-session acceptance, and it does not
reproduce the retained installed failure.

Native device replacement is now checked separately from signal latency.
A process-private input device disappears and returns under the same UID with
a new native object ID; the observed source-only restoration takes 196 ms.
With muted native monitors and an unconditional output-silence guard, two runs
restore the affected input and monitor in 360 / 354 ms. The independent Zen
source and monitor remain connected without new missing frames or monitor
errors. These values measure recovery after recreation, not steady audio delay,
and are not physical USB or sleep/wake measurements. Evidence:
`artifacts/native-device-recovery-proof.json`.

## Physical continuity through the production Engine, 20 October

The next private production Engine also passes ninety seconds of physical
preview continuity through the installed driver and authenticated XPC at the
original 512-frame hardware period. Both Quad Cortex and Zen have 4,330,496
post-warm-up requested history frames, with zero new provider/history misses,
failed sample lookups, reacquisitions or render deadline misses. After capture
release, joined callback diagnostics show no sample-counter discontinuities or
unsupported clock intervals. This is a continuity check without public
recording clients or physical outputs; it adds no latency result and does not
close 32/64-frame, analog or broad-client acceptance. Initial harness cleanup
failures and their verified repairs are retained. Installed app/driver bytes,
canonical session, graph/backup, settings and defaults are restored. Evidence:
`artifacts/full-source-diagnostics-proof.json`.

## Recording continuity while app timing changes, 20 October

The first cold affected-mix test exposes a real regression: lowering a running
mix's 1024-frame app budget causes a native input-latency reconfiguration and
produces 19 silent blocks plus one timestamp error. The private policy now
retains each matching app binding's committed floor while its mix, monitor or
downstream virtual dependency is externally requested. Editor preview alone
does not pin this floor. Required budget growth remains possible. Reductions
apply after real clients leave; recording never silently drops queued audio
just to reach the lower delay. The desired document is unchanged.

Verified app delivery survives a stopped capture. A reopened tap may reuse it
only when its routes match and every actual private stream has the same rate
and no larger period/latency. A changed or unknown format keeps conservative
timing; unsupported reports clear the cache and reacquire that source. This
keeps original-playback mute, matching app relaunch and ordinary idle capture
release from unnecessarily moving the healthy microphone's origin.

Two earlier guard candidates pass 20-second already-open affected recordings,
then optimize while idle and pass the next six-second recording: normal
64-frame linear rendering selects 512 frames and approximately 12.59 ms, while
fast 32-frame rendering with a 64-frame hardware period selects 416 frames and
approximately 10.92 ms. Cold recordings deliberately keep their original
1024-frame history. This avoids a disruptive optimization during recording;
it does not make that first cold client use the lower history retroactively.

Stricter lifetime telemetry subsequently finds 49 driver capture misses during
app exit even though the microphone provider loses no frames. The same run
contains a 1.704 ms render operation, exceeding the 1.333 ms publication lead.
This is consistent with a scheduling/publication miss, not lost microphone
capture. The old whole-block lateness counter stays zero and cannot certify
every publication deadline. New diagnostics count actual publication deadline
misses and maximum lateness. Shared-provider missing-read counters now survive
scene replacement and reconnects; individual logical reads each count.

The macOS renderer now owns and joins an audio work interval and reports each
work cycle's start and publication deadline. Apple recommends a separate audio
workgroup for independently clocked realtime rendering:
[Workgroup Management](https://developer.apple.com/documentation/audiotoolbox/workgroup-management).
Creation/release stays on control; interval start/finish uses the realtime-safe
API. Workgroup failures remain visible and do not stop audio. This integration
does not change source history, filter phase or the physical buffer setting.

The exact signed workgroup candidate passes a fresh **40-second continuous
microphone recording** across both original-playback mute settings and owned-app
exit/relaunch. Its shared microphone acquires once, both lifetime microphone
and driver miss deltas are zero, and the public transport generation and
512-frame mix history stay unchanged. Workgroup join succeeds without errors;
publication deadline misses remain zero in the observed interval. Matching
waveform correlations exceed 0.9998 and hardware-adjusted window medians are
12.5916–12.5924 ms. After editor preview closes, both providers actually close;
the next six-second microphone recording and eight-second app marker recording
restore at the same latency and transport generation. App markers also pass
under both original-playback policies. Intentionally absent app audio is not
counted as continuous app signal during its exit.

One preceding workgroup run has clean availability/deadline counters but fails
one waveform window. The offline matcher has a separately reproduced alias:
identical band-limited signals delayed by +137.40625 frames are incorrectly
matched at -64.09375 frames by the single decimated candidate. The corrected
analysis fractionally refines multiple coarse hypotheses, retaining the same
0.9 positive-correlation acceptance threshold. The regression covers several
band-limited mixtures and still rejects unrelated audio. Release, sanitizer
and backend-disabled tests pass. The preceding failed window remains unverified
because raw microphone audio was not saved; the fresh accepted recording uses
the corrected signed probe. No failed result is rewritten.

The accepted 40-second recording contains four isolated zero-valued samples,
with maximum run one. These are retained as observations, distinct from the
zero unavailable-frame deltas. Reported hardware delay is included; no analog
calibration or reliable 3–5 ms total latency claim follows from these tests.

The exact final workgroup candidate also repeats the 20-second cold affected
recording and idle optimization successfully with the corrected signed probe.
A fresh **minute at a 64-frame hardware/client period** simultaneously records
solo microphone, microphone/app mix and the downstream app-only dependency.
Both microphone recordings receive 2,880,000 frames with no callback/timestamp
errors or silent blocks. All three scene/driver miss deltas remain zero over
seconds 3.24–59.27. Both physical routes use linear phase and 512-frame history;
the mix's hardware-adjusted window medians are **12.9126–12.9200 ms**. Its
application budget is 448 frames and the 61 app marker windows report 11 ms at
1 ms resolution, with no signal/timestamp/queue errors. Each microphone
recording contains three isolated zero values, maximum run one. This is a
minute under the observed desktop load, not an external-load stress test.

These are bounded private acceptance cases, not all-load or all-app guarantees.
The app-timing policy remains off by default and uninstalled. Evidence:
`artifacts/application-timing-continuity-proof.json`. The earlier application
timing observations below remain historical evidence for their own signed
candidates; this section supersedes their open affected-mix lifecycle status.

## Verified application timing replaces the fixed budget, 20 October

An opt-in Engine policy now uses each admitted process tap's actual private
aggregate rate, buffer period, input device/stream/safety latency and observed
delivery. It waits for one second of valid callbacks, includes the maximum
observed callback size and positive late completion, and retains publication,
filter-support and scheduling reserves. Every stream of a multi-output source
must be verified; new, unavailable or partially known sources keep conservative
history. It never changes the source app's physical playback buffer or requests
permission. The normal default is still off:
`--automatic-application-timing true` selects the private policy.

The owned app's private tap actually delivers 128-frame blocks at 48 kHz,
roughly every 2.67 ms, with zero reported aggregate device/safety delay. Its
completed-block lateness is tens of microseconds in these observations. The
old blanket 1024-frame app budget is replaced by **352 frames** for 32-frame
rendering/lead, while the mixed microphone's linear filter requires 384 / 416
frames at 32 / 64-frame hardware periods. Real minute-long mixed acceptance on
the first signed candidate verifies:

| Hardware/client period | App history | Aligned mix history | Microphone in mix: hardware-adjusted median range |
| --- | --- | --- | --- |
| 32 frames | 352 frames | 384 frames | 9.923–9.927 ms |
| 64 frames | 352 frames | 416 frames | 10.907–10.913 ms |

These replace approximately 23.25 / 23.57 ms in the preceding fixed-app-budget
profile. Each microphone client receives 2,880,000 samples. All three source and
driver miss deltas are zero over the roughly 56-second steady observation
windows. Timestamp/callback/silent-block checks pass and the stereo app
dependency is continuous. Isolated zero values in the linear microphone
recording remain distinct from missing-frame counters.

The final signed candidate adds source-specific recovery for unsupported timing
and checks retained source history as well as render-clock storage. A large
latency report must not leave the graph permanently unable to replan: the
ordinary close/backoff/reopen path reacquires that app while preserving the
healthy microphone. Native regressions also prove that a pending timing
transaction leaves the committed graph and shared capture active until native
acknowledgement. Release, sanitizer and backend-disabled policy tests pass.

The exact final candidate passes fresh timed-marker checks. In the **normal
64-frame linear-phase renderer**, the app budget is 448 and the common mix is
512 frames: microphone delivery is 12.578–12.580 ms including reported hardware
delay. The application marker delay is 11 ms at 1 ms resolution. The fast
32-frame renderer measures microphone delivery at 9.909–9.918 ms and app marker
delay at 8 ms. Marker delay runs from the producer's intended presentation host
time to the virtual input timestamp; callback receipt offsets are reported
separately. It is not the same metric as hardware-adjusted microphone delivery.
The app-timing improvement therefore does not require minimum-phase input.

One unrelated microphone recording client stays open for **20 seconds across
app acquisition and timing activation**, in both the fast and normal profiles,
with no callback or timestamp errors or silent blocks. This certifies unrelated
input continuity; uninterrupted cold activation of an already-open affected mix
is still open. Lifetime tap diagnostics include one clock/counter correction
during setup, so do not infer zero setup transitions from the steady checks.
Real app/multi-output compatibility, original-playback mute/relaunch transitions
and broader load testing still precede default promotion.

No installed payload, driver, broker, default device or login setting changes.
All test documents/settings and their backups, the canonical Engine/desktop and
the 512-frame hardware setting are restored and reverified. Evidence:
`artifacts/application-timing-proof.json`. The minute runs bind to the first
candidate; the final candidate has its own short marker/20-second activation
evidence and the additional recovery/bounds regressions. Both remain uninstalled.

## Minute-long phase-preserving mixed acceptance, 20 October

The final private phase-policy Engine now passes **60 seconds at each 32/64-frame
hardware/client period** with simultaneous solo microphone, app/microphone mix
and downstream virtual dependency. The microphone provider is acquired once and
serves the solo minimum-phase and mixed linear-phase readers concurrently. The
app provider is also acquired once. Graph revision, active generation and driver
transport generations remain stable throughout the accepted observation windows.

| Hardware/client period | Solo hardware-adjusted median range | Mixed hardware-adjusted median range | Solo history budget |
| --- | --- | --- | --- |
| 32 frames | 7.978–7.983 ms | 23.247–23.248 ms | 288 frames |
| 64 frames | 8.983–8.994 ms | 23.568–23.581 ms | 320 frames |

These are driver-reported hardware estimates, not analog calibration. The mix
and virtual dependency retain the 1024-frame app budget. Each microphone client
receives 2,880,000 samples; the app dependency passes 61 stereo sine windows at
each period. The source/driver counter windows span approximately seconds
3.24–59.19 and 3.33–59.29, respectively, with zero new misses for all three
recording devices. Timestamp, callback, nonfinite and silent-block checks pass.
The solo microphone and app dependency have no post-warmup zero samples.
The mixed linear recording contains one isolated zero sample at 32 frames and
nine at 64 (maximum run one), while its physical reference has 2512 and 3235
zero samples. A zero value is not itself a missing-frame indication; the
availability counters report no misses. Do not describe these mixed recordings
as having no zero-valued samples.

The portable correlated-signal regression now tests all 25 source/render pairs
from 8, 44.1, 48, 96 and 192 kHz, two signal frequencies per pair and three
fractional host positions per case. Linear-selected physical and app inverse
signals cancel with at most 0.000009992 residual. The expanded test passes in
Release, with address/undefined sanitizers and with the macOS backend disabled.
This certifies the tested mixed-reader phase behavior; live hardware remains at
48 kHz and minimum-phase output-clock/local-correction quality is still open.

The first long harness reaches the end of capture but then checks a nonexistent
`runtime.serviceSession` field. That failure and its successful cleanup remain
retained. The corrected harness checks actual revision/active/transport
generations, uses the full steady interval rather than an early three-second
slice, and passes repeat minute-long checks at both periods. Saved documents,
settings/backups, original 512-frame buffer, canonical session, desktop and
installed payloads are restored and reverified. No physical outputs or new
permissions are used. Evidence: `artifacts/mixed-phase-sustained-proof.json`.

## Per-scene filter phase for mixed sources, 20 October

The optional causal input path now selects filters per prepared physical-input
binding. An isolated microphone can keep minimum-phase conversion. Mixing
independent physical, application or pass-through identities selects linear-phase
reads for the physical inputs involved. This requirement propagates into upstream
virtual devices, including out-of-order DAG definitions. Two routes from the
same physical identity retain the same causal filter; disabled sources/devices
do not introduce a phase requirement, while a muted enabled source remains
prepared for unmute. The ordinary default filter remains linear-phase.

One physical producer can serve a solo minimum-phase reader and a mixed linear
reader concurrently. Both immutable tables are prepared on control; selecting
the reader allocates nothing and changes no shared capture/filter state. The
shared registry's admission and stale-read rejection still gate both paths.
Automatic timing budgets use the selected filter's actual future support and
retain the largest verified budget for a hardware identity during disconnection.
Explicit diagnostic budgets also select phase-compatible filters, but still
require their own availability checks. Debug snapshots expose the selected
per-source filter; no saved document schema or driver change is involved.

A correlated 8 kHz signal and its inverse reproduce a 0.371215 peak residual
when a 0.2-amplitude physical path uses minimum-phase conversion against a linear
application path. The new linear selection reduces the residual below 1e-6 for
both a direct mix and a compensated virtual dependency. Independent causal reads
of the same physical history remain unchanged. Callback heap, filtered-read
reconnect races, shared-policy query deduplication, retained budget ordering and
timing-cache capacity regressions pass. Nine affected Release groups, eight
sanitizer groups and three backend-disabled portable groups pass; subsequent
dependency and timing-bound regressions also pass in their affected builds.

The final private signed Engine passes real simultaneous microphone, process-tap
and virtual-dependency recording at a 32-frame hardware/client period. Snapshots
verify minimum-phase solo input and linear-phase mixed input, each sharing the
same physical provider acquired once. All three steady source/driver miss deltas
are zero, both microphone recordings have no post-warmup zero samples or
callback/timestamp errors, and the dependent stereo app sine is clean. This is
a six-second observation; it does not replace sustained/load acceptance. The
prior minute-long solo recordings belong to the preceding candidate.
The final Engine is uninstalled and its existing microphone grant is retained.
Canonical session, desktop, empty graph/undo, document/settings backups and
512-frame hardware buffer are restored and reverified. Evidence:
`artifacts/mixed-input-phase-proof.json`.

This resolves converter phase selection within the prepared graph. Actual
analog/event-time alignment between hardware inputs and process taps still needs
calibration; wider rates, local-clock corrections, sustained mixed/load tests and
physical recovery remain open before normal default promotion.

## Measurement correction — driver timestamps and hardware delay

The historical recording figures below measure **physical-input driver timestamp
to recording-client receipt**, using waveform correspondence. They exclude the
input device and selected stream's reported hardware latency. They must not be
described as calibrated analog microphone-to-recording latency. This correction
changes the interpretation of the retained measurements, not their raw values.

The Zen currently reports 74 input-device frames and zero selected-stream frames
at 48 kHz: **1.542 ms** of additional hardware delay. Consequently the private
192-frame profile's 4.445–4.447 / 4.773–4.775 ms software measurements imply
approximately **5.987–5.989 / 6.315–6.317 ms** hardware-adjusted estimates.
These estimates use driver-reported hardware delay; an analog calibration has
not been performed. A total 3–5 ms recording target remains unproven.

The corrected probe reports both `driverTimestampToDeliveryMs` and
`hardwareAdjustedDeliveryAgeMs`, retains the legacy `deliveredSampleAgeMs` field
with its original definition, and records the physical device/selected stream
latencies and reference Audio Unit latency. A nonzero reference-unit latency
invalidates the hardware-adjusted estimate pending analysis. Safety offsets and
buffer periods are not added again to observed delivery time. Hardware facts
must remain stable across each recording. This follows the input timestamp
distinction described by [Apple's Core Audio engineer](https://www.mail-archive.com/coreaudio-api@lists.apple.com/msg01234.html).
Hardware facts: `artifacts/zen-input-hardware-latency-facts.json`. The analysis
regressions pass in Release and with address/undefined-behavior sanitizers.

The first corrected live probe reports **0.363 ms of reference AUHAL latency**
and withholds hardware-adjusted values. A no-IO format inspection traces this to
AUHAL's disabled playback bus: its private client format defaults to 44.1 kHz
while the device is 48 kHz. Matching that disabled bus's client format to its
hardware format makes global AU latency zero; both active input sides already
remain at 48 kHz. This changes no hardware format and enables no playback.
The probe now applies that normalization to its physical reference and verifies
the active input rates. Evidence: `artifacts/input-unit-format-latency.json`.

Fresh six-second automatic-policy recordings verify stable reference-unit
latency of zero, device latency 74 frames and selected-stream latency zero.
They report **7.988–7.991 ms at 32 frames** and **8.993–8.995 ms at 64 frames**
as hardware-adjusted delivery estimates; corresponding software values are
6.447–6.449 / 7.451–7.453 ms. Both source/driver miss deltas are zero and all
recording waveform/continuity and cleanup checks pass. These remain estimates
based on driver-reported hardware delay, not analog calibration. The earlier
approximately 5.99 / 6.32 ms arithmetic describes the narrower manual profile,
whose 64-frame reliability is rejected below. Evidence:
`artifacts/input-timing-policy-q32-normalized-reference-automatic-lead32-instrumented-result.json`.

That rerun also rejects the manual 192-frame / 64-frame-period profile: 99
steady source misses and 275 zero recording frames occur despite the earlier
minute passing. The 32-frame case has zero new source/driver misses. This new
failure supersedes any inference that the earlier minute proves the manually
tuned profile universally safe. Evidence:
`artifacts/recording-delay-192-lead32-instrumented-hardware-adjusted-minimum-phase-result.json`.
All device, document and buffer cleanup checks pass.

## Corrected passive comparison with Loopback, 20 October

The normalized reference probe is also rerun through Loopback 2.4.10 using the
same Zen microphone input 3, 48 kHz and virtual recording channel 1. Values below
are window-median **hardware-adjusted delivery estimates**, including the same
74 reported hardware frames for both products:

| Recording-client period | Loopback as configured | Private automatic LoopZ |
| --- | --- | --- |
| 32 frames | 19.116–19.117 ms | 7.988–7.991 ms |
| 64 frames | 19.437–19.441 ms | 8.993–8.995 ms |

Both products have six-second actual recording checks with no post-warmup zero
samples, callback failures or timestamp errors. LoopZ also has zero source and
driver miss deltas; Loopback's private counters are unavailable. Both reference
units report zero processing latency after format normalization. These are
driver-based hardware estimates, not analog calibration. Loopback's internal
physical-capture period is unknown and is not matched; the result describes
these observed configurations rather than an equally tuned product benchmark.
The private LoopZ candidate is uninstalled. The owned Loopback fixture and
original preferences are restored byte for byte, both Loopback devices are
disabled, the hardware buffer is 512 and audio defaults/Core Audio are unchanged.
Evidence: `artifacts/normalized-reference-loopback-comparison-proof.json`.

## Automatic causal-input policy and real mixed graph, 20 October

The private Engine's automatic timing policy now understands the selected
resampling filter. Minimum-phase input removes future-sample support from the
budget calculation while retaining measured hardware delay, input period,
rendering lead and scheduling reserves. Linear-phase behavior is unchanged;
unknown/disconnected devices retain conservative fallback timing. The explicit
physical-only diagnostic override still takes precedence, without shortening
application or pass-through history. Release and sanitizer checks cover timing,
physical-device policy, CLI selection and Engine runtime; portable timing tests
also pass with the macOS backend disabled.

Actual automatic mixed acceptance at 32 frames selects **288 frames** for the
solo microphone and keeps **1024 frames** for the app/mic mix and its virtual
dependency. Driver-timestamp delivery medians are 6.448–6.453 ms for solo input
and 21.766–21.771 ms for input in the mix. Both microphone recordings pass; the
app dependency has seven clean stereo sine windows with no channel leakage,
zero samples or timestamp failures. All three source/driver miss deltas are
zero. Hardware-adjusted values remain null for the reference-AU reason above.
This six-second simultaneous check restores both documents, hardware settings,
the canonical Engine session and desktop; it opens no physical outputs.
Evidence:
`artifacts/input-timing-policy-q32-minimum-phase-automatic-mixed-lead32-instrumented-result.json`.
Separate **60-second observations at each 32/64-frame period** also pass with
zero new source/driver misses, zero post-warmup zero samples and no callback or
timestamp failures. The automatic budgets are 288 / 320 frames and software
window medians are 6.447–6.452 / 7.442–7.444 ms, respectively. The minute-long
runs precede disabled-output reference normalization; their hardware-adjusted
fields remain null rather than being rewritten. The newer short recordings
above establish the corrected metrology. Evidence:
`artifacts/causal-input-policy-proof.json`.
The candidate remains uninstalled. Correlated-source phase alignment, wider
sample rates, physical recovery and sustained mixed/load acceptance remain open.

## Physical-only diagnostic budget in a mixed graph, 20 October

A newer private Engine accepts `--physical-input-delay-frames` independently of
the global source budget. With minimum-phase physical input, 192-frame input
history, 32-frame rendering/publication lead and 32-frame hardware/client periods,
simultaneous recording now verifies:

| Path | Selected history budget | Window median delivered age |
| --- | --- | --- |
| Microphone-only device | 192 frames | 4.454–4.456 ms |
| Microphone in app + mic device | 1024 frames | 21.781–21.783 ms |
| App → mixed device → virtual dependency | 1024 frames | Sine/channel/continuity pass; marker latency not measured |

The six-second microphone clients have no zero/silent samples, callback or
timestamp failures. All three steady source/driver miss deltas are zero. The app
dependency has seven passing stereo sine windows with no channel leakage,
timestamp jumps or zero samples. Physical input and app capture each acquire
once despite shared routes. The desktop remains closed during private session
replacement; an authenticated 45-second debug preview lease starts acquisition
before recording clients open. Its disconnect releases preview demand.

Native policy/CLI groups pass in Release and under address/undefined sanitizers;
the exact signed Engine also passes capture-free CLI checks. A prior harness
wait without preview/public demand times out correctly with zero acquisition
attempts and is retained. All routing, backup, hardware settings, canonical
session, desktop and test undo state are restored and reverified. Evidence:
`artifacts/physical-input-budget-proof.json`. The new Engine is uninstalled.
Generic automatic timing, correlated-source phase alignment, broader rates and
long mixed sessions remain open; a fast microphone setting cannot shorten the
app's required capture history.

## Fresh passive microphone comparison, 20 October

The same signed input-only probe now measures Zen microphone input 3 → virtual
input 1 through Loopback 2.4.10, the installed LoopZ pair and the private causal
input profile. All use 48 kHz and the indicated recording-client period.

| Client period | Loopback, observed configuration | Installed LoopZ | Private LoopZ profile |
| --- | --- | --- | --- |
| 32 frames | 17.543–17.563 ms | 11.031–11.038 ms | 4.445–4.447 ms |
| 64 frames | 17.893–17.894 ms | 11.383–11.385 ms | 4.773–4.775 ms |

Values are window medians of actual delivered sample age. Loopback's internal
source buffer is **not measured or matched**; it runs as configured. The private
LoopZ profile is explicitly tuned and remains uninstalled. Loopback and installed
LoopZ runs last six seconds per period; private LoopZ runs last sixty seconds.
This comparison describes this route/settings combination, not universally
equivalent internal tuning or product performance.

Loopback's recording clients each receive 288,000 samples with zero post-warmup
zero/silent samples, timestamp errors, invalid blocks or callback failures.
Private backend source/drop counters are unavailable. Only the pre-existing owned
comparison fixture is changed: one physical source, channel 3 → virtual channel
1, no app sources or monitors. Original preference bytes, both disabled device
definitions, running GUI choice, 512-frame observer/Engine settings, default
devices and Core Audio identity are restored and verified. Restoration verifies
the device's withdrawal; [quitting Loopback alone leaves devices configured](https://rogueamoeba.com/support/manuals/loopback/?page=devicebasics).
Evidence: `artifacts/physical-loopback-comparison-proof.json`.

## Updated installation and live causal-input acceptance, 20 October

The pass-through prefix fix is installed as driver manifest
`bf2ce02f4956035a0bcbaa8ae0415c98460d2d7e921774f7f6ddc277fe105ff1`.
The subsequent ordinary user app update installed recovery/Settings pair
`2d0c9b014a44e4c1111892bf0f40336b209b3bff7380ee0c6b6431f7a09e133c`
without sudo. Actual driver/app hashes, account ownership, signatures, restored
normal session and existing microphone grant are verified. The installed input
path passes fresh six-second checks with muted controls at 32 and 64 frames,
with zero new source or driver misses.

A separately signed, uninstalled Engine uses minimum-phase physical input
history, 32-frame rendering, a 32-frame publication lead and an explicit
192-sample source budget. Passive Zen input 3 → virtual input 1 recording checks
run for **60 seconds at each hardware/client buffer**:

| Hardware/client buffer | Window median delivered age | Largest window p95 | New source/driver misses |
| --- | --- | --- | --- |
| 32 frames | **4.445–4.447 ms** | 4.744 ms | 0 / 0 |
| 64 frames | **4.773–4.775 ms** | 5.369 ms | 0 / 0 |

Each client receives 2,880,000 samples with no post-warmup zero/silent frames,
timestamp errors, invalid blocks or render failures. Waveform correspondence is
checked at seconds 1, 20, 40 and 58; actual callback receipt supplies delivered
age. Complete steady-state observations have zero source and driver miss
increments. Physical output is never opened and raw microphone audio stays in
RAM. This proves bounded microphone-only IO for these exact private profiles,
not a universal setting or an installed default.

The longer 192-sample / 64-frame-publication-lead profile passes at 32 hardware
frames but fails at 64, with 1,053,214 missing source frames and zero driver
misses in its bounded steady observation. It remains rejected. A first long-run
launch also collided with the desktop's normal-session supervisor before IO;
the controller now gracefully closes only the verified desktop during private
session replacement and reopens it after restoring the canonical job. Both
failed rehearsals are retained.

The installed Engine, desktop, original empty document and backup, empty test
undo history and saved/applied 512-frame buffer are restored and verified.
Defaults, disabled login choice, broker and original Core Audio process remain
intact. Evidence: `artifacts/post-driver-delivery-proof.json`. Generic automatic
minimum-phase timing, mixed-source alignment, wider rate/clock changes, longer
load testing and a matched-internal-buffer Loopback comparison remain open. The
fresh as-configured comparison is recorded above. Normal
installed recording measures approximately 11.04 / 11.38 ms at 32 / 64 frames.

## Experimental causal conversion, 20 October

The portable history reader has an optional minimum-phase prototype. A private
native input mode can now select it, but normal captures retain the existing
filter and no experimental Engine is installed. Its fixed-rate tests require no samples
after the final requested source coordinate. Sampled spectral checks at six rate
pairs (8–192 kHz endpoints) have less than 0.001 dB passband ripple and more than
90 dB stopband rejection in the tested downsampling bands. Separate reader tests
cover 1–64 channels, integer-phase crossing at a fixed corrected rate, strided
buffers and missing support without callback heap activity. The existing default
linear-phase and causal-output tests still pass.

Changing the filter cutoff with every measured source-clock slope caused an
extra phase discontinuity. The optional history mode now pins the filter shape
to the nominal source format while sample positions follow the measured clock.
Tests cover ±200 ppm source-clock changes, closed-interval slopes of 32/31 and
32/33, and one-sample callback timestamp corrections at 8, 48 and 192 kHz.
Those history reads have zero missing frames and no instrumented heap activity.
Sampled rejection remains greater than 90 dB across ±0.5% source-clock drift
with a fixed output clock. Malformed nominal formats fail silently without
reading the provider; the default linear reader ignores the experimental hint.
All five affected test groups pass in Release, under address/undefined
sanitizers, and with the macOS backend disabled locally. These are offline
checks, not native capture or Windows/Linux acceptance.

This uses the [real-cepstrum minimum-phase construction](https://www.dsprelated.com/freebooks/sasp/Minimum_Phase_Causal_Cepstra.html)
on the existing windowed-sinc magnitude design. Its nonlinear phase changes group
delay, so a direct-read shortcut cannot be used at integer phases. These isolated
DSP checks alone do not establish a shorter safe native queue, a 3–5 ms recording path, output-clock
cutoff transitions, alias rejection across large local timestamp corrections,
mixed-source alignment or a new Loopback comparison. Evidence and remaining
integration checks: `artifacts/minimum-phase-prototype-proof.json` and
`artifacts/minimum-phase-clock-proof.json`.

The private Engine flag is `--experimental-input-filter minimum-phase`, together
with an explicit `--source-delay-frames` diagnostic budget. `CaptureSourceOptions`
passes the selection into physical input history only; application taps retain
their existing reader. The runtime does not infer a shorter safe input budget
from this option. Both native construction modes (1/2/8/64 channels), invalid
selection guards, capture-free CLI reporting and Engine runtime regressions pass
in Release and under address/undefined sanitizers. The separately signed private
Engine retains the existing microphone grant without requesting access, and its
exact bytes pass the capture-free CLI checks. This preparation evidence predates
the live acceptance above:
`artifacts/minimum-phase-native-proof.json`.

A separate Release-only processing benchmark uses the actual `AudioHistory`
reader with prepopulated data, +200 ppm source drift and optional one-sample
timestamp corrections. On this Apple M2 Max, 16-channel minimum-phase reads at
32 frames / 48 kHz take 45.8–46.0 µs median, 68.8–69.3 µs at the 99th percentile,
and at most 508.3 µs across these two 1,000-read cases. The nominal block period
is 666.7 µs. Across all 48 cases (2/16/64 channels, 32/64 frames, 48/44.1 kHz
output and both filters), no retained-history reads are missing; one 64-channel
corrected 44.1 kHz case has one read longer than its block period. These are
ordinary-thread elapsed times, not real-time deadline guarantees. The benchmark
does not include a concurrent producer, the whole graph, native IO or OS
scheduling stress. Evidence: `artifacts/input-history-benchmark-proof.json` and
`artifacts/input-history-benchmark-result.jsonl`. The optional portable target is
`loopz-input-history-benchmark` with `LOOPZ_BUILD_BENCHMARKS=ON`.

## Installed adaptive physical-input timing, 20 October

The signed installed Engine now derives physical-input history requirements from
the input's actual buffer period, device/safety/stream latency, sample-rate ratio,
sinc support and rendering lead. This replaces the fixed 1,024-frame input delay
when verified physical-device facts support a smaller budget. The normal
background session retains its 64-frame render quantum and 64-frame publication
lead. Application and pass-through budgets still participate in mixed-device
alignment; enabling either can correctly increase the common device delay.

| Zen hardware and recording-client buffer | Earlier fixed input budget | Installed adaptive input budget |
| --- | --- | --- |
| 32 frames | approximately 21.71 ms | **11.047–11.050 ms** |
| 64 frames | approximately 22.04 ms | **11.365–11.369 ms** |

These are microphone input 3 → virtual input 1 recording-client delivery ages at
48 kHz, measured from matched physical acquisition to actual callback receipt,
not virtual advertised timestamps. Each installed case has six seconds and four
waveform windows, with zero new scene-source or driver-capture misses after
warmup. A separate 60-second run of the identical signed Engine at 32 frames
measures 11.040–11.048 ms, with the same two miss counters remaining unchanged.
The offline waveform matcher applies the same centered 6 kHz low-pass to both
streams to stabilize fractional alignment against high-frequency noise. This
does not filter product audio or add relative analysis delay. Synthetic known-delay
and strong high-frequency interference tests calibrate that measurement path.

The same candidate retains **186 samples / 3.875 ms physical monitoring** at
gain 0.5 in all six active windows, with two separate muted controls. Monitor
missing-frame, graph-failure, invalid-block, timestamp-error, discontinuity and
xrun deltas are all zero. Monitoring and virtual recording are separate paths.
Evidence: `artifacts/input-timing-installed-result.json` and
`artifacts/input-timing-policy-q64-candidate-acceptance-lead64-instrumented-long-result.json`.

A temporary 32-frame renderer, not the installed session configuration, measures
approximately 9.05/10.04/12.06/24.14 ms at hardware/client periods 32/64/128/512.
The Zen reports 138 input-latency frames including stream latency. The automatic
policy includes this fact and a scheduling reserve; the earlier manually tuned
7.04 ms experiment below is not the automatic production policy.

Mixed acceptance combines real app capture, the physical microphone and a
downstream virtual dependency. Its shared 1,024-frame application budget remains
intact, microphone recording measures approximately 21.71 ms, and an actual
stereo sine reaches the dependent device. Steady-state source and driver miss
deltas are zero. See
`artifacts/input-timing-policy-q64-mixed-lead64-instrumented-result.json`.

The latest interface creates new mixes empty. Pass-through is an explicit Add
Source choice, and new source connections require explicit wiring. Existing
documents retain their stored connections. Earlier empty-mix interface manifest:
`15368dd7bb83c5371732e729c81c70afac33543e10ee73052f364c185af3c777`.
Its Engine is byte-identical to the measured signed candidate
`0e365cf0d7fd3fab0dea01ea34d9c9f0d63351a208984f9712e1c86d647e56b0`.
The interface update preserves the Engine process and requires no administrator
access. Temporary routes and test undo history are removed, the saved hardware
buffer is restored to 512, and Core Audio, defaults, driver, broker and disabled
login startup are preserved. See `artifacts/input-timing-delivery-proof.json`.

These bounded checks do not certify every device, long-term load or disconnect
scenario. The 3–5 ms virtual-recording target and fresh matched Loopback comparison
remain open.

## Earlier recording-client diagnosis, 20 October

The installed 3.875 ms result below measures physical monitoring. A new passive
two-input diagnostic separately measures Zen input 3 → LoopZ virtual input 1.
It matches the microphone waveform between independent physical and virtual
input clients, then subtracts the matched physical acquisition time from the
virtual callback's actual receipt time. It does not use the virtual device's
advertised timestamps as the latency result. Both streams are input-only; no
speaker output or permission request is opened, and raw audio stays in RAM.

| Installed Engine and recording-client buffer | Median delivered sample age |
| --- | --- |
| 32 frames | approximately **21.71 ms** |
| 64 frames | approximately **22.04 ms** |

Each baseline has a six-second capture, four waveform windows, a separate muted
control, continuous sample counters and no increase in scene source misses after
startup. The normal graph uses 64-frame render blocks, a 64-frame publication
lead and a 1,024-frame source delay. A small hardware buffer alone does not
remove that 21.33 ms source delay. Evidence:
`artifacts/installed-recording-path-result.json`.

Lower-delay experiments expose two distinct failure boundaries:

* Reducing the source budget below the input-delivery and resampling support
  requirement increases scene source misses. The current symmetric sinc kernel
  has radius 96, requiring roughly 103 future source frames near equal rates.
  Those samples can be unavailable even when hardware sample counters are
  continuous. The 224/192/160-frame trials fail, and the 288-frame/64-frame-lead
  profile also fails the longer check despite initially clean waveform windows.
* Removing the publication lead can leave driver capture reads ahead of the
  Engine's completed output. An instrumented 256-frame/zero-lead run keeps its
  scene source count constant while driver capture misses rise from 6,207 to
  21,877 across successive observations. A matching 1,024-frame/zero-lead
  control has zero new driver capture misses. Source health alone does not
  certify public recording continuity.

The apparent 5.71 ms / 60-second result is **rejected**: its earlier checker
missed short zero bursts because there were no entirely silent callbacks and
the scene source count stayed steady. Exact zero-valued microphone samples can
also occur naturally, so individual zeros are not classified as dropouts.
Authenticated runtime snapshots now expose the transport generation and the
driver's existing capture/playback read/miss counters separately from scene
source misses. These diagnostics are implemented and fixture-tested in the
repository and a private signed Engine at this checkpoint; the later installed
adaptive-timing release above now includes them. Counters
remain decimal strings to preserve all 64 bits.

See `artifacts/recording-delay-instrumented-lead0-result.json`,
`artifacts/recording-delay-256-long-result.json`, and
`artifacts/recording-delay-instrumented-lead64-288-long-result.json`.
All experimental profiles are temporary and preserve the installed applications.

The conservative private profile passes a 60-second check at 32-frame physical
and recording-client periods, 32-frame graph blocks, a 320-frame source delay
and 64-frame publication lead. Median actual delivered sample age is
**7.044–7.048 ms**, with per-window p95 at 7.343–7.344 ms. The two clients each
receive 90,000 callbacks with continuous sample counters and no render failures.
Steady-state scene source misses and driver capture misses both increase by
zero; the driver accounts for 2,742,144 frames between the bounded steady-state
snapshots. The virtual recording has no silent blocks; its six individual zeros
are isolated, with maximum zero run one. Waveform correlation is above 0.987
in the windows at 1, 20, 40 and 58 seconds. This improves the measured recording
path by about three times, but is **not installed**, is not yet a general timing
policy for mixed application/hardware graphs, and does not achieve 3–5 ms.
Evidence: `artifacts/recording-delay-320-lead64-instrumented-long-result.json`.

A fresh matched Loopback hardware comparison remains open; Rogue Amoeba's
[published latency guidance](https://www.rogueamoeba.com/support/knowledgebase/?product=Loopback&showArticle=AppLatency)
is not a measurement of this Mac and this 48 kHz route.

The acquisition/receipt distinction follows Apple's
[AudioDeviceIOProc timestamp contract](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc?language=objc).
Further reduction must preserve filter quality and both availability boundaries.
Candidate work includes coordinating renderer deadlines with IO using
[Audio Workgroups](https://developer.apple.com/documentation/audiotoolbox/understanding-audio-workgroups)
and evaluating a converter with less future-input support; Apple's
[minimum-phase converter](https://developer.apple.com/documentation/audiotoolbox/kaudioconvertersamplerateconvertercomplexity_minimumphase)
is an API reference, not an implemented or accepted LoopZ solution.

## Saved-buffer Engine recovery, 20 October

Installed manifest `2162f9036f430cf24015711027c97cff4629a13e26486d5a5f53d10d24105cba`
now saves and restores the Engine's device buffer choices. A forced replacement
of only the verified user-owned Engine restored the complete three-device graph
and its 32-frame setting in 588 ms. The same physical mic3 → monitor1/internal
return9 route measured 186 samples / 3.875 ms in all three waveform windows before
and all three after the restart, with a separate muted control and gain 0.5.

The two-device stereo virtual dependency chain retained its existing public
clients, device/stream identities and clock origins. It had 464 ms of silence
during the deliberate Engine crash; stable pre/post checks have zero silent
frames and timestamp errors. These are separate restoration and steady-state
latency measurements. The test graph is removed and 512 frames restored and
saved. See `artifacts/device-settings-delivery-proof.json` and its native IO,
waveform, signed-payload and policy-test evidence.

## Installed Engine acceptance after migration, 20 October

The installed pair now passes the same-device microphone monitoring check with
manifest `2ca62765093c9db02634ab4c84100f12d9bb87c4208bbdf11c1f352eb9108fd0`.
The ownership migration was verified, and this subsequent app-only fix installed
as UID 501 without sudo. Driver/broker contents and the Core Audio process stayed
unchanged.

| Actual Engine IO buffer | Measured waveform delay | After-warmup continuity |
| --- | --- | --- |
| 32 frames | 186 samples / **3.875 ms** | 60-second observation, zero reported source/monitor missing frames, graph failures, hardware timestamp errors, discontinuities or host xruns |
| 64 frames | 250 samples / **5.208 ms** | Same checks pass |

Each period has two four-second recordings and six agreeing half-second windows,
with correlation above 0.9999. Separate muted controls return no correlated
signal. The route is Zen mic input 3 → installed Engine device/monitor DSP →
monitor output 1 → internal return input 9, at 48 kHz and monitor gain 0.5.
It excludes loudspeaker/air/analog paths outside that return and does not measure
the virtual recording path. A fresh matched physical Loopback comparison is
still outstanding.

The first installed attempt exposed a separate settings defect. Setting the
diagnostic/UI process to 32 frames left the Engine and reader at 512, measuring
23.875 ms. A two-process readback confirmed different buffer values; these runs
remain failed buffer-application evidence, not successful small-buffer tests.
Settings now reads the Engine's hardware inventory and sends its bounded,
expected-current Apply request through authenticated XPC to the Engine control
owner. Actual monitor `bufferFrames` must match during every observation.
The later saved-buffer update above closes the Engine-restart persistence gap.

Both test devices were removed, the empty original document was restored, and
the Engine buffer returned to 512. Defaults and Core Audio PID/start identity
are unchanged. No raw microphone recording was saved. Evidence:
`artifacts/engine-buffer-installed-latency-result.json`,
`artifacts/engine-buffer-user-update-verified.json`, and
`artifacts/zen-buffer-process-scope.json`.

## 32/64-frame investigation and Engine integration, 20 October

The same-device monitor optimization now runs in the actual EngineRuntime /
MonitorOutputs implementation in a private signed test, using both device and
monitor DSP stages. Zen input 3 → monitor output 1 → internal loopback input 9
measured **186 samples / 3.875 ms at 32 frames**, and **250 samples / 5.208 ms
at 64 frames**, at 48 kHz. Two recordings per period give six agreeing waveform
windows per period (correlation >0.9999). Muted controls return no signal.
Each 24-second Engine run reports zero monitor missing frames, hardware-frame
errors, discontinuities and host xruns. These results exclude speaker/air and
analog differences outside the internal return, and do not measure the virtual
recording path. This private evidence is now supplemented by the installed
acceptance above.

The capture investigation identified a clock-mapping bug: roughly 22–26 µs
host-time projection corrections can occur while the source sample counter
remains continuous. At a 32-frame period, these corrections exceed the old
±0.5% per-interval rate check. It rejected the entire interval and emitted
33 silent/missing output frames. The same behavior rejects about 65 frames at
64-frame buffering. This is not evidence that the hardware failed to deliver
those samples. A direct HAL comparison reproduces the clock corrections with
continuous sample counters, isolating the mapping issue from AudioUnitRender.

The portable history now marks intervals whose source counters prove exact
continuity. Only those closed intervals admit a bounded correction (at most two
sample durations, local rate within 0.8–1.25 of nominal) and retain bandlimited
conversion. Unverified intervals and large timestamp jumps remain invalid;
a real source-counter gap still resets the history generation. A correction's
local slope is never extrapolated into future samples. Regression tests cover
both directions, 32/64 frames, 8/48/192 kHz, signal positions, real gaps and
allocation-free callback use.

Earlier capture-counter jumps were also observed during stream teardown. The
probe now ends delivery observations before shutting down other audio clients,
while joining all callbacks before reading the diagnostic snapshot. Whole-path
acceptance additionally requires zero real capture-counter gaps and zero monitor
hardware errors; a source-missing count alone cannot pass this check.

Fresh bounded live results and their installed/staged distinction are recorded
in `artifacts/input-clock-investigation-result.json`. Original hardware buffering
is restored to 512 after every run. No saved mix, audio defaults, installed
driver/broker or global Core Audio restart is involved.

Research references: Apple's [AUHAL input note](https://developer.apple.com/library/archive/technotes/tn2091/_index.html)
describes input callbacks and propagating their timestamps to AudioUnitRender;
the [AudioDeviceIOProc contract](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc?language=objc)
distinguishes acquisition time from callback scheduling time. Apple's
[audio workgroup guidance](https://developer.apple.com/documentation/audiotoolbox/understanding-audio-workgroups)
explains deadline misses and automatically managed framework threads. The
measured timestamp corrections above are observations from this Zen, not a
guarantee in those documents.

## Direct hardware-clock prototype and buffer settings, 20 October

A private signed prototype now measures **3.875 ms (186 samples at 48 kHz)**
from Zen mic input 3 through the portable SingZ routing graph to monitor output
1 / hardware loopback input 9. Three half-second waveform windows agree, with
correlations above 0.9998; a separate muted control has no returned signal.
Two further recordings confirm six more windows at exactly 186 samples. The
20-second repeat additionally verifies zero hardware-frame timestamp errors,
reported hardware discontinuities and host xruns.
Both bounded 16-second runs completed without invalid graph callbacks or
nonfinite output. Hardware buffering was 32 frames; gain was 0.5. This is the
same internal-return measurement scope as the installed 44.708 ms baseline,
excluding analog/speaker/air differences outside that return.

**This is a prototype, not the installed Engine's monitoring path.** It uses
one duplex hardware clock and avoids asynchronous histories for this simple
same-device microphone route. Production integration must preserve full device
and monitor controls, metering, recovery, staged edits and the existing path for
application capture, virtual sources and independent hardware clocks. No public
virtual device was published by the prototype.

Shrinking the existing independent queues alone is not accepted: a 32-frame
hardware / 448-frame source / 192-frame monitor profile measures 14.042 ms but
records missing source frames during longer tests. Smaller shared periods also
expose occasional timestamp-anchor jumps; delivery diagnostics now distinguish
those from source sample-counter discontinuities. The final input-only check
records one source-counter discontinuity and 33 missing frames even with a
1,024-frame source history, so this cannot be fixed by shortening queues alone.
Failed continuity profiles
remain failed evidence, regardless of their measured delay.

Settings now has a hardware-device/buffer selector and an explicit Apply action.
It reads actual supported ranges and current values, shows per-buffer duration
without calling it total latency, disables writes on stale discovery, and sends
the expected previous value with each one-shot request. The native backend
rejects unavailable/self devices, invalid sizes and stale selections. Synthetic
native WebKit checks and backend regressions pass; the actual Settings backend
also changed the Zen 512→128→512 successfully. The signed app pair is staged in
`artifacts/hardware-buffer-settings-signed-stage.json`, not installed.

The user authorized smaller shared-buffer experiments and deletion of the old
mix. Every completed experiment restored and verified the original 512-frame
period. The old mix was removed through authenticated debug controls. Audio
defaults, the installed backend and global Core Audio were not changed.
See `artifacts/physical-direct-latency-32-monitor.json`, its `-engine.jsonl`
companion, muted controls and `artifacts/hardware-buffer-settings-live.json`.

## Earlier installed microphone monitoring: Zen Quadro, 2026-10-20

The installed Engine measures **44.708 ms (2,146 samples at 48 kHz)** from
Zen Quadro microphone input 3, through a temporary microphone-only LoopZ mix,
to monitor output 1 as observed on the Zen's internal loopback input 9.
This is a waveform measurement on simultaneous channels of the same hardware
input clock. It excludes the loudspeaker, air path and analog converter
differences outside the internal return. It is not the app-only path below.

Four four-second monitored recordings at monitor gain 0.5 produced the same
2,146-sample match in 10 of 12 half-second analysis windows. Two noisy windows
were inconclusive. Four monitor-muted control recordings produced no matching
peak at that delay. All eight recordings passed capture validation with zero
timestamp errors, invalid frames or nonfinite microphone samples. The diagnostic
also passed synthetic pulse and waveform checks, including a known 2,203-sample
waveform offset that is not aligned to its coarse search grid.

Runtime reports 1,024 frames of source history and a 1,088-frame monitor queue:
44 ms of configured buffering. That accounting explains most of the measured
delay; it is not used as the measurement. Earlier 3–5 ms application-capture
results do not establish low-latency physical microphone monitoring.

The test used authenticated Engine controls, without interface clicks, another
installation, privacy prompts or a Core Audio restart. Temporary devices were
removed; the original document, public device catalog and audio defaults were
preserved. Raw microphone recordings stayed in memory. The proposed unity-gain
test was rejected by automatic approval review for feedback risk and did not run;
confirmation retained gain 0.5. Evidence and per-window results are in
`artifacts/zen-mic-monitor-latency-result.json`.

## Repeated matched input-only comparison

`artifacts/matched-input-comparison-proof.json` records two fresh, sequential
20-second measurements per product using the same signed input-only reader and
the same generator identity/binary within each pair. Both used stereo 48 kHz,
32-frame source and reader requests, original-playback mute, no pass-through and
no physical monitoring. The repeat explicitly records actual generator callbacks
at 32 frames; both initial and repeated readers report actual 32-frame callbacks.
One additional input-only reader held acquisition during each timed observation.

| Product / recorded configuration | First run | Repeat |
| --- | --- | --- |
| Signed LoopZ workspace candidate | 3 ms | 3 ms |
| Installed Loopback 2.4.10 / ARK 13.1.3 | 16 ms | 15 ms |

These are approximate timestamp-marker delays with 1 ms search resolution.
Each run contains 19 analyzed one-second windows after warm-up. All four timed
readers pass with zero silent samples, bad windows, timestamp/callback failures,
nonfinite samples or checker overruns. The generator's final counters confirm
32-frame callbacks with no source timestamp/render failures in the repeat.
The separately held demand readers acknowledge cancellation and buffer-period
restoration; their interrupted startup-inclusive runs are not claimed as passing
timed measurements.

LoopZ uses a source-only endpoint (no public output), a 32-frame graph/private
capture request, zero render lead and 160-frame source history. Its ordinary
defaults are unchanged. The earlier 5 ms/60-second result used a duplex reader
and a zero-gain pass-through branch, so it is a different measurement path.
The new diagnostic opens only input and feeds both products into the same
portable `StreamCheck`; it requires a virtual device and an existing microphone
grant, restores the period it changed and joins callbacks before reading their
counters. It never requests privacy authorization or changes a physical device.

The comparison uses an owned virtual-output generator and the recorded Loopback
configuration, not a physical acoustic path or a general ranking of all apps,
rates and hardware. Source processes/endpoints were freshly launched for each
phase rather than being one continuously running process across products.
Long-session, other app/period/rate, microphone/monitor and compatibility gates
remain open. The LoopZ candidate is uninstalled.

After each pair, the comparison device was disabled again. The original generator
files were restored byte-for-byte, Loopback configuration was preserved (excluding
derived selection/last-used/bookmark cache data from the semantic hash), temporary
LoopZ devices/Engine processes were withdrawn, and audio defaults, installed
files, broker and Core Audio identity remained unchanged. The first Loopback
fixture attempt waited for an intentionally disabled LoopZ destination; it
opened no generator or reader IO, was cleaned up and is retained as a failed
fixture record. Only enabled endpoints are expected by the corrected fixture.

## Delivery measurements and the 32-frame application candidate

`artifacts/capture-delivery-proof.json` records a new signed workspace candidate,
its exact profile and passing/failing measurements. Capture diagnostics now
measure actual callback sizes, receipt spacing and history publication timing.
The earlier 128/64-frame requests produced actual 128/64-frame callbacks; in the
recorded four-case sweep, publication completed between about -0.063 and
+0.516 ms relative to each block's exclusive end timestamp. Callback batching,
render scheduling and the converter's unchanged future support therefore need
to be considered together; a connected source or zero aggregate latency property
does not prove a safe history delay.

The newer candidate supports a 32-frame private capture request and 32-frame
scene/scheduler/monitor preparation. Its experimental stereo 48 kHz profile uses
32-frame source and reader clients, a 32-frame graph, zero render lead and
automatic public timing. The destination still has a zero-gain pass-through
route. Filters and normal defaults are unchanged.

| Source history delay | Eight-second result | Marker delay |
| --- | --- | --- |
| 128 frames | Failed: missing audio | Not a valid latency result |
| 160 frames | Passed | 5 ms |
| 192 frames | Passed | 6 ms |
| 224 frames | Passed | 6 ms |
| 256 frames | Passed | 7 ms |

The 160-frame profile then passed a 60-second check: 59 analyzed one-second
windows, zero silent samples, bad windows, timestamp errors, queue overruns or
render failures. The observed render counters had zero late/skipped frames,
reanchors, graph failures and sink failures; maximum render execution was about
87 microseconds. Actual capture callbacks were 32 frames, with zero history
publication rejections and maximum capture processing of 7.5 microseconds. After
one second of capture warm-up, exclusive-end publication age ranged from about
-0.060 to +0.398 ms and callback spacing from 0.268 to 1.067 ms. Signed negative
ages preserve timestamps that lie slightly in the future.

The marker search resolution is 1 ms: **5 ms is approximate**, not a hard upper
bound or an acoustic measurement. Separate acquisition readers preserve demand;
their startup is not passing steady-continuity evidence. The 60-second check
does not replace mixed-source two-hour stability, broad app/period/rate testing,
microphone/physical monitoring or a fresh matched Loopback comparison. Both the
short sweep and longer check withdrew their private devices and stopped owned
processes; installed files, defaults, broker and Core Audio were preserved.
The candidate remains uninstalled and is not the desktop's default profile.

## Application capture after the installed timing update

The installed Engine was tested with an owned 64-frame virtual source, stereo
48 kHz app capture, a 64-frame reader/graph/render lead and automatic public
timing. The destination retained a pass-through source at zero gain, so its
safety floor remains part of the measured graph. Each steady check ran eight
seconds after a separate demand reader acquired the source and a two-second
settling interval. Signal analysis excludes the first reader second.

| Engine / private capture request | Source history delay | Result |
| --- | --- | --- |
| Installed / 128 frames | 128 frames | Silence; failed |
| Installed / 128 frames | 256 frames | Missing samples; failed |
| Installed / 128 frames | 512 frames | Passed, 13 ms |
| Installed / 128 frames | 1024 frames | Passed, 25 ms |
| Workspace candidate / 128 frames | 512 frames | Passed, 14 ms |
| Workspace candidate / 64 frames | 128 frames | Missing samples; failed |
| Workspace candidate / 64 frames | 192 frames | Silence; failed |
| Workspace candidate / 64 frames | 256 frames | Missing samples; failed |
| Workspace candidate / 64 frames | 384 frames | Passed, 11 ms |
| Workspace candidate / 64 frames | 512 frames | Passed, 13 ms |

That initial workspace candidate exposed `--application-buffer-frames 64|128` for independent
diagnostic selection of each private process-tap aggregate's request. This does
not change the application's hardware output period or the default request of
128 frames. All capture groups receive the chosen immutable factory option,
including after reacquisition. Actual capture configuration logs confirm the
64-frame property; they do not establish callback delivery timing or availability
of the newest history samples. The non-monotonic failures at the smallest delays
are retained rather than treated as a minimum-latency guarantee.

`artifacts/application-buffer-proof.json` identifies the exact installed and
signed candidate binaries, full passing/failing measurements and cleanup.
Passing spans had zero silent samples, timestamp/render failures and bad signal
windows. The earlier acquisition reader's startup is recorded separately and
does not count as a passing continuity test. Defaults, installed files and Core
Audio identity stayed unchanged. No physical devices or new privacy prompts
were involved. This establishes neither a stable 3–5 ms application profile nor
a fresh matched Loopback comparison. Callback delivery/available-history timing,
repeated startup phases and sustained lower-queue validation remain necessary.

## Continuous clients during Engine restart

Engine crash interruption is measured separately from steady-state audio delay.
`artifacts/continuous-recovery-installed-proof.json` records four passing checks
with the installed driver and a privately staged signed Engine. Original
2/8/64-channel clients, including a virtual-device chain, stay open through each
crash. Two tests after a twelve-second session settling period measured about
296 and 312 ms of silence; the latter observed a replacement process after
219 ms and an authenticated full-graph acknowledgement after 430 ms, using a
200 ms controller polling interval. Early-session checks had multi-second gaps
under launchd's configured crash throttle.

The recovery checks use 48 kHz, 64-frame graph/clients and automatic pass-through
timing. Their independent healthy marker intervals remain approximately
6–7 ms and contain no silent samples or timestamp/render errors. This is a
different client path and profile from the matched 32-frame application-input
comparison. It does not measure app/microphone/monitor crash recovery or acoustic
latency. Installed files, defaults and the Core Audio process remain unchanged.

## Installed callback timing and short virtual pass-through checks

The reviewed existing-driver update is installed with manifest SHA-256
`267c8ffca51696d9cacc942fde13bc921ea413669241df24772e7586ffa3f7a4`.
`artifacts/driver-update-installed-proof.json` records independent installed-file
verification, the administrator report and subsequent actual HAL checks.
Output-only clients reported the requested 512, 32, 64, 128 and 64 callback frames
in five successive driver sessions; each smaller session replaced the preceding
session's high-water timing. No input or permission request was needed for that
telemetry test.

Stereo 48 kHz digital pass-through marker tests then used a 64-frame graph and
64-frame render lead. Each test ran for eight seconds, excluding its first
second from signal analysis. The delay search has 0.25 ms steps.

| Client buffer | Explicit 128-frame queue | Automatic queue |
| --- | --- | --- |
| 32 frames | 3.25 ms | 4.25 ms |
| 64 frames | 5.25 ms | 6.25 ms |
| 128 frames | 7.75 ms | 11.25 ms |
| 512 frames | 23.75 ms | 44.00 ms |

All eight short probes passed channel/gain/correlation checks, with zero silent
samples, timestamp errors, nonfinite samples or render failures in their checked
spans. The explicit queue is a diagnostic override; its shorter result does not
replace automatic safety behavior. Initial latency properties captured while
opening a probe can precede the automatic runtime adjustment; the table reports
the measured timestamp marker delay rather than those initial properties.

Evidence is in `artifacts/installed-driver-timing-f65b35d1/result.json` (explicit)
and `artifacts/installed-driver-timing-3405d746/result.json` (automatic). Both
withdrew their temporary devices, stopped their owned Engine, preserved installed
files, defaults and Core Audio identity, and opened no physical devices.
These short virtual-only results establish neither app/microphone capture
latency nor a fresh matched Loopback comparison. Sustained minimum-buffer and
recovery tests remain necessary.

## Bandlimited history candidate: processing cost only

The newer capture/monitor history reader has a prepared bandlimited converter.
Its offline 64-channel 48-to-44.1 kHz benchmark took about 0.245 ms per 64-frame
block at the median on the tested arm64 Mac; this is computation time, not signal
latency. The existing source/monitor queues and signal timestamps are unchanged.
The symmetric kernel needs available history on both sides of the requested
timestamp (about 112 source frames at that rate pair); smaller live queues still
need validation. Equal-rate aligned history reads use the direct path.

`artifacts/sinc-history-proof.json` records that history-only candidate's tests
and scope. A subsequent shared-reader candidate now also covers public virtual
inputs. Its private playback-mapping benchmark measured about 0.261 ms for a
64-channel 48-to-44.1 kHz read and 0.242 ms in the reverse direction, using
64-frame blocks. `artifacts/public-input-proof.json` records the warmed component
measurements; they are not a complete graph deadline or actual HAL latency test.
A subsequent causal output candidate replaces linear public-output conversion.
Matched rates retain one source frame of delay; 48-to-44.1 kHz adds 113 render
frames (2.354 ms) and the reverse adds 104 frames (2.358 ms), with this delay
included in advertised input latency. These are designed filter delays, not
measured end-to-end results. `artifacts/causal-output-proof.json` records offline
spectral, continuity and safety tests. These candidates are not installed and
do not establish a new 3–5 ms result or a new Loopback comparison. The
installed measurements below retain their original binary hashes and settings.

The stereo capture path, including application sources in the document runtime,
now shares the same history reader (`artifacts/stereo-history-proof.json`).
This removes linear input conversion and adds stop/restart generation isolation.
It leaves configured source queues and signal timestamps unchanged; fractional
and off-rate reads require both filter wings. No new installed latency,
minimum-queue or Loopback comparison is established by these offline checks.

The source-rate estimate now uses a bounded recent observation window. The
clock-step fixtures in `artifacts/changing-clock-proof.json` measure settling
and explicitly retain transient errors; settled SNR does not certify phase
quality during a clock transition. This changes clock tracking, not configured
audio queues or advertised latency, and supplies no new installed comparison.

Historical callback-clock mapping now also has phase-transition evidence in
`artifacts/clock-phase-proof.json`: dense synthetic traces include abrupt and
per-callback varying rates. Stable spans coalesce only after timestamp-precision
validation, correcting an intermediate component-cost regression at wide extreme
downsampling. No audio queue or advertised delay was added. Warmed component
costs and digital phase error remain separate from actual capture latency,
physical-clock quality and the pending matched Loopback comparison.


The newer monitor candidate also follows measured physical output rate rather
than interpolating every callback at its nominal format. Its 25–50 ms rate
observation window adds no audio queue; callbacks still anchor to their actual
first-sample timestamps. `artifacts/output-clock-proof.json` records synthetic
fixed-drift audio and timestamp/lifecycle checks. These establish neither live
monitor latency nor minimum stable hardware buffers, and do not supersede the
installed measurements below.


## Newer conversion candidate: live rehearsal

`artifacts/output-clock-live-proof.json` records a separately signed Release
workspace Engine against the existing installed driver. Concurrent 2-, 8- and
64-channel virtual clients ran for 30 seconds, using a 64-frame graph/client,
128-frame pass-through lookback and 64-frame render lead at 48 kHz. The
8-channel case traversed a second virtual device. Marker delays were **4.75,
4.75 and 5.25 ms**, respectively, at the diagnostic's 0.25 ms search resolution.
Silent samples after warmup, nonfinite samples, timestamp/render failures and
graph late/skipped/reanchor increments were zero. Maximum observed graph
execution was 0.132334 ms against a 1.333 ms period. These results do not prove
a consistent 3–5 ms profile or a whole-system deadline guarantee.

Two always-muted real outputs at 48/44.1 kHz and a separate all-16-channel
Antelope output check each passed 12 seconds without missing frames or render
errors. Their actual queues were 1280/1371 internal 48 kHz frames (about
26.7/28.6 ms), before other source/hardware delays; this is queue accounting,
not a measured physical signal latency. Existing output rates/buffers, audio
defaults, installed binaries and the Core Audio process were preserved.

The first virtual harness reported a transient catalog read failure during
withdrawal; later snapshots confirmed successful removal. The original failed
harness report is retained, alongside its passing signal checks, six fail-closed
retry tests and a fully passing three-second lifecycle repeat. Neither this
rehearsal nor the silent monitor checks measure application-capture/acoustic
latency or compare the new candidate with Loopback. The canonical installed
Engine and pending desktop package were not replaced.


The subsequent physical-input check in `artifacts/capture-live-proof.json`
validated real 16/8-channel acquisition and shared stereo continuity, using a
1024-frame source lookback (21.33 ms at 48 kHz). Its outputs were muted, so the
zero-output virtual probes do not measure physical-input marker delay. Neither
the observed source levels nor zero missing-frame growth establish 3–5 ms
physical input latency or a new comparison with Loopback.


## Newer application capture: continuity profile

`artifacts/app-live-proof.json` records two real generator apps writing to
isolated LoopZ virtual sinks. The source lookback was 1024 frames, readers used
512-frame buffers and eligible app taps used the 128-frame device-stream path.
Software app-capture markers measured about **42–43 ms**, with a 1 ms search
resolution; the direct virtual sink markers were about 24 ms in the same setup.
These settings prioritize the continuity rehearsal and do not establish a
3–5 ms app profile or acoustic latency.

The first failed run included a slow capture startup before a valid output
clock was published. A new running-output guard and an intentionally delayed
source startup produced a 3.3–3.5 second readiness result in later fixtures.
Startup readiness is separate from signal delay. After readiness, 35- and
50-second readers passed with zero silent samples, bad signal windows, queue
faults or timestamp errors through changes on the other app. Original-playback
mute, shared capture, silence during app absence and fresh capture after relaunch
also passed. No matched test against Loopback has been performed for this code.

## Public-client queue rehearsal and observation limits

The single-process application rehearsal in
`artifacts/public-pass-timing-proof.json` found 90 zero samples in an unrelated
app's original pass-through with the fixed 128-frame lookback and 512-frame
clients. Shared application capture itself remained valid. A diagnostic
1024-frame queue removed the original-playback failure; an automatic per-device
floor then passed the same dormant/start/mute/close/reopen scenario without a
manual pass-delay override. The unrelated application capture passed 70 seconds
without silent samples or signal/timestamp/queue errors.

Those automatic-policy rehearsals measured roughly **44 ms at 48 kHz** and
**52–53 ms at 44.1 kHz**, at 1 ms marker-search resolution. The graph had a
64-frame quantum and 1024-frame application lookback. A second writer reported
32 or 64 frames while the Engine's query still returned the 512-frame profile;
there is no live evidence here that it adopted a smaller queue.

The newer workspace driver reports actual WriteMix callback periods and an IO
session in a read-only property. The Engine can then replace an idle fallback
with measured timing, retain a larger queue during that session, and reconcile
graph/input-latency metadata through native approval. This is currently unit
and isolated-driver-host evidence; that driver is not installed. Neither these
checks nor the passing conservative rehearsals establish a 3–5 ms app/microphone
profile or a new matched Loopback comparison. The earlier low-delay virtual
results below retain their original exact profiles and binary hashes.

## Installed multichannel runtime: virtual-client rehearsal

The existing-slot migration passed without restarting Core Audio. A separate
signed Release Engine candidate then ran against that installed driver with a
64-frame graph, 128-frame pass-through lookback, 64-frame render lead and
64-frame virtual-client buffers at 48 kHz. Three clients ran concurrently for
30 seconds: stereo, an 8-channel virtual chain and a 64-channel route. All three
measured 4.75 ms host-timestamp marker delay, at 0.25 ms search resolution, with
zero silent samples after warmup, nonfinite samples, timestamp errors or render
failures. Graph late/skipped frames, reanchors and sink/graph failures had zero
increments. Maximum graph execution was 0.624 ms versus a 1.333 ms render period.

Evidence: `artifacts/installed-routing-io-proof.json`. The candidate was signed
with the existing Engine identity and reused the existing grant without a
permission request. It ran from the workspace; installed Engine binaries and
production defaults were not replaced. This is virtual routing rather than
application capture, microphone/monitor or acoustic latency. The older Loopback
comparison below measures a different path and cannot establish a new speedup.
A fresh matched comparison and long-session validation remain required.

Preserved failed measurements explain the profile: default 512-frame clients
measured 23.25 ms. Reducing client buffers to 128/64 frames with the 256-frame
graph reduced delay but failed signal validation. A Debug 64-frame graph passed
short checks but produced silent gaps in the strict concurrent 30-second test.
The Release candidate passed that same strict workload. Small buffers alone
are therefore insufficient; neither averaged correlation nor a successful
short check establishes continuous audio.

The temporary graph was withdrawn and its user job removed afterward. Default
devices and the Core Audio PID/start time remained unchanged. Existing-slot
activation does not resolve fresh-slot discovery or production background/login
supervision. The measurements below retain their historical settings and scope.

The smaller-block LoopZ app-only experiment measured **4 ms at both 48 kHz
and 44.1 kHz** in twenty-second rehearsals on this arm64 Mac running macOS
26.6.2. These are measured digital signal delays, not advertised device
properties or acoustic microphone latency. Marker search resolution is 1 ms.

The latest certificate-signed helper, using the generic portable routing graph
through its stereo wrapper, repeated these short checks successfully. App-only
delay was 4 ms at both rates. With microphone and muted monitoring enabled,
the 1024-frame route measured 21 ms at 48 kHz and 23 ms at 44.1 kHz. Two clients
passed in every run, with zero active source, driver, discontinuity and deadline
errors. Evidence: `artifacts/routing-regression-suite.json`.

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

The same 64/192-frame configuration then passed a **600-second 48 kHz**
regression. Both continuous clients validated 599 marker windows: one measured
4–5 ms and the other 4 ms throughout. All 59 additional one-second client
starts/exits passed. Active error deltas and post-checkpoint increments were
zero, including source/driver misses and missed publication deadlines. Minimum
source margin was 0.914 ms and minimum publication slack was 0.591 ms. Observed
resident-memory ranges after the first minute were 64 KiB for the helper and
48 KiB each for the source and two clients. Helper exit was clean.

The post-run idle snapshot verified driver build 3, source metadata and input
latency both 192 frames, queue zero, period 128, and unchanged default device
selections. Callback receipt offsets were approximately 0.990–1.132 ms after
their input sample timestamps. The 600-second result remains specific to this
app-only fixture and does not meet the two-hour gate. The 44.1 kHz evidence is
the twenty-second rehearsal, not a ten-minute run.

The first longer small-block attempt was interrupted at approximately 334
seconds when a concurrent build removed the probe used by later client churn
launches. It is failed evidence, not a 600-second pass. The controller now
snapshots its probe and source app into each run directory and verifies their
signatures; source bundles are sealed locally. Actual snapshot executable
hashes are recorded alongside the original build inputs. The interrupted run
has no active checkpoint, so its final 420 missing driver frames cannot be
assigned to the recording interval or cleanup from the available evidence.

## Matched Loopback comparison

The latest matched comparison with installed Loopback used the larger settings
below. It is a separate reference: the new 4 ms experiment uses a 128-frame
source-device timestamp period rather than the matched 1024-frame setting.
The updated routing helper repeated the LoopZ side at both rates with the same
results and zero active errors. Evidence:
`artifacts/routing-matched-regression-suite.json`; the current Loopback reference
is `artifacts/reviewed-loopback-1024-result.json`.

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

That longer microphone regression belongs to the preceding helper. The newer
small-block helper initially timed out while requesting microphone access;
that failed setup remains at
`artifacts/ultralow-48000-mic-monitor-regression-result.json`.
System Settings subsequently showed LoopZ Engine access already enabled.
Without changing permissions or replacing the helper, twenty-second retries
passed at both rates: 21 ms at 48 kHz and 23 ms at 44.1 kHz, with both clients,
nonzero microphone input, muted monitor coverage, clean helper exit and zero
active errors. The earlier timeout's exact cause is not isolated; it did not
establish that consent was missing or that a dialog was visible.

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

An earlier two-hour attempt failed around 420 seconds; that failed result is
retained. The later signed stereo helper passed a full 7200.952-second 48 kHz
mixed-source run with two continuous clients and 719 additional client checks.
Both clients passed 7199 marker windows at 21–22 ms / 22 ms, with zero active
engine or consumer error increments. The microphone and muted 44.1 kHz hardware
monitor also passed continuity/coverage checks. It uses 256-frame graph blocks,
1024-frame source lookback, driver period 128 and zero added read queue. Evidence:
`artifacts/two-hour-mixed-source-proof.json` and its immutable result archive.
This validates those settings and exact installed hashes; the newer recovery/
service builds, acoustic latency and broader hardware/OS coverage remain
unverified. Conservative defaults have not been promoted automatically.

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
- `artifacts/ultralow-helper-install-result.json`
- `artifacts/ultralow-driver-update-result.json`
- `artifacts/ultralow-48000-block64-source256-driver3-result.json`
- `artifacts/ultralow-48000-block64-source192-driver3-result.json`
- `artifacts/ultralow-44100-block64-source192-driver3-result.json`
- `artifacts/ultralow-48000-block64-source192-churn-result.json`
- `artifacts/ultralow-48000-block64-source192-interrupted-result.json`
- `artifacts/ultralow-192-idle-snapshot.json`
- `artifacts/ultralow-cleanup-result.json`
- `artifacts/ultralow-summary.json`
- `artifacts/ultralow-48000-mic-monitor-consent-retry-result.json`
- `artifacts/ultralow-44100-mic-monitor-consent-retry-result.json`

Earlier installation/recovery evidence remains in
`artifacts/driver-host-recovery-result.json` and
`artifacts/driver-timing-update-result.json`. Historical latency results and
failed rehearsals are retained alongside the newer runs.


## Global buffer and graphics follow-up, 21 October 2026

The current Quad Cortex input / Zen monitor mix now has one persistent hardware-buffer selector. A selector change applies to both used endpoints and refreshes the owned monitor stream; the hardware and negotiated stream periods were checked at 32 and 64 frames through authenticated debug control. The desktop labels each choice with its per-buffer target duration at the current hardware rate. Those labels are not physical end-to-end measurements.

A trial removing the normal 1024-frame monitor floor failed its 30-second live check: the monitor accumulated 290925 missing frames with a 128-frame queue. That trial was rejected, and the conservative floor was restored. No improvement in physical latency or matched Loopback result is claimed from this work. Failed and accepted runs are kept separately in the artifacts.

Inlining the unchanged linear filter lookup reduced the drifting-clock 8-channel 48 kHz resampling benchmark median from 76.000 to 70.917 microseconds (about 7%). Ten-second live samples at the same 32-frame hardware setting showed Engine CPU 16.47% then 15.56% of one core. Graphics CPU varied from 4.09% to 4.99% in those uncontrolled windows, so they do not establish a graphics CPU reduction. The graphics changes remove meter width animation and card blur, isolate meter painting, freeze meter values behind modals, and avoid rebuilding wire geometry on changing counters. Native WebKit checks confirm the modal freeze/resume and bounded transform smoothing. These short samples do not establish sustained performance.
