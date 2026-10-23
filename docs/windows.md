# Windows preview port

Target: Windows 10 22H2 x64 and Windows 11 x64. Windows 10 compatibility is
checked on physical hardware separately from hosted Windows CI.

Current acceptance covers the physical Windows 10 22H2 x64 Dell and the
user-supplied Windows 11 x64 VM. On 2026-10-23 the user initially excluded Windows
11, then supplied `loopz@10.5.22.10` for testing. Its initial inspection identifies
Windows 11 Enterprise Evaluation build 26300, four virtual processors and 8 GiB
RAM, with WebView2 already present. Inspection alone is not Windows 11 acceptance.
The user approved VM test-signing and reboot after disabling Secure Boot in the
VM settings; Secure Boot was still enabled at the last check. Local VM creation
remains stopped. Remaining audio, lifecycle, hardware/client, installation and
production-signing gates still apply.

This is an incomplete port. It builds the native desktop host and a per-user
background engine, using the same React interface and C++ routing/DSP core as
macOS. It supports physical input capture, process-tree application capture,
channel routing, gain/mute/limiting, hardware monitoring, saved documents and
undo/redo. The engine prepares graph replacements while capture and unchanged output streams
remain live, then pauses only the serial graph writer for the ownership swap.
Matching buses, clock scheduling and compensation histories survive edits.
This is stream preservation, not a measured guarantee of glitch-free filter changes.

**The older public desktop-only ZIP does not install a virtual audio driver.** With only that ZIP, saved mixes do
not appear as recording devices in other applications. Pass-through and muting
an application's original playback are unavailable. Application capture does
not silently fall back to capturing the whole system. This preview is not a
replacement for the released macOS app.

## Current integration status

The installed Dell Windows 10 22H2 candidate is driver 0.4.0.25 and R644
app/control/helper/broker, with the unchanged R462 interface. R644 links the
Windows user-mode runtime statically after clean VM installation exposes the
R622 broker dependency on absent global VC runtime DLLs (exit C0000135). R644
passes 48 selected checks against frozen R643, whose only change from R620 is
the CMake runtime setting. R648 native update and saved-state verification and
R649 installed 48 kHz stereo/15-second PCM16 waveform/native-flow audit pass.
The prepared workbench ZIP, passing existing-microphone Windows 11 console
recording (R654), and failed new-endpoint default-preservation check (R653) are
recorded in [preview release preparation](windows-preview-release.md).

The preceding R622 candidate passes all 48
selected Windows checks against frozen source R620. R624 native maintenance
migrates one device without a reboot and verifies the loaded R621/R623 kernel,
normal runtime and saved document/defaults. Windows regenerates the microphone
endpoint UID during the upgrade; recording clients may require reselection.
R625 passes the previously failing 96 kHz / 60-channel, 15-second PCM16 path,
including independent profile/native-flow audit and cleanup. R626's fresh full PCM16 matrix stops after 109 passing profiles when the
96 kHz / 28-channel microphone format remains updating. Cleanup and saved-state
checks pass; complete coverage is not accepted.

On the preceding .24/R560 pair, R562 and R563 pass untraced one- and five-minute maximum-format continuity
checks after the R560 worker fairness correction; the severe .23 timestamp
regression is absent. R566 stops after 95 successful profiles because report
replacement is denied; its cleanup and saved-state checks pass. R577 passes the
fresh complete 192-profile float matrix on R560, independent native-flow audit and
installed-state/cleanup checks with the tested report-lock fix. R596's .24/R593
PCM16 matrix fails at 96 kHz / 60 channels after 125 passing profiles; the focused
R612 reproduction also fails. Matched float R614 passes. PCM16, transitions,
lifecycle, hardware/client, clean install and production-signing gates remain
incomplete, so this is still a preview.

The app runs without elevation. The driver uses the unchanged development
certificate; test-signing remains enabled as requested for this workbench. Saved document
bytes and all audio default roles are preserved. Driver upgrades still regenerate
the microphone endpoint identity. The old public app-only ZIP does not represent
this installed setup.

R451 passes ordinary app routing from an initially absent recovery store,
including independent process markers, mute/unmute, durable receipt creation,
preference restoration, receipt release and internal sink removal. Original
saved document bytes and recovery database/ACLs are restored. Continuous nonzero
automatic handoff and crash/sleep/replug recovery remain unverified. R446's
render-only provisioning and invalid-request checks and R448's existing-history
routing evidence apply to the preceding .21/R440 build.

R454 measures 48 kHz stereo exclusive-PCM16 virtual feed → half-gain Engine →
public microphone latency on the earlier .22/R462 pair: median 76.52275 ms and range
66.5901–77.66645 ms across 20 markers. Capture gaps/discontinuities/timestamp
errors are zero. This excludes selected-app capture, physical hardware, acoustics
and networks. Saved bytes/defaults/microphone identity pass post-verification.
R457 passes that earlier pair's 15-second bounded-recorder stereo pilot.
R458's five-minute 192 kHz / 64-channel recording fails continuity: 476,610
all-channel zero frames, a 158,848-frame longest run, publication misses, three
driver underruns and one worker restart. The recorder itself loses no packets.
Terminal cleanup and saved-state/default/microphone checks pass.

The preceding .21/R440 pair passes R447's complete 192-profile exclusive-float
matrix and R445's 60-second 88.2 kHz / 54-channel recording. These results do not
prove the current .22/R526 pair's complete matrix.
The public microphone UID changed during both recent driver upgrades, so external
recording-app selection preservation remains unresolved. Full format/stress,
handoff/lifecycle, hardware/third-party, clean Windows 10 installer, UI and
production signing/distribution gates remain open. R463's 15 idle GPU samples
report zero percent for identified desktop/WebView engines, without proving
interaction-dependent or macOS GPU behavior.

### Current native publication acceptance (R502–R504)

R502 passes all 46 selected Windows checks against frozen source R501. R503
installs the exact rebuilt app and control tool. R504 then records 60 seconds at
192 kHz / 64 channels with native publication flow version 1, the original R462
bounded recorder, no profiler and the saved 48 kHz mix active. It **fails
continuity**: 3,065 consecutive all-channel zero frames (about 16 ms), matching
one driver underrun and 3,065 driver missing frames. All 11,520,000 accepted frames
are analyzed; recorder drops, invalid/pending packets, capture position errors,
publication misses, worker restarts and deadline misses are zero. Publication
wait timeouts are zero. This is an output supply failure, not a recorder loss.

The terminal run is cleaned up. R504 post-verification confirms the exact R502
app/control hashes, normal unelevated desktop/background/Engine processes,
running broker, saved document bytes, all default devices and microphone identity.
Evidence is retained in `artifacts/windows/native-screen-r504/`.

R493's one-minute and R495's five-minute maximum-format recordings passed on the
earlier R481 app with explicitly experimental paired flags. They do not prove
native R502 reliability. R505–R508 are prepared for sustained, latency, float and
PCM16 acceptance, but remain unstarted after R504 fails. Release readiness remains
blocked on resolving continuity and completing the wider acceptance gates above.

### Output deadline candidate (R509–R511)

R509 changes only the private Windows worker's ingress scheduling: the outer
loop stops draining when its output deadline arrives, and publication waits
stop draining at their existing queue-budget deadline. Each packet remains
atomic; inter-chunk drains and the driver's bounded ingress queue are retained.
Cancellation is checked before each packet. This neither changes prefill nor
extends the worker deadline. The frozen R510 snapshot differs from R501 only in
`native/platform/windows/virtual_cable.cpp`; R511 builds it in a fresh Dell
workspace. Formatting and all 46 selected Windows checks pass. R512 installs the exact
R511 app/control binaries and verifies saved bytes, all defaults, microphone
identity, normal permissions and unchanged service/interface/driver.

R513's 60-second 192 kHz / 64-channel recording fails with 2,000 all-channel zero
frames, longest run 1,584 frames. Every one of the 11,520,000 accepted frames is
analyzed, with no recorder drops/invalid/pending packets or capture errors.
Within-recording driver underruns and missing-frame deltas are zero, but
publication missing frames increase by exactly 2,000 and publication waits time
out ten times. Worker restart/deadline counters remain zero. This run shifts the
observed failure to publication availability; it does not prove reliable output
or establish a general improvement from one comparison. The terminal task is
removed and post-verification confirms saved state/defaults/microphone identity.
Raw evidence is in `artifacts/windows/native-screen-r513/`.

### Per-domain timing investigation (R514–R516)

R513 publication losses first appear between observations 72 and 73. Aggregate
maximum renderer lateness rises from 28.550867 to 37.7198 ms in that interval,
while the maximum render-call duration stays 3.8758 ms. These are aggregate maxima
across domains; neither identifies the late domain nor proves scheduler
starvation. The extracted observations are retained in
`artifacts/windows/domain-timing-r514/publication-events.json`.

R514 adds per-domain atomic calls, total time, maximum call time and maximum
lateness to `renderTiming.domains`, keyed by sample rate. Allocation happens
before rendering, and JSON construction stays on the control thread. Audio
buffers, deadlines and scheduling are unchanged. The local Release domain check
passes (0.60 s), as does the ASan/UBSan domain check (1.00 s). Frozen R515 differs from R510 only in `scene_domains.hpp` and
`runtime.cpp`; fresh native R516 build and all 46 checks pass. R517 deploys the verified app and
control binaries; saved state, defaults, microphone identity and normal
permissions pass verification.

R518 then fails its 60-second maximum-format recording with 3,933 consecutive
all-channel zero frames, matching the within-recording driver missing-frame
delta and one underrun. The recorded underrun requested 206 frames with only
116 queued. Re-prime silence amplifies that initial shortage. Publication missing
frames and wait timeouts remain zero; renderer maximum lateness is 5.447834 ms
at 48 kHz and 6.8723 ms at 192 kHz, unchanged across the underrun observations.
This run therefore does not reproduce R513's publication failure. All 11,520,000
accepted frames are analyzed with no recorder drops/invalid/pending packets or
capture errors. The terminal task is removed and post-verification passes.
`artifacts/windows/native-screen-r518/domain-analysis.json` retains both domain
and driver-queue event observations. Delivery continuity remains unresolved.

### Inter-chunk ingress deadline candidate (R519–R523)

R519 extends ingress deadline scheduling to paired output chunks. Before each
subsequent chunk, ingress drains only until that chunk's existing nominal due
time. If output is already due, queued ingress remains available for subsequent
slack windows and for publication waits that need more source history. Legacy
buffered delivery retains its existing inter-chunk drain. No queue capacity,
prefill, presentation timestamp or wait/deadline limit changes.

Frozen R520 differs from R515 only in `virtual_cable.cpp`. Formatting passes;
R521 native build and all 46 checks pass. R522 deploys the exact verified
app/control binaries; saved settings, defaults, microphone identity and normal
permissions pass verification.

R523 fails its 60-second maximum-format recording with 5,244 consecutive
all-channel zero frames. The pass-through source missing-frame counter rises by
exactly 5,244, accompanied by an ingress packet discontinuity. Publication
missing-frame, driver underrun/missing-frame, worker restart/deadline and
publication timeout deltas are zero. All 11,520,000 accepted frames are analyzed
without recorder drops/invalid/pending packets or capture errors. This shifts the
observed failure to incoming playback continuity and does not establish a
reliability improvement. The terminal task is removed and post-verification
passes. `artifacts/windows/native-screen-r523/ingress-events.json` retains the
source-history transition; `domain-analysis.json` retains the render timings.
The installed R521 candidate remains a preview, not an accepted continuity fix.

### Ingress sequence diagnostics (R524–R526)

The driver already numbers every accepted playback packet. Explicit packet
discontinuities can follow either a playback reset or a full ingress queue, so
R523's discontinuity alone cannot establish overflow. R524 counts skipped packet
numbers and backwards sequence resets on the worker, separately from timestamp
resets. The tracker starts anew per connection; cumulative diagnostic counters
retain the existing worker-recovery semantics. Natural unsigned wrap remains
continuous. No driver, transport layout, sample processing or scheduling changes.

Local Release duplex checks pass (0.55 s), as do ASan/UBSan checks (5.77 s),
including first-packet, contiguous, skipped, duplicate, backwards and wrap cases.
Frozen R525 adds `ingress_sequence.hpp` and updates worker diagnostics, runtime
JSON and the duplex test. R526 native build and all 46 checks pass. R527 deploys the exact verified
app/control; saved state, defaults, microphone identity and normal permissions
pass verification.

R528 fails its 60-second maximum-format recording with 6,445 all-channel zero
frames (longest run 3,600). Within the recording, playback discontinuities
increase by four and the new sequence counter reports ten skipped packets with
zero sequence resets. Driver recording missing/underrun and publication missing
frame deltas remain zero. All 11,520,000 accepted frames are analyzed without
recorder drops/invalid/pending packets or capture errors. This confirms ingress
packet loss, rather than a sequence rewind. The packet-count-limited queue can
exhaust its slots before its nominal 9,600-frame budget when packets are short;
queue fragmentation is the next investigation, not yet a proven sole cause.
Terminal task cleanup and post-verification pass. Raw observations are retained
in `artifacts/windows/native-screen-r528/ingress-events.json`. These counters are
diagnostic evidence, not a continuity fix or a way to reconstruct missing audio.

### Fragmented ingress packing candidate (R529)

A new local float-bridge regression reproduces loss of early packets when 9,600
contiguous frames arrive as one-frame fragments: the 40 slots fill despite the
payload fitting the intended frame capacity. R529 packs timestamp-contiguous
float fragments into the existing unread tail slot and splits a fragment at a
full packet boundary. Partial packets remain immediately readable. Legacy PCM16
packet behavior, payload capacity, recording queue and prefill remain unchanged.
The bounded integer-only operation uses existing ingress storage without waiting
or allocation. Timestamp comparisons allow the two 100 ns rounding ticks possible
when a previously split fragment is compared with a new rounded timestamp.

Release float/legacy duplex checks pass (0.55 s). The regression covers all six
advertised rates with 1-, 37-, 239- and 240-frame fragments, exact payload order,
packet sequence, timestamp error at most one tick, complete frame coverage and
separate timestamp-gap packets. ASan/UBSan float and legacy duplex checks also pass (16.92 s). This driver
candidate has not been kernel-built, signed, installed or accepted on Dell;
installed driver remains exact .22 and app R526. R530 extends controls to 2/64 channels at 44.1/192 kHz, unread wraparound,
immediate partial-tail reads, exact payload order, real-capacity overflow,
discontinuity signaling and guard bytes. Release checks pass (0.37 s), as do
ASan/UBSan checks (16.96 s). Frozen R531 changes the queue, tests and the explicit
driver version to 0.4.0.23. R532 WDK Release x64 build passes, with source hashes checked before and after
compilation. The stamped INF is 0.4.0.23. Preserved SYS SHA-256 is
`8A1480B2BD8B19DDEC628A6EC412C661F56AB3226501337803C8DF4329F8C21B`.
The candidate is unsigned and uninstalled; signing, native Windows regression
checks, package admission and live Dell continuity remain pending.

### Test signing and package admission (R533–R535)

R533 test-signs .23 using the unchanged non-exportable LocalMachine workbench
certificate `FEC5B498B1777655AF90E665A0856E2755212078`. SYS and catalog signatures
verify Valid with that exact thumbprint; Inf2Cat reports no warnings/errors.
Signed SYS SHA-256 is
`58371198405DA056A2FE5F6237BDEE42DFFD6BDF6AD74E1B7E5419D24FE9BF9D`,
and catalog SHA-256 is
`33BDD25EF363953D073E889A5C5EA2A56D667FD78868AC75E300E4E3280182BA`.
The package remains development-only and uninstalled.

The .23 delta preserves the adapter configuration record and private property
layouts; it changes ingress packing and the package version. Native render-only
package admission now explicitly includes .23, while .24 and malformed versions
remain rejected. The local configuration test passes (0.59 s). Frozen R534
changes only the admission header and test relative to R531; the driver subtree
is identical. R535 native build stops on a test-only MSVC size-narrowing warning. Its selected check set now
includes the float-bridge fragmentation/lifecycle regression (47 checks rather
than the preceding 46). Installed runtime remains app R526, driver .22 and R462
maintenance service/helper pending verification and native maintenance.

### Native maintenance preparation (R536)

R536 scripts prepare the .23 update through the native maintenance helper,
preserving the current stage, installed broker, loaded driver package, document,
defaults and device state first. The prepared scripts parse on Windows; no
maintenance action has started while the corrected R538 builds. Post-verification checks the
exact new app/service payload, loaded signed SYS, normal permissions and native
publication flow. Microphone identity is recorded separately because prior driver
upgrades regenerated it; selection preservation remains an open release gate.

The compiled R532 `pushPlayback` function reserves 32 stack bytes and pushes one
register, with no function calls or floating-point arithmetic. Compiler-generated
XMM metadata data moves are present. This narrow inspection is retained in
`driver-build-r532/queue-code-review.json`; it is not a whole-driver stack audit
or evidence of live kernel reliability.

### Windows test portability correction (R537–R538)

R535 stops compiling the newly selected float-bridge test: MSVC C4267, treated as
an error, rejects an implicit `size_t` to `unsigned` conversion at the bridge
configuration boundary. No tests or installation are claimed from that failed
build. The controlled test storage sizes are now explicitly converted to the
API's unsigned byte counts. Local Release float-bridge check passes (0.55 s).

Frozen R537 differs from R534 only in that test; the kernel subtree is identical,
so R532/R533 kernel-build and signature evidence remains applicable. Fresh R538
native build/checks are running. The unstarted R536 maintenance scripts now pin
R538/R537. Installed Dell remains app R526, driver .22 and R462 service/helper.

### Installed .23 regression (R536/R539)

R538 passes all 47 selected Windows checks with source verified before/after.
R536 native maintenance migrates one device and updates the broker without a
required reboot. The actual loaded SYS matches signed .23. Normal app permissions,
saved document bytes and all default roles pass; the microphone UID regenerates
to `{0.0.1.00000000}.{a118f88f-ef63-4964-9e4b-b618bf8c73ea}`. Temporary maintenance
tasks are removed.

R539's 60-second 192 kHz / 64-channel recording is a severe regression: 5,886,716
all-channel zero frames and 3,668 playback timestamp resets. Skipped ingress
packets, driver underruns/missing-frame deltas, publication missing-frame deltas
and recorder losses are zero. Every one of 11,520,000 accepted frames is analyzed.
This disproves the queue candidate's installed correctness despite its local
payload/timestamp-bound tests. The local tests must also check the actual
PlaybackClock continuity contract, rather than only packet timestamps against a
one-tick bound. R539 task cleanup and post-state preservation checks pass; .23 is
still installed and must be corrected or recovered before further acceptance.
Native maintenance uses normal driver ranking and cannot directly force a .22
downgrade; recovery must account for that constraint. Raw reports are retained in
`artifacts/windows/native-screen-r539/`. Release readiness remains unproven.

### Continuous ingress timestamp correction (R540–R544)

R540 adds the real worker `PlaybackClock` to the packet-fragment regression;
it fails against .23 despite the earlier one-tick packet timestamp bounds. The
correction keeps a single ingress time origin and cumulative frame count, and
derives each split packet timestamp from that origin rather than adding rounded
fragment durations. Input timestamps farther than one tick from the nominal
continuous clock still start a new origin. Playback resets clear that clock.
This retains immediate partial reads, exact payload order and the same queue
capacity. No output prefill or latency budget changes.

Release float/legacy duplex checks pass (0.37 s), ASan/UBSan checks pass
(17.07 s), and package-admission checks pass (0.31 s). Checks exercise continuous
worker timestamps through fragments and unread wraparound; explicit timestamp
gaps still reset the worker clock. Frozen R541 updates driver .24, its explicit
package admission and these tests. R542 kernel build passes. R543 test-signs SYS
and catalog with the unchanged workbench certificate; both verify Valid and
Inf2Cat reports no warnings/errors. Signed SYS SHA-256 is
`E1C5056CFCC283623FC5BAFCD816754CE01A229B88258A8805666FBB9464DB95`.

R544 native build/checks are running; the exact float-bridge clock regression is
built and run first before the broader 47-check set. Installed .23/R538 remains
the known failed candidate until its replacement is verified and applied. No
claim of installed correction or continuity is made from these local results.

### Installed timestamp recovery and remaining input timing (R545–R546)

R544 passes the early Windows clock regression and the full 47-check set, with
source verified before/after. R545 native maintenance installs .24 and matching
app/service payloads without requiring a reboot. Exact loaded SYS and normal
runtime verification pass. Saved document/defaults are preserved. The microphone
UID regenerates to `{0.0.1.00000000}.{7aecd6ed-6313-4dcb-be73-7aa2f68b5514}`;
selection preservation remains unresolved. Temporary maintenance tasks are removed.

R546 fails the 60-second 192 kHz / 64-channel recording with 5,597 consecutive
all-channel zero frames. Unlike .23, timestamp resets are zero. Skipped ingress
packets, sequence resets, driver underrun/missing-frame deltas and publication
missing-frame deltas are also zero. The pass-through source missing-frame delta
is exactly 5,597, all in the input history's unpublished category. Both render
rate domains retain their pre-event lateness maxima. All 11,520,000 accepted
frames are analyzed without recorder drops/invalid/pending packets or capture
errors. This verifies recovery of the .23 timestamp regression in this run,
but not full continuity or sustained ingress reliability. The next investigation
is input-history availability, rather than another queue-size increase.

Terminal R546 cleanup and post-state verification pass. Raw waveform and source
history transition evidence are retained in
`artifacts/windows/native-screen-r546/ingress-events.json` and
`domain-analysis.json`. No profiler is attached and the saved 48 kHz mix stays
active. Longer recordings and format matrices remain unstarted pending this
failed screen.

### Input publication age diagnostics (R547–R549)

R546 input history adds 5,597 missing frames in the unpublished category between
observations 94 and 95, matching recorded silence. Packet sequence/timestamp
reset counters and renderer lateness maxima do not change. Existing counters
therefore cannot distinguish a delayed driver-to-history publication from a
history-read problem.

R547 measures the maximum age of a packet's final frame when its history append
completes, and the maximum elapsed time between history publications. It reuses
the worker's existing completion clock sample and adds only atomic diagnostic
updates. Control state also exposes the current input history end timestamp and
its sampling time. Gap maxima can include idle periods and are not themselves
proof of steady-state delay; interpret them against the active recording window.
No queue, prefill, scheduling or audio-delay setting changes.

Frozen R548 differs from R541 only in worker diagnostics, their aggregate and
runtime JSON. Kernel subtree and signed .24 package are unchanged. Formatting
passes; R549 native build/checks are running. Installed runtime remains .24/R544
until that diagnostic candidate is verified.

### Input timing screen and sustained run (R549–R552)

R549 passes the full 47 Windows checks with source verified before/after. R550
updates only app/control, preserving .24 driver, R544 helper/service, saved
bytes, all default roles and microphone identity.

R551's 60-second 192 kHz / 64-channel recording passes the native-flow independent
audit: all 11,520,000 frames analyzed, 99 within-recording observations, no
all-channel silence, recorder losses, capture errors, ingress sequence/timestamp
resets, driver underruns or publication misses. Maximum ingress publication lag
rises from 4.420275 to 11.884484 ms and maximum publication gap from 4.9607 to
8.6847 ms. Terminal cleanup and post-state verification pass. This recording
does not reproduce R546's intermittent input availability failure and does not
prove it fixed; the candidate added diagnostics rather than changing scheduling.

R552 starts a fresh five-minute maximum-format recording with the exact same
installed pair and original R462 recorder. It remains live; do not restart on an
observation timeout. Evidence is in `artifacts/windows/native-long-r552/` on Dell.
Independent report and domain analyses for the passing screen are retained in
`artifacts/windows/native-screen-r551/`. The domain analysis now handles both
successful profile-nested and failed top-level observation reports.

### Endpoint identity requirement clarification (R556)

[Microsoft endpoint ID documentation](https://learn.microsoft.com/en-us/windows/win32/coreaudio/endpoint-id-strings)
explicitly ties ordinary endpoint IDs to device installation and says driver
upgrades can change them. Therefore unchanged MMDevice ID strings must not be
promised as a Windows 10 driver-upgrade guarantee. The newer
[StableId property](https://learn.microsoft.com/en-us/windows/win32/coreaudio/pkey-audioendpoint-stableid)
is documented from Windows 11 24H2 and is not a Windows 10 baseline solution.

[Microsoft's endpoint migration guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/operating-system-upgrades)
uses hardware IDs and static reference strings to migrate user-controlled
settings, with unique references needed for multiple root software endpoints
before Windows 11. LoopZ already derives references from durable root instances
and filter names; migration of settings still needs direct verification.
Required release behavior is durable LoopZ mix/source recovery, preserved default
roles and clear upgrade reselection guidance. External applications caching old
MMDevice IDs may need the user to select the microphone again; do not claim
LoopZ can preserve those applications' selections. This clarifies the platform
contract without treating existing upgrade UX or settings migration as complete.
Research evidence is retained in `artifacts/windows/endpoint-identity-r556/`.

### Prepared follow-on acceptance (R553–R555)

R553 prepares the current pair's 48 kHz stereo exclusive-PCM16 marker latency
measurement. R554 and R555 prepare the complete 192-profile exclusive-float and
PCM16 matrices. All scripts parse on Dell and pin R549 app/control, R544 service,
exact loaded .24 via R545, unchanged source interface and the original R462
probe. They preserve the saved mix/defaults and remain unstarted while R552 is
live. These prepared fixtures are not test results or release evidence.

### Sustained failure and current latency (R552–R553)

R552 finishes with a failed five-minute 192 kHz / 64-channel recording. All
57,600,000 accepted frames are analyzed with no recorder loss/invalid/pending
packets or capture errors. Recorded all-channel silence totals 35,654 frames;
the longest run is 8,160 frames. Publication misses and driver recording
underrun/missing-frame deltas remain zero. Input history reports 37,397 missing
source frames within the sampled observation window; do not equate that window
count with the complete waveform count. Seven ingress packets are skipped,
timestamp resets stay zero, maximum publication lag reaches 49.2635 ms and
maximum gap between publications reaches 53.8802 ms. The transition observations
are retained in `native-long-r552/ingress-events.json`. These measurements locate
late input delivery but do not establish the underlying scheduler/driver cause.
Terminal cleanup and saved-state verification pass. The one-minute R551 pass
does not prove sustained reliability.

After that task is terminal and cleaned, R553 passes a separate 48 kHz stereo
exclusive-PCM16 marker-latency measurement on .24/R549: 20/20 markers, median
68.1067 ms, minimum 67.31495 ms and maximum 78.38795 ms. Capture position gaps,
discontinuities and timestamp errors are zero. Independent analysis recomputes
all marker bounds from raw QPC timestamps and checks the exact R462 recorder
hash. Path is application submission → virtual feed → half-gain Engine → public
microphone application receipt, including client buffers and scheduling; it
excludes selected-app capture, physical hardware, acoustics and network paths.
Cleanup/defaults/saved bytes/microphone identity pass. This is a latency result,
not maximum-format continuity acceptance. Raw and independent reports are in
`artifacts/windows/native-latency-r553/`.

### Input-stall scheduling diagnostic (R557)

After R552 and R553 are terminal and cleaned, R557 starts a three-minute
maximum-format timing diagnostic on the same .24/R549 pair. It records process,
thread, context-switch, dispatcher, DPC and interrupt events without stack
sampling, refuses an existing kernel logger, stops its own recorder in a finally
block, and preserves the raw ETL. Its bounded trace deadline does not authorize
restarting an audio task that remains live. This is diagnostic-only and cannot
substitute for untraced continuity acceptance.

Scripts parse on Windows and pin the current app/service/loaded driver plus the
original R462 recorder. CSV normalization uses the measured host code page with
strict round-trip verification, preserving the original export. The diagnostic
is now running; final event-loss checks and schedule analysis remain pending.
R554/R555 format matrices remain unstarted while it is active.

### Current scheduling evidence (R557 completed)

R557 completes the three-minute diagnostic, stops its own kernel recorder and
removes the audio task. Saved-state/default/microphone verification passes. The
waveform fails with 16,919 all-channel zero frames (longest run 8,561); every one
of 34,560,000 accepted frames is analyzed without recorder loss or capture
position/timestamp/discontinuity errors. Input publication gap reaches 50.615 ms
and lag reaches 48.9082 ms, with six skipped packets and zero timestamp resets.
Publication waits also time out 31 times under this diagnostic capture. It is
not acceptance evidence.

ETL/export hashes are pinned; measured-code-page normalization is strictly
reversible. Trace statistics report zero lost events/buffers and the independent
switch-pair check reports zero invalid pairs. Engine PID 6336 stays unchanged.
Render TID 2144 has 113,903 complete intervals, max off-CPU/ready delay 1.995 ms.
Transport TID 5292 has max off-CPU gap 3.747 ms and max ready delay 1.944 ms.
Neither has a complete off-CPU interval above 10 ms. The longest single DPC/ISR
is 3.319 ms in `nvlddmkm.sys`; this does not establish GPU causation or exclude
aggregate interrupt load. Scheduled residency includes interrupts/DPCs and is
not exclusive CPU time.

The observed Engine thread gaps do not explain the roughly 50 ms input
publication stall. The next investigation is upstream producer/driver timing;
no Engine priority or buffer change is justified from these maxima. Summary and
validated thread reports are in `artifacts/windows/scheduling-r557/`; large raw
ETL/CSV files remain on Dell. The installed .24/R549 candidate remains a preview.

### Duplex intake service correction candidate (R558–R560)

R557's short Engine scheduling gaps do not imply that the worker was actually
servicing ingress. Code inspection finds a starvation path introduced by
chunk-deadline-only intake: if output remains overdue, every inter-chunk drain
can be skipped while the worker continues rendering. Published Engine output
can stay available even when it contains source-missing silence, so publication
waits do not guarantee intake service either. This is consistent with the long
publication gaps without long worker off-CPU intervals; live causality remains
to be verified.

R558 guarantees a minimum input frame service before each paired output chunk,
including the first and overdue chunks. After that minimum, additional intake
uses deadline slack. Empty queues and cancellation still stop immediately, and
each call remains bounded by the negotiated transport's packet capacity (40 for
float, 20 for PCM16). It does not change queue sizes, prefill or delay settings.
Legacy buffered chunk handling is retained.

A real float-bridge queue regression models 100 continuously overdue batches at
48 kHz and 192 kHz / 64 channels. The deadline-only negative control discards
early packets; minimum frame service consumes every produced frame without a
backlog. Local Release check passes (0.33 s); sanitizer results are retained with
R558 artifacts. This tests service policy, not installed audio continuity.
Frozen R559 adds the policy header and changes the worker/test only; driver .24
subtree is unchanged. R560 native build/checks are running, with clock and duplex
regressions first. Installed app remains R549 pending verified deployment.

### Installed minimum intake service screen (R560–R564)

R560 passes the early Windows clock/duplex regressions and the full 47-check set,
with source verified before/after. The first R561 close task fails because its
action references a stale script filename; no binaries are replaced. Its terminal
failure is preserved and the task is removed. Fresh corrected R564 deployment
succeeds, updating only app/control. Driver .24, R544 service/helper, saved bytes,
default roles and microphone identity remain unchanged. Successful temporary
update tasks are removed.

R562's untraced 60-second 192 kHz / 64-channel screen passes the independent
native-flow audit: all 11,520,000 frames analyzed, 99 within-recording observations,
no recorded silence, recorder loss/capture errors, publication misses, driver
underruns, skipped packets or timestamp resets. Maximum input publication lag is
4.49805 ms and publication gap 5.2714 ms; worker lateness max is 2.5123 ms.
Terminal cleanup and post-state verification pass. This single pass does not
establish sustained reliability or a general performance improvement.

R563 starts a fresh five-minute recording with the same exact installed pair and
original R462 recorder. It remains live; keep polling the same task and do not
restart it on observation timeout. Raw passing-screen and audit evidence is in
`artifacts/windows/native-screen-r562/`; sustained evidence is being written to
`artifacts/windows/native-long-r563/` on Dell. The installed .24/R560 candidate
remains a preview pending sustained and wider acceptance.

### Sustained native continuity pass (R563)

R563 completes the untraced five-minute 192 kHz / 64-channel run on .24/R560.
The independent native-flow audit passes: all 57,600,000 accepted frames are
analyzed, 499 dense within-recording observations cover the window, all-channel
silence and maximum zero run are zero, and recorder drops/invalid/pending packets
and capture position/discontinuity/timestamp errors are zero. Source missing-frame,
publication missing-frame, driver underrun/missing-frame, worker restart/deadline,
ingress skip/sequence-reset and timestamp-reset deltas are zero. Publication wait
timeouts are zero. Maximum input publication gap is 5.4536 ms and maximum lag
8.880809 ms; no queue capacity, prefill or delay setting was increased.

The saved 48 kHz mix remains active. The exact original R462 recorder and R560
app/control are pinned. Terminal task cleanup and saved bytes/default/microphone
post-verification pass. This is a native/default sustained pass, unlike R495's
earlier experimental-flow pass, but does not establish complete Windows release
readiness or untested hardware/client behavior. Raw report, independent audit,
domain analysis and post-state proof are retained in
`artifacts/windows/native-long-r563/`.

R565 current-build 48 kHz stereo marker-latency measurement is now starting.
R566/R567 complete float and PCM16 matrices are prepared and parse, bound to
R560/.24/R544 service and the R462 recorder; they remain unstarted until the
current measurement is terminal and cleaned. Older R553–R555 fixtures are
superseded for the current app and must not be mistaken for current results.

### Current-build latency and matrix start (R565–R567)

R565 passes a separate current .24/R560 48 kHz stereo exclusive-PCM16 marker
measurement: 20/20 markers, median 77.2062 ms, range 67.03685–78.52875 ms, no
unexpected markers or capture position/discontinuity/timestamp errors. Independent
analysis recomputes every marker interval from raw QPC timestamps and verifies
the reference recorder hash. The median is higher than R553's 68.1067 ms on
R549; the ranges overlap, so these single runs do not quantify a general latency
change. Queue/prefill/delay settings are unchanged, but that is not proof of
unchanged measured latency. Preserve both observations when assessing timing.
Path remains virtual feed submission → half-gain Engine → virtual microphone
receipt, excluding selected-app capture, hardware, acoustics and networks.
Cleanup and saved-state/default/microphone verification pass.

R566 starts the full 192-profile exclusive-float matrix on the exact current
pair after R565 cleanup. R567 PCM16 matrix is prepared and syntax-checked but
unstarted. The older R554/R555 fixtures target the preceding app and are
superseded. Current evidence is in `artifacts/windows/native-latency-r565/` and
`artifacts/windows/native-float-r566/`.

### Release payload staging correction (R568–R569)

While R566 runs, release staging now excludes an input app directory's old
`driver` directory and root `manifest.json`. Only the explicitly verified signed
driver is copied, and the new manifest cannot include a stale hash of itself.
Existing application/kernel signature and catalog-membership gates are retained.
The release workflow documentation now covers the native Windows path.

A Windows PowerShell synthetic fixture checks replacement of a stale development
driver folder, exact explicit-driver contents, complete manifest/hash coverage,
unchanged input files, rejection of a development catalog signer, and refusal to
overwrite an existing output. R568's fixture array-wrapping assertion fails and
is corrected in the fresh R569 fixture; R569 passes all three cases. Signatures
and the verification executable are explicitly stubbed over harmless files; this
is staging/guard coverage, not real signing or installer acceptance. The fixture
is added to the Windows CI job. These packaging-only changes do not alter the
installed R560 app or active audio run. Evidence is in
`artifacts/windows/release-payload-r569/`.

R566 remains live; the latest read-only snapshot has completed 23 of 192 profiles
through 48 kHz / 46 channels. Partial progress is not a completed matrix pass.

### Silent uninstall candidate and fixture preparation (R570)

The production installer previously showed its custom removal confirmation even
when the uninstaller was invoked silently. The confirmation and helper-failure
message are now conditional on interactive mode; helper failure is logged and
still returns false before app files are removed. Restart-required helper results
remain propagated. This follows
[Inno's silent-uninstaller contract](https://jrsoftware.org/ishelp/topic_uninstcmdline.htm)
without bypassing Windows elevation or changing native removal operations.

The isolated fixture now enables its own uninstaller without registering an
Add/Remove Programs entry. Its harmless helper supports both install and uninstall
commands. New cases cover silent helper failure retaining app files, successful
retry, and restart-required removal under `/NORESTART`; setup success/restart/
failure coverage remains. Paths include a Cyrillic character and space, with
quoted log arguments. Uninstaller success/failure is interpreted according to
[Inno's documented exit contract](https://jrsoftware.org/ishelp/topic_uninstexitcodes.htm).

R570 preserves exact source hashes and prepared build/run scripts. PowerShell
syntax checks pass, but no compiler or fixture has run while R566 audio testing
is active. The runtime guard refuses overlap with audio tests/builds. This is an
unverified installer candidate, not installed-driver or production-signing
acceptance. No installed app/driver/service configuration changes occurred.
R566's latest snapshot is 48/192 profiles complete, still Running.

R571 tightens the prepared fixture: each uninstall attempt deletes its previous
helper marker first, so the successful retry must independently invoke the
helper. Silent setup now has the same 45-second watchdog as silent uninstall,
limited to the fixture process just launched. The fresh source snapshot is copied
to Dell and passes Windows PowerShell syntax parsing; compilation and execution
remain pending until the audio matrix is terminal. The unchanged R566 Python
process (PID 8764) and its current recorder were verified live after 61 completed
profiles. Evidence: `artifacts/windows/installer-uninstall-r571`.

R572 replaces the unstarted R567 PCM16 preparation. The old launcher checked
only the historical R447 task; the new launcher refuses any remaining continuity
task and active audio probe/Python/build process, and rejects an existing report.
The run-time guard repeats the process check. Post-verification now uses the
preserved R462 probe rather than a mutable development build path. App, helper,
driver, fixture and render policy bindings are unchanged. All prepared scripts
parse on Windows PowerShell; the suite remains unstarted while R566 is live.
Evidence: `artifacts/windows/native-pcm16-r572`. R567 remains preserved as the
superseded, unexecuted preparation.

### Offline WebView2 prerequisite candidate (R573–R574)

Clean Windows 10 systems may lack the Evergreen WebView2 runtime. Production
packaging now requires an explicit Microsoft-signed standalone installer,
validates its Authenticode policy, excludes stale prerequisite files and hashes
the supplied installer in the new payload manifest. R573 passes four synthetic
Windows PowerShell staging/guard cases, including rejecting a non-Microsoft
runtime signer. Those mocks do not establish actual signature or runtime
installation acceptance.

Setup checks the documented machine-wide 32-bit EdgeUpdate registration and
requires a nonzero parsed version. If missing, PrepareToInstall extracts the
bundled runtime and invokes `/silent /install` before app files are installed.
Failure, absent post-install registration or restart-required status prevents
app/driver installation. A machine-wide runtime avoids relying on the elevated
administrator's private runtime. Uninstall leaves Microsoft's shared runtime.
See [Microsoft deployment guidance](https://learn.microsoft.com/microsoft-edge/webview2/concepts/distribution)
and [Inno prerequisite handling](https://jrsoftware.org/ishelp/topic_scriptevents.htm#PrepareToInstall).

R574 replaces the pending R571 installer fixture. It preserves production
installation/error/version/launch logic while substituting only the runtime
registry query with an owned fixture file. Its runtime stub never installs
software or writes real registry keys. Prepared cases add runtime-present,
missing/install-success, helper failure, zero-exit-without-registration, and
restart-required paths to setup/uninstall coverage. PowerShell syntax parsing
and native formatting pass; compilation/execution remain pending while R566 is
active. These source changes do not alter the installed R560 app or .24 driver.
Evidence: `artifacts/windows/release-webview-r573` and `installer-webview-r574`.

R575 expands the synthetic packaging checks to ten passing cases. Added controls
reject an untrusted runtime certificate despite its Microsoft subject, an
untrusted Microsoft catalog, failed runtime/application policy verification and
failed INF/SYS catalog membership. Each case requires the intended error and
absence of a staged output directory. This exercises failure propagation through
the real packager using harmless text files and a controlled signature-tool stub;
it remains distinct from production cryptographic signing acceptance. Evidence:
`artifacts/windows/release-gates-r575`.

### Installer fixture pass and report-reader recovery (R574–R578)

R574's isolated Inno fixture compiles and passes seven setup and three silent
uninstall cases. WebView2 present/missing-success, helper failure, successful
exit without runtime registration and restart-required paths exercise the
production gates with harmless stubs and an owned registration-file substitute.
Failure and restart prevent app installation; uninstall failure retains files,
retry succeeds, and `/NORESTART` preserves restart handling. This is not actual
Microsoft runtime installation, driver removal or signed-distribution acceptance.

R566 terminates with task result 1 after 95 successful profiles, before the next
88.2 kHz / 64-channel recording. Its error is WinError 5 replacing report.pending
with report.json. The live SCP report snapshot may have denied delete sharing;
the exact lock owner is not proven. All completed profiles report success, but
the full matrix remains incomplete. Cleanup, task removal and independent normal
runtime/saved-bytes/defaults/microphone checks pass.

R576 adds a bounded report writer: it retries only Windows permission/sharing
errors during atomic replacement for up to five seconds, preserving the old
complete report. Other errors propagate immediately. Four Windows tests pass,
including a real CreateFile reader that blocks replacement until released;
three portable tests pass locally with that Windows case skipped. The first
PowerShell wrapper treats unittest's stderr as an error; a corrected wrapper
records Python's successful exit. Test sources are unchanged between attempts.
Live progress readers now explicitly allow read/write/delete sharing; do not SCP
the active report. Observation errors never authorize restarting a live test.

R577 starts a fresh full float matrix with only the fixture/report writer changed;
the .24/R560/R544 pair and R462 recorder remain pinned. Its Python PID 9540 is
verified Running and its first profile passes. R578 replaces unstarted R572 for
PCM16 with the same report fix and overlap guards; it remains unstarted.
Evidence: `native-float-r566/terminal-analysis.json`, `installer-webview-r574`,
`report-sharing-r576`, `native-float-r577` and `native-pcm16-r578` under
`artifacts/windows/`.

### Surviving-child capture fixture preparation (R579)

The existing group membership fixture adds/removes independent processes, but
its nested kill-on-close job also kills a child when its parent exits. R579 adds
an explicit capture-only `LOOPZ_GROUP_PARENT_EXIT_ACCEPTANCE=1` case. The child
inherits the controller-owned cleanup job instead of a parent-owned nested job;
creation refuses this mode without job containment. After initial membership
checks, only the owned parent is terminated. The child's pinned process identity
must remain live, and both selector aliases must recover child/independent roots
and the expected 660/1320 Hz public waveform without the parent's 440 Hz marker.
All sources remain on verified virtual endpoints, and controller cleanup still
owns every fixture process.

The Python wrapper records the requested mode and rejects an older probe that
does not report the explicit parent-exit transition. Capture-group planning
currently rejects unavailable ancestors, so surviving-child behavior needs
actual reproduction before changing that policy. Native formatting, Python
formatting/parsing and source hashes pass; the fixture is not compiled or run
while R577 is active. This adds test coverage preparation, not evidence of
surviving-child or seamless handoff support. Evidence:
`artifacts/windows/parent-exit-r579`.

R580 reconfigures the task's local portable Release tree and runs the registered
`loopz-windows-float-report` and `loopz-windows-acceptance-report` CTest entries.
Both pass (0.32 s total), verifying the new report regression is included in the
ordinary test registration. The native Windows lock case is skipped on macOS;
its actual Dell evidence remains R576. No Dell build or audio operation is added.
Evidence: `artifacts/windows/report-ctest-r580`.

R581 downloads the x64 Evergreen Standalone Installer through the link observed
in Microsoft's official WebView2 page component. The HTTPS redirect chain,
214,823,632-byte installer and SHA-256 provenance are retained locally. No binary
is executed or copied to Dell during R577. A prepared read-only Windows signature
verification script refuses active audio/build processes and will verify the
pinned hash, Authenticode status and Microsoft signer after the matrix. Download
provenance alone does not establish trust or successful runtime installation.
Evidence: `artifacts/windows/webview-runtime-r581`.

R582 independently inspects R574's completed runtime-install fixture. Its app
files remain present and contain zero copies of WebView2RuntimeInstaller.exe,
confirming the production Files exclusion keeps the setup-only prerequisite out
of the installed app. The inspection performs no install/build/audio operations
and does not establish actual Microsoft runtime installation. Evidence:
`artifacts/windows/installer-footprint-r582`.

R583 takes one immutable snapshot of the live R577 report using explicit
read/write/delete sharing, closes its stream and copies only that snapshot.
Its SHA-256 matches between Dell and local storage. The unchanged independent
auditor's profile and native-flow functions pass all 112 completed profiles,
including waveform bounds, channel coverage, private float format and native
wait budgets. No report flags are rewritten to make the full-matrix function
accept a partial run. The matrix remains live; complete format coverage,
terminal success, cleanup and installed-state verification are still required.
Evidence: `artifacts/windows/partial-audit-r583`.

### Full current float matrix accepted; Windows 10 scope (R577, R584–R585)

R577 finishes with task result zero and no remaining fixture processes. All 192
exclusive-float rate/width profiles pass, covering six rates and every even
width from 2 through 64 channels. The unchanged independent full-matrix auditor
passes normal native publication mode. Every profile's longest all-channel zero
run is zero; maximum observed native publication wait is 2.3528 ms. The pinned
R560 app/control, R544 helper and R462 reference probe hashes match. Cleanup,
task removal, exact saved-document bytes, all default roles, normal runtime and
saved microphone identity pass. This completes current short float-format
coverage, not PCM16, transitions, shared clients or the wider product gates.
Evidence: `artifacts/windows/native-float-r577/independent-audit.json` and its
bound report, binary verification and post-verification.

R584 records the user's instruction to continue without Windows 11 testing.
The previous OpenSSL 3.6.4 command link is restored and verified after the user
approves continuing; no Windows VM is started. R585 prepares an isolated native
process fixture build from the exact R559 archive plus only the R579 C++/Python
test changes. Original source and overlay hashes are pinned before/after build;
the guard refuses outstanding continuity tasks or active tests/builds. Its
PowerShell scripts parse, but compilation remains pending here. It will not
replace the installed app or reference probe. Evidence:
`artifacts/windows/resume-win10-r584` and `process-fixture-r585`.

R581's real downloaded prerequisite passes Windows Authenticode verification:
status Valid, Microsoft Corporation signer, pinned SHA-256
`9B0142E22E8DAA92855D746AE4D1E9EEEF561CCAF67D1DD452165C4AF3DF3191`.
It is not executed or installed; this establishes artifact signature acceptance,
not the real missing-runtime installation path.

R585 builds the isolated native fixture and passes both focused process-identity
checks with all source hashes verified. R586 then reproduces a product failure:
after the owned parent exits and its child remains live, both source aliases
become offline with `Application ancestor is unavailable`. The fixture had
already exercised the ordinary group and independent-instance transitions.
Its report remains failed. Temporary mixes/root adapters, exact saved-document
bytes, defaults and original microphone are restored; one owned image directory
remains. Subsequent hash-checked cleanup removes only the two recorded fixture
paths after confirming no associated processes remain. Separate cleanup and
post-verification evidence is retained without changing the failed report.

R587 adds a control-thread ProcessCapturePlan candidate that retains handles for
authenticated ancestry nodes across refreshes. Process IDs remain reserved while
those handles exist, including after exit, as documented by
[Microsoft PROCESS_INFORMATION](https://learn.microsoft.com/windows/win32/api/processthreadsapi/ns-processthreadsapi-process_information).
Later plans reuse only pinned evidence; unknown missing ancestry remains an
error. A single selected root keeps its evidence without requiring parent
deduplication. CaptureGroup uses the retained plan while reusing unchanged live
capture roots. Core DSP, driver, queue sizes and delays are unchanged.

New native identity regressions cover a surviving child after parent exit,
rejection without historical evidence, recovery from a single-root history and
another refresh after the original plan is released. Fixture image removal now
retries briefly for exiting-process file locks. Formatting passes. R588 compiles
the candidate in another fresh verified workspace; it is not yet deployed or
accepted. Evidence: `artifacts/windows/parent-exit-r586`, `ancestry-cache-r587`
and `process-fix-r588`. PCM16 remains prepared and unstarted pending this fix.

### Surviving-descendant fix and installed candidate (R589–R596)

R588 passes the two focused Windows process checks against verified candidate
sources. R589 replays the parent-exit failure through the candidate native
Runtime and real virtual audio. The surviving child's 660 Hz and independent
instance's 1320 Hz tones remain present in both channels after parent exit;
the exited parent's tone and excluded processes are absent. The post-transition
24,000-frame recording has zero silent frames, discontinuities and timestamp
errors. Cleanup and exact saved-state checks pass. This isolated Runtime result
does not prove uninterrupted audio across the transition or the installed app's
complete lifecycle. Evidence: `artifacts/windows/parent-exit-r589`.

Frozen R592 overlays the reviewed-in-session candidate files onto the R559 build
snapshot, excluding concurrent macOS/interface changes. Its only production
native changes are process_capture_group.cpp/.hpp; driver and interface inputs
are unchanged. R593 builds the complete desktop/tools and passes 48 selected
checks with source hashes verified before and after. R594 updates only app and
control binaries, then verifies the normal three unelevated processes, unchanged
R544 broker/helper, driver .24, interface, saved-document bytes, microphone UID
and all default roles. Temporary deployment tasks are removed.

Installed LoopZ.exe SHA-256 is
`CF8CD27C067B9302263D250515710E2F3B22EAB66A24F029A51D873D4A81EB2C`;
control SHA-256 is
`94F85D225639F57057EE522229C521A99C7C47ED2557DBEE14220C51C3E1859D`.
R596 starts a fresh 192-profile exclusive-PCM16 sweep against this exact installed
candidate using the unchanged R462 reference probe. Its result is pending;
R595's corresponding float repeat is prepared but unstarted. Earlier R560
format/stress/latency evidence remains bound to that earlier executable.
Evidence: `artifacts/windows/source-r592-delta.json`, `consumers-r593`,
`app-deploy-r594` and `native-pcm16-r596`.

R600 inspects the newly supplied Windows 11 VM without changing boot settings.
R601 transfers a hash-bound development payload containing the exact R593 app,
R544 maintenance components, .24 driver and only the public workbench certificate.
R602 verifies every extracted payload file and confirms initial LoopZ user state
is absent. Its normal-user launch fixture stops before app launch because the
separate test-tool directory lacks runtime DLLs (exit 0xc0000135). R603 records
that failure, supplies byte-identical copies of the existing bundled app-local
DLLs to the test tools, removes the completed failed task and starts a fresh
normal-user launch attempt. R603 then passes normal-account launch, initial engine
state and unchanged audio defaults. R604 captures the actual 1280-by-820 WebView
window, with navigation and native bridge ready; visual inspection confirms the
empty-device interface renders. This does not establish driver/audio acceptance.
Evidence: `artifacts/windows/win11-vm-r600`,
`win11-payload-r601`, `win11-app-r602` and `win11-app-r603`.

R605 transfers 43 native regression cases from the exact R593 build and starts
them in the VM's normal desktop account. Its PowerShell runner loses exit codes
when output is redirected, then reaches its 120-second SQLite-case timeout.
The report remains failed. R607 controls reproduce null exit codes for both 0
and 7 with redirection, while an explicitly retained Process handle preserves
both codes. R608 starts a fresh run of the unchanged binaries with that corrected
runner, elapsed times and a bounded longer SQLite budget. Its first 12 cases
pass; the database case remains pending. R609 observes increasing write counts
and no owned assertion window during that case; this is progress evidence, not
a completed database pass. No VM boot setting or driver is changed here.
Evidence: `artifacts/windows/win11-ui-r604`, `win11-regressions-r605`,
`win11-runner-r607`, `win11-regressions-r608` and `win11-db-r609`.

R608 finishes all 43 native cases with actual exit zero. SQLite durability takes
336.994 seconds in this VM, within its recorded 600-second limit. R611's first
source-free 48 kHz stereo mix is persisted successfully, but its postcondition
checks the nonexistent device.channels field instead of routing.outputChannels.
That fixture report stays failed. R613's read-only audit passes without repeating
the edit. SSH is restored. R635 passes runtime and report checks, preserving the
saved document; these are user-mode checks, not live audio acceptance. Secure
Boot is verified disabled after the user changes the VM settings. Development
test-signing setup is in progress; no driver/audio acceptance is claimed.

### PCM16 regression and conversion candidate (R596, R612–R620)

R596 fails the 96 kHz / 60-channel exclusive-PCM16 profile after 125 successful
profiles. It records 18,368 all-channel zero frames and a 9,728-frame longest
run, despite zero capture discontinuities, position gaps and timestamp errors.
Publication history misses match the zero-frame count; native wait timeouts and
late graph rendering are observed. Cleanup, task removal, exact saved mix,
defaults, microphone identity and normal runtime pass. This is a real failed
matrix, not a report-write failure or completed format acceptance.

R612 reproduces the same format for 15 seconds without concurrent Dell builds
or bulk transfers: 61,518 zero frames, a 14,592-frame longest run and four capture
position gaps. Publication misses, one render reanchor and one driver underrun
also occur. Cleanup and saved-state checks pass. R614's matched 15-second
exclusive-float path at the same rate/width passes with zero waveform zero runs
and zero capture errors; independent profile/native-flow checks pass. This
comparison points toward format-dependent work but does not alone establish the
cause. Original reports remain bound to R593/.24 and the R462 probe.

R616/R617 compare a fixed-width PCM16 codec candidate with unchanged .24 codec
sources under the driver's compiler hardening. The candidate replaces variable
byte-width sample loops and the PCM16 magnitude loop with bounded integer work;
float-to-float logic, audio queues, clocks and buffers are unchanged. Exhaustive
PCM16, random-float, address/undefined-behavior and 192 mixed-format guard/output
checks pass. R618's three alternating offline trials preserve every output hash.
For packet sizes of at least 3,072 samples, PCM16-to-float median time ratios
are 0.406, 0.474 and 0.461; float-to-PCM16 ratios are 0.484, 0.520 and 0.520.
These are offline conversion measurements, not live continuity or total CPU
acceptance. The candidate is versioned .25 with explicit render-only admission;
it has not been installed. R615's earlier measurement fixture is superseded
before timing because its reference evaluated an unnecessary signed expression
for float bits. Evidence: `artifacts/windows/native-pcm16-r596`,
`pcm16-repro-r612`, `float-compare-r614`, `codec-baseline-r616`,
`codec-candidate-r617` and `codec-measurement-r618`.

R620 freezes the .25 codec/version/admission changes and measurement source
without concurrent interface/core changes. R621's WDK build passes with the
original hardening flags. Disassembly of all three codec functions in the actual
WDK common.obj contains no floating-point/vector instructions. R622's full
matching user-mode build passes 48 selected checks. R623 signs SYS/catalog with
the unchanged nonexportable workbench certificate. R624 preserves the previous
stage, installed broker, .24 package and saved state, then native maintenance
migrates one device with exit zero and no reboot. The loaded signed SYS hash is
`73B542A37AAFB6F82C7400788DE3E98140F0FB3B8652F83224094A15F2585CCF`.
The saved document and all default roles remain unchanged; the public microphone
UID becomes `{0.0.1.00000000}.{4464e5b0-29bc-4492-94a0-6e42f296102e}`.
Temporary maintenance tasks are removed and three normal-user processes verified.

R625 repeats R612's exact 96 kHz / 60-channel exclusive-PCM16, 15-second fixture
using R622/.25 and the unchanged R462 probe. It passes with zero all-channel zero
frames/runs, capture discontinuities/position gaps/timestamp errors, native wait
timeouts and worker restarts. Independent waveform/native-flow checks and saved
state/default/microphone/cleanup verification pass. Startup/lifetime driver
missing-frame counters are nonzero, so this does not claim every lifetime counter
is zero. R626 completes 109 passing profiles, then stops while the 96 kHz /
28-channel endpoint remains in format setup. Cleanup and installed-state checks
pass; the 192-profile matrix remains incomplete. R628 freezes diagnostic-only bounded PCM16 recorder/fixture/auditor
support, with app/driver/interface sources unchanged from R620. Local sanitizer,
spectrum/queue checks and 14 report controls pass; its Windows build and live
long-duration acceptance are still pending. Evidence: `artifacts/windows/driver-build-r621`,
`consumers-r622`, `driver-sign-r623`, `maintenance-r624`, `pcm16-candidate-r625`,
`native-pcm16-r626`, `pcm-streaming-r627` and `source-r628-delta.json`.

### Earlier continuity investigations (R407–R435)

Independent mixes now use their own render rate. R407 passed a 60-second,
60-channel, 44.1 kHz float32 half-gain round-trip with no measured capture gaps,
zero audio runs or within-recording worker restarts. The saved 48 kHz mix remained
active. R408 then passed 90 short profiles, including every even channel count
from 2 through 64 at both 48 and 44.1 kHz, before failing at 88.2 kHz / 54 channels.
Its 1.5-second failed recording contains 1,482 all-channel zero frames. Cleanup,
saved document bytes, microphone identity and default devices are verified.
That full R405/.20 float sweep failed; higher rates were not yet
validated. The short-profile passes do not replace long-duration stress tests.

R409 moves optional AVX2 CPU/OS detection into the Windows adapter, preserving a
platform-neutral core. All 58 portable checks and 28 Windows regressions pass, including separate
baseline and optimized resampling tests. The portable-core boundary check also
passes on Windows in a separate run. R410 deploys the exact verified app/control
binaries; normal-user processes, saved mix/defaults and microphone identity pass
post-deployment verification. R412 fails a 60-second 88.2 kHz / 54-channel recording: 133,888 all-channel
zero frames, with a longest run of 16,439 frames. Capture packet/timestamp errors
remain zero. Observed recording deltas include 124,192 publication missing frames,
7,424 skipped scene frames, one scene reanchor and one driver underrun; the
private worker has no restart/deadline miss. The core-boundary cleanup does not
resolve high-rate continuity. Saved bytes/defaults and microphone identity remain
verified, and temporary roots/task are removed.
R413 adds a 64-byte gap between CachedFir temporary channel rows. Six focused
portable checks, six ASan/UBSan checks and all 29 selected Windows checks pass.
Three offline benchmark trials preserve every output hash in all 32 cases;
trial median time ratios are 0.9694, 0.9704 and 0.9581 versus R409. Some shapes
regress, so this is a modest median saving, not uniform acceleration. R414 deploys
the exact source-verified app/control binaries with saved state/defaults and
microphone identity preserved; driver/broker/helper/UI are unchanged.

R415 still fails the same 60-second 88.2 kHz / 54-channel recording: 96,010
all-channel zero frames, longest run 16,607. Within the observed recording there
are 94,021 publication missing frames, but no driver underrun, worker deadline
miss/restart, scene reanchor or skipped scene frame. Render maximum lateness is
70.303074 ms and maximum individual render duration 10.7899 ms. These counters
identify late publication; they do not establish why the thread ran late.
All capture packet/position/timestamp errors remain zero. Cleanup, exact saved
bytes, microphone identity and all defaults pass. The cache improvement does not
resolve continuity; current full-rate acceptance remains open.

R411 measured five idle GPU samples at 0% across the installed app's identified
WebView GPU engines. Idle GPU evidence does not cover interaction or macOS.

R416 records scheduler-only ETW events without sampled CPU profiling or stack
walking on the exact installed R413 app. The trace reports zero lost buffers and
events. Pinned engine PID 2720 / render TID 7760 has 24,584 complete scheduler
gap intervals: maximum off-CPU gap 5.746 ms, maximum ready delay 2.319 ms. Worker
TID 9740 likewise has no off-CPU gap over 10 ms; its maximum is 5.123 ms. Independent
pairing validation finds no unmatched/CPU-mismatched render switches, and the
largest DPC/ISR event is 3.534 ms. The minimal profile omits loader metadata, so
routine module identities remain unknown. Scheduled residency is not CPU time
and includes interrupt/DPC activity. This traced run does not establish a long
scheduler stall as the cause of late publication, nor does it prove ordinary
untraced scheduling is always healthy. The recording still contains 62,258 zero
frames and is diagnostic only. Trace and test task are stopped/removed; saved
state/defaults remain verified. Evidence: scheduling-r416 reports, pinned ETL/CSV
hashes and exact event files retained on the Dell.

R417 temporarily relaunches the same R413 binaries with existing process-local
stage timing enabled; R418 records the same 60-second 88.2 kHz / 54-channel
virtual-only path without ETW. Observed stage-duration deltas are input 12.405 s
(57.60%), graph 7.663 s (35.58%), output/history/meter 0.344 s (1.60%) and sink
1.127 s (5.23%). These are wall-clock stage durations, not isolated CPU profiles;
preemption and instrumentation can contribute. The recording still fails with
58,357 all-channel zero frames. Input and graph processing are the next targets
for CPU attribution before another optimization. R419 restores normal launch,
verifies stage timing disabled, preserves exact binaries/saved mix/defaults and
microphone identity, and removes its temporary tasks. WPR is not recording.

R420 adds a pinned offline complete-graph benchmark without changing production
DSP. R421's candidate initially fails the independent reproducibility test because
its expected generated digest still names the old output; that failed result is
retained and not deployed. R422 updates that expected digest to the new explicitly
pinned generated source (vendor-input and overlay-tamper rejection checks remain).
It vectorizes graph-runner containment on SSE2, preserving scalar behavior for
partial overlap, exact finite bits and saturating non-finite counts. All 58
portable checks, five focused sanitizer checks and 33 Windows checks pass.
Three complete-graph benchmark trials preserve output hashes in all 30 cases;
median ratios are 0.9517, 0.9540 and 0.9505. The 54-channel 256-frame identity
half-gain/limiter case is 9–13% faster. Some shapes vary; this is offline evidence.
R423 deploys the exact verified binaries with saved state/defaults and microphone
identity preserved; driver/broker/helper/UI remain unchanged.

R424 still fails the same uninstrumented 60-second 88.2 kHz / 54-channel path:
106,328 all-channel zero frames, longest run 13,959. Within the observed recording,
publication missing frames increase by 106,328 with no driver underrun, worker
restart/deadline miss, scene reanchor/skipped frame or completed-ingress zero run.
Capture packet/position/timestamp errors remain zero. Cleanup and exact saved
state/defaults pass. The graph saving does not resolve continuity.

R425 admits nearly equal clock-rate fractional reads to the existing
cached FIR while preserving their exact clock position and filter. It declines
any block containing a near-integer direct-copy frame before reading the provider,
so mixed direct/FIR blocks keep the original tiled implementation. New reference
checks keep the complete time grid and read narrow channel groups, forcing the
original tiled FIR; splitting the time grid at an exact phase threshold can
otherwise change a direct/FIR decision through floating-point reassociation.
All 59 portable checks, 35 selected Windows checks and two ASan/UBSan resampling
checks pass (116.64 s total sanitizer time). Baseline and AVX2 Windows tests each
run the new reference cases separately. R426 deploys the exact verified binaries
with saved state, all defaults and microphone identity preserved; driver, broker,
helper and UI remain unchanged. Normal timing is retained; stage timing and ETW
are off.

R427 still fails the unchanged 60-second 88.2 kHz / 54-channel recording: 43,718
all-channel zero frames, longest run 12,973. Within the observed recording,
publication missing frames increase by 43,339 with no driver underrun, worker
restart/deadline miss, scene reanchor/skipped frame or completed-ingress zero run.
Capture packet/position/timestamp errors remain zero. Cleanup and post-state
verification pass. Fewer zero frames in one run do not establish a reliable
continuity improvement. R428's full matrix fixture is prepared but deliberately
not started while this critical gate remains failed. Next: function-symbol CPU
attribution on the exact optimized DSP using a separate diagnostic build; retain
and restore the normal installed baseline rather than alter shared timing or
priority settings based on indirect counters.

R429 builds exact frozen R425 source in a fresh workspace as optimized Release
with /O2 /Ob2 /DNDEBUG /Z7 and full linker symbols, without changing DSP source.
All 35 selected Windows checks pass (42.69 s); executable/PDB hashes and compiler
flags are retained. R430 temporarily deploys only the diagnostic app/control pair.
R431's WPR export fails; R433 separates native stdout/stderr and exposes native
error 0x80010106 (Cannot change thread mode after it is set, RunningProfile).
Both audio tasks finish and clean up; neither exports an ETL or supports CPU
attribution. Those terminal failures are preserved rather than retried in place.

R434 uses direct xperf recording only after checking the NT Kernel Logger is free.
It stops only that owned logger, retains its raw ETL and separately merges the
trace. No shared profiling interval/priority setting is changed. Merge succeeds,
with zero lost events/buffers. Initial event-name-filtered stack exports are empty
and not used; re-exporting the same trace without that incorrect filter yields
populated native XHTML reports for pinned engine PID 4232, renderer TID 2172 and
private worker TID 1164. Matching local PDBs resolve the app and loaded driver.

Of 19,197 selected worker-context exclusive stack hits, 8,988 (46.82%) are in
LoopZAudio.sys!loopz_driver::convertPackedSamples. Its caller table attributes
8,255 hits to CAdapterCommon::TransportProperty and 398/371 to BridgeWrite/Read.
Of 20,858 renderer-context hits, 5,949 (28.52%) are in that conversion routine and
4,806 in unity convolution/FFT functions. These reports cover captured thread
lifetimes including setup/cleanup and kernel/interrupt work, with profiling
perturbation. They are not isolated user CPU time, power, ordinary throughput,
or continuity evidence. The diagnostic recording still has 368,366 zero frames.

R435 pins the loaded .20 SYS hash and codec source to the R324 build. Its actual
compiler flags include /Ox /Os /Oy- /Qspectre /kernel: it was already optimized.
Disassembly of the sampled conversion function (RVA 0x2680–0x2834) confirms
runtime-width byte decode/store loops and an lfence inside the per-sample loop.
Next: benchmark a fixed-width integer float32-to-float32 sanitizing/copy path,
preserving bounds checks, non-finite containment, security mitigations and the
kernel no-FP constraint, before installing any driver change.

R432 restores the exact normal R425 app/control pair, verifies normal-user
processes, disabled stage timing, saved mix/defaults and microphone identity,
and removes its temporary tasks. The direct kernel recorder is verified absent;
other loggers remain untouched. Driver .20 and broker/helper R391 are unchanged.
The full port remains incomplete. Evidence: symbols-r429, cpu-r431/r433/r434,
driver-flags-r435 and app-restore-r432 artifacts.

Production signing/distribution, clean Windows 10 and Windows 11 installation,
upgrade endpoint reselection, hardware/third-party-client checks, DPI/accessibility,
and extended recovery/continuity remain release gates. This port is incomplete.

## Historical integration evidence

The dated runs below describe their own pinned binaries and settings. Later
results supersede their installed-version statements; do not treat an older
"current" label or successful narrow check as release readiness.

The Dell Windows 10 22H2 workbench runs development driver 0.4.0.20
(R324 source, R325 installation/reboot), private float transport v3, the R327
desktop/per-user engine (app-only deployment R328), and the R298 LocalSystem broker/helper.
The failed .19 endpoint metadata experiment is removed from source and the loaded
kernel image. Autologon and the non-elevated app recovered after the reboot.
R326 verifies the PnP-parent lookup against the loaded .20 driver: render-only
sink creation, 48 kHz stereo silent playback, idempotent setup and removal pass.
Both parent and topology identification succeed. This does not prove a causal
comparison against the earlier intermittent topology-only failure.

R327 fixes saved-mix status coordination when the shared intent snapshot contains
an active or pending-removal routing sink. Tests verify mix readiness, explicit
mix removal, preserved sink intent and shared revision ordering. All four focused
native checks pass. The R327 app/control binaries are deployed at R328; the installed broker/helper
remain R298. Public sink requests remain disabled and routing leases still need
runtime integration. R328 verifies the exact installed app hashes, normal-user
processes, microphone reconnection, unchanged interface assets, saved document,
endpoint identity and six defaults. Its isolated five-phase private-transport
regression passes with cleanup; that test is not an engine round-trip or new
latency measurement. Evidence: desktop-r328-deployment-summary.json and
private-r328-report.json in artifacts/windows.
R324 native incremental-build results were superseded by R326: preserved archive
timestamps had prevented an affected object from rebuilding. R326 logs prove
recompilation; the fresh-directory R324 kernel build was unaffected.
Evidence: driver0420-r326-integration-summary.json, source-r326 build logs,
and source-r327-consumers-build-tests.log in artifacts/windows.

The desktop, supervisor and engine are non-elevated. The saved mix and six audio
default roles are preserved. The R325 .19 → .20 upgrade changed the microphone endpoint GUID and required
a reboot; the saved logical mix reconnected. The 0.4.0.15 → 0.4.0.16
upgrade did change the public microphone endpoint GUID; the logical mix reconnected.
External-app reselection handling remains an acceptance gate. Development signing
is enabled as requested; this is not Microsoft production signing.

The terminal R274 controlled format sweep tested the rebuilt R272 app
with publication waiting and streaming chunks enabled. R272 passes the previously
failing 30-second 88.2 kHz / 54-channel recording without zero audio frames or
capture gaps. The full sweep failed after 57 profiles at 44.1 kHz / 52 channels with a
recording FIFO underrun. Cleanup and exact R246 app/control restoration are
verified; both temporary options are OFF again. R278 diagnostics/probe targets
have built and passed their focused tests; none is deployed and no audio
acceptance test is active.

Automatic microphone setup now follows durable saves through the runtime
coordinator and protected SQLite intent store. R228 verifies creation, attachment,
rename, backup-recovery authority and removal. Elevated maintenance now migrates
existing desired roots between packages; routine broker requests cannot do so.
R251 migrated the original root through the native helper without a reboot.
The installer/service scripts build, but standard-user UAC consent/cancellation,
clean installation and end-to-end signed installer/update acceptance remain open.

Windows 10 per-app capture has R230 isolation evidence and an R232 30-second
selected-app → mix → public-microphone recording, excluding a simultaneous sibling
marker. Those recordings used the earlier PCM16 driver. Third-party application,
new-driver selected-app and extended lifecycle acceptance are still needed.
Use --silent-probe FEED_UID for virtual-only isolation, not the older audible mode.

R239 completed all 192 rate/channel profiles with the PCM16 driver. That result
is not transferred to the new float driver. R252 passed a 30-second explicit
float32 recording at 48 kHz stereo; R253 passed PCM16 public-client compatibility
with float private transport. R254's new-driver full sweep passed stereo, then
failed the 2 → 4 channel setup change. The R256 broker now prefers float32 format
selection with explicit PCM16 compatibility fallback. R259 passes the previously failing 2 → 4 channel transition, then fails later
at 88.2 kHz / 54 channels after 90 passed profiles. The normal baseline leaves both
streaming-chunk/publication-wait experiments disabled; the controlled R274 test
temporarily enables them as described above.

R258's 30-second low-amplitude recording passes at 48 kHz stereo. An input
amplitude of 1e-6 produces the expected 5e-7 output through the half-gain mix,
below a PCM16 step, with no capture gaps or silent frames. This rules out hidden
PCM16 truncation on this path; it is not bit-exact/full-mantissa fidelity proof.
R259 is terminal: 90 profiles passed before an 88.2 kHz / 54-channel recording
failed. Driver 0.4.0.17 fixes a reproduced timestamp-rounding error; R267
confirms zero input timestamp resets but still fails on unpublished mixer output. R283 refreshes latency on the .17 driver below, with its public-client format
and experimental app settings recorded separately.

Current scoped evidence is in artifacts/windows: desktop-r272-installed-verification.json,
format-r272-report.json, format-r272-post-verification.json and
publication-wait-r272-installed-summary.json. Earlier float format and repair
results remain in their corresponding R252–R267 reports.
The R256 build and native codec checks pass. Earlier dated sections are historical;
check live processes and terminal reports before treating any handle/state as current.
The old public ZIP remains an app-only preview, separate from the workbench package.

R287 verifies a default-following marker's process capture before and after
routing between two temporary virtual Feeds on Windows 10. Both measured windows
have zero missing frames; one cumulative discontinuity is reported during the
handoff. This is not a seamless transition or production mute-original feature.
Original preferences, saved mix, microphone and six defaults are preserved, and
the installed app remains R246.

Original-app playback muting, reselection UX across upgrades, production signing
and installer distribution, Windows 11/clean-install acceptance, new-driver full
format coverage and precision/extended recovery checks, native accessibility,
DPI and third-party-app verification remain unfinished.

R330's current-build full float sweep stopped after two passing profiles: the
48 kHz four-to-six-channel transition reported reboot_required before the new
format became ready. Cleanup and saved-state checks passed. R331 adds a runtime
guard against reopening a KS transport whose endpoint inventory still reports
the previous format; the candidate is deployed at R332 after four focused native
tests. R333 passed the prior failing transition and eleven profiles, then stopped at
48 kHz/24 channels with another reboot_required result before waveform probing.
Cleanup, test-root absence, normal-app reconnection and saved-state preservation
are verified. The guard does not resolve the intermittent PnP restart failure;
the full format matrix remains failed. The installed
app is now R331; driver .20 and broker/helper R298 are unchanged. The R329 latency
measurement below applies to the earlier R327 app. Evidence: format-r330-summary
pending-format-r332-summary, and format-r333-summary in artifacts/windows.

R334 diagnostic retry investigation passed 61 profiles without a reboot veto,
then failed waveform continuity at 44.1 kHz/60 channels: 691 consecutive recorded
all-channel zero frames, one driver FIFO underrun, zero publication missing
frames and zero timestamp resets. Worker maximum lateness was 20.96 ms; this
observation does not by itself prove the scheduling cause. Driver missing-frame
totals also include startup and must not be equated with the recorded zero-run
length. Both timing experiments were OFF. No retry occurred, so delayed recovery
is still unproven. Cleanup, normal app, saved state, defaults and test-root/task
removal are verified. Windows System Kernel-PnP event 225 identifies Audiosrv as
the vetoing process for the exact R330 and R333 test roots; its handle-retention
cause is not yet proven. Evidence: format-r334-summary and pnp-r334-veto-summary.

R335/R336 paired-channel SSE2 FIR computation trials show a 9.05% median saving
across four runs per variant (two ABBA comparisons), with 80 bit-exact sample and
coverage parity cases. Promoted source R337 passes two portable and six Windows
regressions and is deployed as app/control R338; driver .20 and broker R298 remain.
This is not a live CPU comparison. R339's 30-second 44.1 kHz/60-channel recording
still fails: 10,378 all-channel zero frames, longest run 7,158 frames, one driver
underrun and 9,755 publication missing frames. Its greater duration differs from
R334, so totals are not a before/after comparison. The optimization does not
resolve live continuity. Cleanup and normal saved-state verification pass.
Evidence: fir-r336-comparison, fir-r338-integration-summary and format-r339-summary.

R340 adds diagnostic-only counts of actual post-clock-segmentation resampler
blocks (1–31, 32–127, 128–256, >256 frames). A two-reader counter regression and
all six focused Windows checks pass; R341 deploys the exact diagnostic app.
R342's 30-second 44.1 kHz/60-channel repeat still fails (588 total recorded zero
frames, longest run 532). Across that main gap, publication missing frames and
short-block counts stay unchanged, while driver missing frames increase exactly
532 (630 → 1,162). Short-block fallback is therefore not supported as the cause
of this specific gap; no small-block fast-path change is made. Final ingress
block counts are [0,2,5857,0], publication [7,17,6584,0]. Cleanup and normal saved
state pass. Driver .20, broker R298, timing options and buffers are unchanged.
Evidence: history-block-r341-deployment and format-r342-summary in artifacts/windows.

R343 isolates chunked publication ON with publication waiting OFF, using the
same installed R340 app, 30-second 44.1 kHz/60-channel profile, buffers and waveform
checks. It still fails: 2,798 all-channel zero frames exactly match publication
missing frames; longest zero run is 2,307 frames. The driver reports no FIFO
underruns in this run. One run does not prove chunking causally eliminates
underruns or resolves continuity. Both experimental options are restored OFF;
normal app, saved state, defaults, root removal and task cleanup pass. Evidence:
format-r343-summary and format-r343-post-verification in artifacts/windows.

R344 enables both publication waiting and chunked writes for the same 30-second
44.1 kHz/60-channel case. It fails with one 518-frame recorded gap and one driver
FIFO underrun, while publication missing frames remain zero. There are 62
publication waits, zero wait timeouts, and a maximum wait of 3.465 ms. This does
not establish a continuity fix. Original options are restored OFF and normal
saved-state, defaults, device/task cleanup checks pass. Smaller publication
batches at unchanged FIFO capacity/prefill remain an untested hypothesis, not a
selected production behavior. Evidence: format-r344-summary/post-verification.

R345 implements opt-in `LOOPZ_EXPERIMENT_SMALL_BATCHES=1`: 300 publication
batches/s instead of 100, with integral frames at every supported rate (147 at
44.1 kHz). Deadlines derive from an origin and batch index rather than accumulating
rounded intervals; one-hour 1/300-second-grid, sample-count and overflow tests
pass. All seven focused Windows regressions pass in 9.57 seconds. Exact candidate
binaries are preserved but not deployed. Driver FIFO capacity, 20 ms prefill,
30 ms worker failure limit, filters and normal 100-batch/s default are unchanged.
This is not evidence of live continuity improvement. Evidence:
publication-cadence-r345-plan and source-r345 verification/build-test logs.

R346 deploys the exact R345 candidate and verifies its normal 100-batch/s mode.
R347 enables 300 batches/s with waiting and chunked writes ON for a 30-second
44.1 kHz/60-channel recording. The recording passes with zero all-channel zero
frames, capture position gaps, discontinuities and timestamp errors. Driver
underruns are zero; all 49 in-recording snapshots show stable driver missing
frames (1,063, including startup) and zero publication missing frames. Batches
are 147 frames and maximum batch rendering is 5.93 ms. This is not a live CPU
saving claim, proof of intermittent-failure resolution, or full-format pass.
FIFO capacity, prefill and waveform checks are unchanged. Normal 100-batch/s
cadence and both other options are restored OFF, saved-state checks pass, and
the test device/tasks are removed. Current installed app is R345 (R346 deployment),
driver remains .20 and broker/helper R298. Evidence: format-r347-summary and
format-r347-post-verification.

R348 repeats the same opt-in 147-frame cadence for 60 seconds and fails:
46,426 recorded zero frames (longest run 45,299), one FIFO underrun, zero
publication missing frames, and 42,530 driver lease-silence frames. The worker
records one 31.4885 ms deadline miss, exceeding its unchanged 30 ms failure limit,
and reconnects once. Post-reconnect maxima and CPU time describe the new worker;
they must not be used to deny the earlier deadline miss. The long silence includes
worker shutdown, lease expiry and delayed control-thread reconnection, not merely
the initial FIFO depletion. The earlier 30-second pass does not resolve
intermittence. Normal cadence/options and saved state are restored and verified;
test root/tasks are removed. Evidence: format-r348-summary/post-verification.

R349 fixes measured idle GPU use in the Dell desktop by making the persistent
Windows warning status dot static. Three GPU-engine samples before the CSS change
average 20.7% and 32.3% on the two 3D engines; three after samples report 0% on
both. These percentages are separate engines and must not be added or presented
as power consumption. The static warning remains visible. The desktop reload
preserves audio engine PID 5504, the native R345 app/control binaries, saved mix,
microphone identity, six defaults and normal timing options. Bridge/navigation
and exact CSS hash pass verification. This is a CSS-only development patch of
the existing bundle, with unchanged JS and explicit patch provenance; no unrelated
current interface edits are deployed. Source fix is in interface/src/desktop.css;
formatting passes. Evidence: gpu-r349 before/after/patch/verification/summary.

R350 repeats normal cadence/options for 60 seconds after the GPU CSS fix. It
still fails at 44.1 kHz/60 channels: 19,788 recorded zero frames, 19,116 publication
missing frames and one FIFO underrun, with no deadline miss, restart or lease
silence. This does not undermine the measured GPU improvement, but it does not
establish an audio continuity fix. Saved state, normal options, root and task
cleanup pass. Evidence: format-r350-summary/post-verification.

R351 separates worker recovery from the three-second inventory/JSON refresh.
The serialized background control owner checks workers every 100 ms; newly
observed faults can retry immediately, persistent failures keep a three-second
per-output backoff, healthy observations re-arm recovery, and successful starts
clear stale output errors. Inventory refresh cadence and the 30 ms worker failure
limit are unchanged. Portable policy regression and eight native Windows checks
pass (9.13 seconds). Exact binaries/source hashes are preserved; this candidate
is not deployed, and actual live recovery latency remains unmeasured. It is not
a dropout fix. The installed app remains R345/R346 with the R349 CSS patch.
Evidence: worker-recovery-r351-summary and source-r351 verification/build-test logs.

R352 deploys the exact tested R351 recovery app/control while retaining and
verifying R349's GPU CSS fix. R353 validates background-only recovery with the
desktop closed: an authenticated, newly created stereo test mix's private worker
is paused for 45.4931 ms and always resumed in finally. It records the expected
41.2986 ms deadline miss, reconnects once, and its replacement thread's creation
time is approximately 87.63 ms after resume, before the first observer query
(after a 350 ms observation delay). GetThreadTimes creation precision differs
from the precise wall clock; this is not an exact or worst-case reconnect bound.
Saved-mix workers do not restart. The deliberate fault produces 8,400 recorded
zero frames; fixture success proves recovery, not waveform continuity. Normal
format acceptance still rejects gaps and its verdict is unchanged unless the
explicit, stereo-only fault opt-in is used. Desktop and the same engine PID 8344
are restored/verified, along with native binary hashes, normal timing options,
GPU CSS hash, saved document, microphone identity and six defaults. The test root
and temporary tasks are removed. Current native app is R351/R352. Evidence:
recovery-r353 report, verification, post-verification and summary; reusable fixture
is tests/windows_worker_recovery_fault.py with an opt-in in windows_format_acceptance.py.

The same modified harness then passes an ordinary 48 kHz stereo float recording
with fault mode explicitly OFF, native waveform verdict true and zero recorded
zero frames. Its separate report and final root/task cleanup are verified. This
control guards against treating the intentional-fault exception as normal audio
acceptance. Evidence: recovery-r353 normal-report/normal-verification/
normal-post-verification and summary.

R354–R356 add a silent, owned-app exit/relaunch routing probe. Every case uses
one unique copied executable identity and only job-contained children. R355
shows all three persisted preferences still target its Feed after relaunch, but
the new automatic stream is on the system output even after a one-second settle.
Its failed inheritance assertion is retained. The earlier combined
preferenceSurvivedExit field also required the stream to move; R356 separates
these observations instead of interpreting that field as lost preferences.

R356 then reapplies the same target through the new active PID: the stream moves
to the Feed, and restoring the exact original three roles moves it back. The
first PID is 856, the second 9076; reading the dead PID's preference fails. The
complete lifecycle fixture passes, including unchanged saved document bytes,
original microphone UID, six system defaults, removed temporary roots and marker
images. Independent post-verification retains the R351/R352 app, engine PID
8344, normal timing flags and R349 GPU CSS. The source snapshot, compiled object
log and exact test executables are retained in routing-lifecycle-r356; R354/R355
raw failures remain separate. This establishes behavior only for this owned
renderer on this Windows 10 workbench. Production routing still needs durable
identity/journal integration, verified stream movement and relaunch handling;
this is not third-party-app or full Windows-port acceptance.

R357 runs the unchanged captured-tone routing mode using the exact R356 test
executables against the installed R351/R352 app and current driver. Captured
440 Hz amplitudes are 0.0299998 and 0.0299958 across two temporary Feeds, with
zero missing frames in both measured windows. The second observation includes
one discontinuity; this is not seamless-switching proof. The child stops
rendering before original policy restoration. Defaults, document bytes and
original microphone identity are preserved, both temporary roots/images are
removed, and the four R354–R357 task registrations are removed. Post-verification
preserves the normal installed runtime and GPU CSS. Evidence is in
automatic-capture-r357 report/verification/post-verification/summary. This remains
owned-renderer process capture, not a selected third-party app through the public
microphone. No installed production app, broker or driver binary was replaced by
these probe-only changes.

R358/R359 add a Windows user-storage adapter for the portable SQLite routing
journal. AppRoutingStore defaults to LocalAppData/LoopZRoutingRecovery and creates
a dedicated protected directory owned by the authenticated process user, with
access limited to that user, SYSTEM and Administrators. Existing unexpected
ownership/access is rejected, not repaired. It accepts ordinary local fixed-drive
paths, pins all ancestors and the database against rename, rejects reparse points
and hard-linked files, validates recovery sidecars before SQLite can open them,
and refuses an impersonating control thread. It trusts the same Windows user,
SYSTEM and administrators; it is not a boundary against those principals.
SQLite is closed before the retained handles are released. The portable journal
schema, receipts, transaction rules and existing saved-document storage remain.

R359 x64 Release builds and passes two focused native tests (0.51 seconds).
The exact retained executables also pass in the signed-in desktop with token
elevation explicitly verified false. Coverage includes persisted original roles,
two concurrent connections, directory/database rename denial, root/ancestor
junction refusal without creating a redirected child, database hard links,
permissive directory/database/sidecar ACLs, traversal/ADS/trailing-dot paths,
foreign-data preservation, and thread impersonation rejection. An owned child
commits a receipt then calls _Exit without SQLite/C++ destructors; reopening the
store retains its exact baseline and attempted target. This checks post-commit
process-exit recovery, not interrupted writes or storage-device failure. No
fixture directories remain; the temporary task is removed. Source, archive,
manifest and executable hashes independently match. Installed app/control,
engine, GPU CSS, normal audio flags, original microphone, six defaults and saved
mix bytes are verified afterward. Evidence: routing-store-r358 build records and
routing-store-r359 build/desktop/post/cleanup verification, logs and summary.
This adapter is built and tested but not yet used by the production routing
controller or deployed app. Application identity, actual policy leases and sink
coordination still require integration and acceptance.

R360 adds AppRoutingLease as a portable control component beside the journal.
It durably prepares the exact original three preferences before any setter,
rechecks identity and receipt revision around reads, verifies all target roles
before confirmation, and retains receipts after ambiguous setter/read/verification
failures. Release compares each current role with owned targets, restores only
owned values, verifies the final preferences, and CAS-forgets the receipt. An
existing unowned preference, including a user returning to the original output,
relinquishes the lease instead of being overwritten. Callers must latch the
UserOverride result until a new user request, not reacquire on every poll. The
protocol requires one serialized policy/journal owner per application. Windows'
private API offers no atomic compare/set against a simultaneous user change;
read-before-write and final verification do not eliminate that race. Stream
movement remains a separate platform check.

Portable lease/journal tests pass (0.53 seconds), as do ASan/UBSan variants
(1.16 seconds). Fake-policy tests check durable-before-mutation ordering on every
setter, same-target reapply, partial setter/read failures, lost restoration
acknowledgments, current/older-target/original-output user overrides, stale
receipts, changed identity and failed final verification. Three exact-source
Windows native checks pass (lease, journal, storage; 1.22 seconds).

The R360 silent lifecycle fixture now uses the actual lease controller and
Windows AppRoutingStore rather than an in-memory restore helper. Its adapter
pins/checks the uniquely copied owned child's process handle and image path at
each policy operation; that narrow identity adapter is not authorization for
arbitrary user applications. It routes the owned child, stops it, destroys and
reopens the controller/journal, verifies its receipt survives, launches the same
owned image with a new PID, reapplies the target, verifies real stream movement,
and restores original preferences/output before clearing the receipt. PID 5288
becomes 9892; all durability/reapply/restoration assertions pass. An ordinary
restoration failure retains the isolated image and recovery journal for recovery
rather than deleting the receipt in cleanup. The live run has no failure and
removes both temporary roots and marker/storage directories.

Exact source/archive/manifest/executable hashes independently match. Final
verification preserves installed R351/R352, engine PID 8344, the R349 GPU CSS,
normal timing flags, saved mix bytes, original microphone UID and six defaults;
the R360 test task is removed. No production binary was deployed. Evidence:
routing-lease-r360 portable/sanitized/build logs, build/live/post/cleanup
verification, exact executables and summary. General Windows application
identity, production lease ownership/user-override state, typed sink coordination,
third-party behavior and the wider release gates remain open.

R361 adds AppRoutingProcess and ProcessRoutingPreferences as Windows control
adapters. The selected executable must match the live process's resolved path
and file identity. The adapter holds a process handle, records creation time,
authenticates primary-token user and Windows session against its caller, refuses
thread impersonation, and checks identity around every private preference API
call. Recovery constructs a new adapter explicitly and must match the receipt's
application key; an old object never follows a new PID. This is application/path
selection, not publisher signing or executable-content authentication. Holding
the process object prevents PID reuse while it is referenced; see Microsoft's
[process-ID lifetime explanation](https://devblogs.microsoft.com/oldnewthing/20110107-00/?p=11803).

Desktop journal keys contain the current user SID and normalized volume-GUID
executable path, obtained through
[GetFinalPathNameByHandleW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew).
Packaged keys instead use the current user SID and opaque
[GetApplicationUserModelId](https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getapplicationusermodelid)
value; the package full name from
[GetPackageFullName](https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getpackagefullname)
is retained for live-instance checks. Packaged hosts without a distinct app ID
are refused. Durable keys omit PID, creation time and session so a correctly
re-resolved app can match its receipt after relaunch. Actual packaged-app updates,
shared hosts, multi-instance policy semantics and stream routing still require
acceptance; metadata identity alone does not establish those behaviors.

Four exact-source Windows checks pass in R361 (identity, lease, journal, storage;
1.40 seconds). Owned-process tests cover a Unicode image path, upper-case path
selection and launch, identical executable basenames in different directories,
wrong selection/recovery keys, dead-process rejection and impersonation refusal.
The exact identity executable also passes with token elevation verified false.
The real silent journal-backed routing fixture now uses these production-capable
adapters instead of its narrow test-only identity implementation. PID 10620
becomes 11004, with distinct creation times and the same verified application key;
receipt reopen, same-target reapply, actual stream movement, original preference
and stream restoration, and receipt release all pass. Both temporary roots,
marker/storage directories and the test task are removed. Source, archive and
exact executable hashes independently match. Installed app/control, normal audio
flags, engine PID 8344, saved mix bytes, original microphone, six defaults and
R349 GPU CSS remain verified. No production binary was deployed.

R362 adds a read-only identity-inspection mode to the test executable and passes
its native process regression (0.14 seconds). Its first desktop metadata script
exits before a report because it requires protected service configuration fields
that are null under the normal account. That terminal failure is retained. R363
uses the exact same verified R362 executable in a fresh fixture case, preserves
per-app results, and obtains the service reference through a separate read-only
administrative query without changing access rights. At elevation=false it
successfully validates and rechecks app-model recovery keys for already-running
SearchApp, ShellExperienceHost and SystemSettings in session 1. It invokes no
audio preference API on these apps. The protected LocalSystem broker (PID 4700)
is rejected at OpenProcess with access denied; that observation does not prove
inspection of its foreign user token. The ordinary account independently sees
service state/PID but null StartName/PathName, confirming the first script's
precondition issue. Both metadata task registrations are removed; final runtime,
GPU patch and saved/default/microphone checks pass. Evidence is in
routing-process-r361 build/identity/live/post/cleanup verification and summary,
routing-process-r362 source/build/failure summary, and routing-process-r363
read-only observations/reference/visibility/post/cleanup verification and summary.
General production runtime selection, policy ownership/user-override state and
typed sink coordination remain unintegrated; this is not packaged-app audio
routing, third-party microphone or full-port acceptance.

R364/R365 add tri-state routing-sink demand planning and extend the existing
asynchronous broker coordinator to reconcile typed sink intents in the same CAS
revision stream as saved microphones. Unknown demand emits no sink edit or typed sink status query; the shared owner
snapshot is still read and validated for CAS. Pending receipts require retaining
a sink even when the saved
document is absent; desired apps require it only from authoritative saved state.
Release requires authoritative desired state and examined recovery with no
remaining need. The coordinator rejects an unauthoritative release submission.
Existing callers default to Unknown, retaining their saved-mix-only behavior.
Sink status uses the strict version-2 appRoutingSink query and a separate optional
state field. Accepted intent metadata is not live endpoint or audio readiness.

R364's native build stops on MSVC's /W4 /WX test-variable shadowing warning; the
failed build log is retained. R365 renames that test variable without weakening
warning flags. Exact-source x64 Release desktop, control, setup/embedded-broker
and broker consumers build; fifteen focused Windows checks pass (18.92 seconds).
The current portable planner regression passes (0.60 seconds). Four exact native
coordination/protocol/database executables also pass with token elevation
verified false and no remaining temporary database directories. Those tests use
injected coordinator transport and isolated SQLite databases, not live broker
or device operations. They cover mixed intent revision ordering, default sink
isolation, receipt-only retention without a saved document, explicit removal
status, lost replies, cancellation/supersession, invalid snapshot/format/revision
inputs, and release preconditions.

Source/archive/manifest and exact retained consumer/test executable hashes
independently match. The temporary desktop task is removed, and final checks
preserve installed R351/R352, engine PID 8344, normal timing flags, R349 GPU CSS,
saved document bytes, original microphone UID and six defaults. No candidate app,
helper, broker or driver is deployed. The public DeviceIntentStore sink guard
remains intact. Runtime aggregation of authenticated routing demand, examined
journal state and persistent user-override handling still needs integration
before enabling it. Evidence: sink-coordination-r364 failure log/summary and
sink-coordination-r365 portable/build/desktop/post/cleanup verification, exact
consumer executables and summary. This does not establish live provisioning by
the new coordinator, third-party routing, waveform continuity or full-port
completion.

R366–R368 connect deferred output-choice acknowledgment to native runtime
persistence. AppRoutingLease can restore/verify preferences while retaining its
receipt. When an external choice is observed, callers can keep that evidence
until desired state is saved rather than forget it before saving. Reopening a
controller with the retained receipt recognizes original/non-owned preferences
as an override and does not reacquire the route. Default acknowledgment behavior
for existing callers is unchanged; no journal schema migration is introduced.

Runtime::acknowledgeApplicationOutputOverride accepts the authenticated pinned
process preference adapter, requires authoritative saved routing, and clears only
matching application bindings' muteOriginal flags, including disabled aliases.
Canonical app keys and authenticated legacy executable selectors are supported;
unrelated bindings and routing controls remain unchanged. This is a native
control-owner API, not an exposed JSON command or an automatic detector. It uses
the normal atomic saved-document/edit-history commit path, factored into
commitDocument and also used by ordinary edits. A failed save leaves in-memory
commit/receipt acknowledgment unfinished; callers must not forget the receipt
until the saved desired state permits release.

R366 builds exact-source x64 Release consumers and passes fifteen focused native
checks (18.98 seconds). Portable lease/journal regressions pass (0.88 seconds),
as do ASan/UBSan variants (1.13 seconds). New cases retain original/non-original
user overrides across journal reopen and acknowledge them only afterward.
R367 adds structured failure reporting to the opt-in live fixture but its strict
build catches a missing explicit success return in the renamed entry function.
A task was mistakenly started before that build failure was inspected; Python
could not open the not-yet-copied fixture script, so no fixture/device code ran.
Its terminal failure and task cleanup are retained. R368 corrects the return and
adds start/run preflight checks requiring successful source verification and
exact native probe hashes. Warning settings remain strict. Production sources
and retained consumer/lease-test binaries are identical to native-tested R366;
only the live fixture error boundary/return differs.

The verified R368 desktop fixture passes with a normal-user launch guard. It
uses an owned all-silent marker and two temporary virtual Feeds. A separate
Runtime document is initialized without broker/device management; it never loads
or edits the user's original routing file. The fixture applies an owned route,
simulates an external output choice on that owned process, retains the receipt,
and holds the document without delete/write sharing to force a real save error.
The unchanged document and retained receipt are verified. It reopens runtime and
journal, verifies the output choice remains, successfully saves both matching
bindings while preserving an unrelated binding, and only then releases the
receipt. A second runtime reopen loads the saved muteOriginal=false choice.
Fixture original preferences/output are explicitly restored. This validates an
ordinary sharing-error/reopen sequence, not sudden power-loss ordering, waveform
continuity, automatic production detection or third-party-app behavior.

Both temporary roots and all marker/storage directories are removed, and the
completed task is unregistered. Source/archive/manifest, exact live fixture and
consumer hashes independently match. Final installed checks preserve R351/R352,
engine PID 8344, saved mix bytes, original microphone UID, six defaults, normal
timing flags and R349 GPU CSS. No candidate app/helper/broker/driver is deployed.
Evidence: runtime-override-r366 native/portable build records, r367 failure and
cleanup records, and runtime-override-r368 live/build/verification/post/cleanup,
portable sanitizer logs, exact consumers and summary. Automatic routing control,
authenticated aggregate sink demand and full live typed provisioning integration
remain open; the public sink provisioning guard is retained.

R369 adds durable Prepared, Applied, UserOverride and Releasing phases to the
per-user routing journal. Interrupted application can resume from its original
baseline; interrupted restoration finishes before reacquisition. Restoration
advances the receipt revision before OS setters, rejecting late confirmations
and acknowledgments. Schema-1 journals migrate transactionally with existing
receipts conservatively treated as Applied. Existing deferred override tests
still preserve the user's choice across restart. New tests cover partial apply,
partial restoration, stale acknowledgments and legacy migration. Portable checks
pass (2 tests, 0.91 seconds), ASan/UBSan checks pass (2 tests, 0.94 seconds), and
the Dell candidate builds all affected consumers and passes 15 focused native
checks in 19.33 seconds. Exact source and executable hashes are verified and
retained under routing-phase-r369. This candidate is not deployed; automatic
controller integration and installed acceptance remain incomplete.

R370–R377 connect outstanding application-output recovery to the serialized MTA
owner loop. The loop opens only an existing, authenticated user journal, retries
at a three-second cadence, and retains receipts on missing/unauthoritative desired
state, identity failures, preference errors or incomplete session observation.
Recovery does not acquire new routes. Confirmed external choices restore other
owned roles and save matching muteOriginal=false bindings before acknowledgment;
deselected routes restore original preferences without rewriting desired state.
All authenticated processes sharing a receipt identity are examined. A live
owned inactive stream still blocks acknowledgment. Inaccessible live clients and
ambiguous shared sessions remain conservative blockers. Proven-dead inactive
clients, classified inactive system sounds and other authenticated applications
do not own this application's receipt. The controller authenticates its own
process too, so its capture sessions are classified rather than left unknown.
Every observed endpoint occupant remains in the recovery result, independently
of per-app receipt completion; an empty journal does not authorize sink removal.
The coordinator still submits Unknown sink demand and its public provisioning
guard remains in place. Full typed sink lifetime/acquisition integration remains
open.

R370 first builds all affected consumers and passes 15 native checks, but its
live fixture stops on a shared-process session. R371 retains the SDK build failure
for the missing symbolic success constant; R372 uses Microsoft's documented
success code locally. R372–R374 live checks retain ownership while inactive
records persist. R374 diagnostics identify records that are not members of the
current authenticated app. R375 separates per-app receipts from global endpoint
occupancy (15 native checks pass; no live run). R376 adds explicit system-sound
classification and conservative process-liveness checks; its live evidence exposes
the excluded observer process. R377 authenticates that process and passes all
15 focused native checks in 19.39 seconds. Portable journal/lease checks and
ASan/UBSan checks each pass two tests in 0.60 seconds; native formatting passes.
Microsoft documents the distinctions in
[GetProcessId](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid)
and [AudioSessionState](https://learn.microsoft.com/en-us/windows/win32/api/audiosessiontypes/ne-audiosessiontypes-audiosessionstate).

The R377 normal-desktop-token fixture passes a real failed document save,
automatic override detection and persistence, an inactive-live-stream receipt
hold, owned-app close/relaunch, runtime/store reopen in existing-only mode, final
receipt release and automatic deselection restoration. Matching disabled aliases
are cleared while unrelated controls remain unchanged. The fixture uses two
unique normal virtual Feeds and copied owned processes that submit only silence.
It invokes the same native recovery pass used by the control loop, with isolated
storage; it does not deploy or test a new installed owner loop, use the internal
typed sink, measure waveform continuity, prove general shared-session mutation,
complete new-route acquisition, or prove preferred-output activation on a new
process. These remain distinct acceptance gates.
All temporary roots, marker/storage directories and scheduled tasks are removed.
Final checks preserve installed R351/R352, engine PID 8344, original microphone,
six defaults, saved mix bytes, normal timing flags and the R349 GPU CSS. No
candidate consumer, broker, helper or driver is deployed. Source snapshots,
manifest hashes, exact executables, failed and passing reports, and final checks
are retained in routing-recovery-r370 through r377.

R378 extends sink demand from desired/recovery state to complete endpoint-session
and Windows-default-role evidence. Positive receipt, desired-app, open-client or
default-role evidence retains the sink even if another observation is incomplete.
Release requires an authoritative document, an examined existing journal, a
hardware-identified sink, a complete session inventory and all six default roles.
Missing recovery storage or endpoint identity stays Unknown. The Windows adapter
uses the account's exact sink key and verified PnP/topology ancestry, rejects
ambiguous identities, preserves positive evidence across independent read errors,
and reuses the conservative session-liveness check from R377. Inactive live or
ambiguous shared clients retain the root; proven-dead inactive clients and
classified inactive system sounds do not.

The candidate's MTA owner loop now computes fresh sink demand after recovery and
submits it through the existing serialized broker coordinator when demand or
document generation changes. Ordinary edits/retries submit Unknown, superseding
cached release work until a new observation is complete. Broker sink status is
projected separately from normal mix status. The public sink provisioning guard
remains retained, so this is not a claim of completed public provisioning or new
application-route acquisition. No candidate is installed.

Portable planner checks pass (one test, 0.55 seconds) and ASan/UBSan checks pass
(one test, 0.65 seconds). All affected Windows consumers build and 16 focused
native checks pass in 19.54 seconds, including a new liveness test with a real
live process, a retained handle to an exited child, system/shared session cases
and missing sink identity. The administrative owned-sink fixture uses the
existing signed development driver 0.4.0.20/oem50.inf, refuses a pre-existing
account sink, identifies a fresh idle render-only sink, and verifies that both
an initialized inactive live client and a started silent client retain lifetime.
It removes only that newly created root and verifies defaults and prior LoopZ
endpoint identities. This tests the adapter against real hardware-backed
endpoints; it does not exercise public broker removal or change a default role.

The separate normal-desktop-token R377 recovery fixture passes again with the
R378 consumers: failed-save retention, deferred override persistence, inactive
stream ownership, app/runtime/store restart and deselection restoration. All
three temporary roots, fixture directories and the scheduled task are removed.
Final checks preserve installed R351/R352, engine PID 8344, original microphone,
saved mix bytes, six defaults, normal audio timing flags and R349 GPU CSS. Exact
source snapshots, consumer hashes, native and live reports,
cleanup and final installed checks are retained in sink-lifetime-r378. Installed
owner-loop behavior, public typed provisioning, acquisition/multiple-process
capture, waveform/stress and clean installer/platform acceptance remain open.

R379–R383 implement grouped application capture. Authenticated members retain
PID and creation-time identity, and process ancestry selects disjoint capture
roots so a selected parent and child are not mixed twice. Independent instances
share the same application group. Raw executable and canonical application
bindings share one native provider while retaining independent audio readers.
The fixed-capacity audio-history mixer performs no callback allocation and
combines coverage without treating valid silence as missing audio.

All 18 focused Windows checks pass with R383 (19.71 seconds), including a real
parent/child/independent-process ancestry check. Portable focused checks and
ASan/UBSan checks pass. R379 exposed an MSVC generic-lambda `requires` mismatch
in the creation-time refresh check; type-level detection fixes it without
removing the PID-reuse assertion. R381 exposed producer-start sequencing in the
waveform fixture, and R382 exposed simultaneous ownership of native process
capture clients; readiness handshakes and serialized probe-to-Runtime handoff
make these conditions explicit.

The normal-token R383 virtual-only fixture passes: three selected processes
(parent 440 Hz, descendant 660 Hz, independent 880 Hz) produce amplitudes
0.03030, 0.03032 and 0.03031 at unity gain, with zero missing frames over 6144
sampled frames. An unrelated executable's 1100 Hz tone measures 0.00043, below
the 0.005 rejection threshold. Runtime prepares both canonical and raw-path
aliases with two disjoint roots. This proves direct grouped capture and Runtime
provider preparation; it does not yet prove Runtime-to-public-microphone audio,
dynamic membership continuity, new routing acquisition or general application
compatibility. Exact R383 sources, binaries, reports, cleanup and final installed
checks are retained in group-capture-r383. Installed R351/R352, saved mix bytes,
six default roles, original microphone and GPU CSS remain unchanged.

R384 extends this proof through candidate Runtime mixing and the public virtual
microphone. The installed coordinator provisions three fresh ordinary mixes in
one document edit, with the third disabled so only the candidate Runtime owns
its producer. Two explicit owned Feeds carry the selected and excluded markers;
no hardware endpoint or default-role fallback is used. Runtime wires the
canonical binding at unity and prepares an unwired raw-path alias against the
same provider. A separate explicit public-microphone client records 24,000
frames after 9,600 warm-up frames at float32 stereo 48 kHz. Both channels measure
0.03000001, 0.03000015 and 0.03000001 for the three selected tones; the excluded
1100 Hz tone is approximately 5.27e-10. The recording has zero silent frames,
discontinuities and timestamp errors. This is a half-second waveform proof,
not long-run continuity, arbitrary third-party application coverage or a new
latency measurement. All three temporary roots and source processes are removed,
and original document bytes and defaults are restored. Fixture-only changes
build from the R383 production sources with a fully checked R384 source manifest;
exact binaries and reports are retained in group-capture-r384. Dynamic group
membership, public routing-sink provisioning, automatic route acquisition,
full-format/stress and clean installer/platform acceptance remain open.

R385 verifies Runtime group refresh after independent-instance start and exit.
A fourth selected marker (1320 Hz) starts after initial public-microphone proof;
ordinary Runtime inventory polling discovers three disjoint roots and both
canonical/raw aliases become ready. The next 24,000-frame public stereo
recording includes all four selected tones at approximately 0.03 and excludes
the unrelated 1100 Hz tone. The original independent 880 Hz process then exits;
Runtime returns to two roots, and another 24,000-frame recording retains 440,
660 and 1320 Hz at unity while 880 Hz falls to approximately 6.77e-10. Both
post-refresh recordings have zero silent frames, discontinuities and timestamp
errors. These are recordings after readiness, not measurements of uninterrupted
transition audio. Parent exit with surviving descendants, rapid membership
churn, and continuous transition/stress coverage remain unverified. Exact
fixture sources, production binaries and reports are retained in
group-capture-r385; the production sources/binaries remain those of R383.

R386–R387 add a serialized Runtime acquisition pass alongside outstanding-lease
recovery. It resolves an exact account routing sink, requires complete session
inventory, authenticated current membership, prepared capture and a connected
public-microphone publication, and reads every selected instance before the
first preference setter. Different originals cannot be merged into one new
application receipt. Existing sink preferences without a receipt are refused.
Receipt-backed setters apply all three roles to every selected instance, while
actual active-session endpoints determine success separately from preference
getters. Unmoved streams time out after ten seconds; restoration retains the
receipt, with a thirty-second retry backoff and the saved graph retained.
External choices remain deferred until saved acknowledgment. The owner loop
updates pending receipt counts after acquisition. Public typed sink provisioning
and the ordinary edit guard remain gated, and a missing recovery store is still
not initialized by recovery. No candidate is deployed.

R386's build stopped on MSVC's shadow-variable warning in the fixture; live
acceptance was never dispatched. The corrected R387 builds all affected
consumers and passes all 18 focused checks in 19.47 seconds. Its normal-token
fixture first verifies the selected waveform through the public microphone,
then stops/resets each marker's nonzero render stream and waits for explicit
silent-stream acknowledgments before touching output policy. All three selected
processes' roles become the internal target. Their explicit-Feed streams do not
all move, so Runtime times out and restores every original role. Receipt
acknowledgment correctly remains blocked by an inactive live administrative
fixture session that could not be authenticated into the desktop session.
The test remains failed, with one durable receipt and the silenced fixture image
preserved for recovery. The administrative holder subsequently exits and removes
only its newly created sink; installed app, original microphone, saved mix bytes
and default roles remain verified. Exact R386/R387 source snapshots and R387
binaries, failure, retained journal and cleanup reports are preserved. This is
failure-path evidence, not proof of successful automatic application movement.

R388 removes the holder's render-session contamination and passes the complete
normal-token controller fixture. Before the new run it reopens R387's preserved
private journal, authenticates a relaunched silent marker at the same image,
verifies restoration to the recorded originals, acknowledges the receipt and
removes that retained fixture directory. The new run repeats direct capture and
24,000-frame public stereo waveform proof, with selected amplitudes approximately
0.03000000, 0.03000006 and 0.03000002, excluded 1100 Hz approximately 4.81e-10,
and no recorded silent frames, discontinuities or timestamp errors. Every marker
then stops/resets its nonzero stream and explicitly acknowledges silent restart.

Runtime acquires all three roles for all selected processes with one durable
application receipt, independently observes that explicit-endpoint streams did
not all move, and restores originals after its ten-second timeout. Deselecting
original-output routing saves the desired change; after old clients close and
a same-image silent marker relaunches, ordinary recovery acknowledges the
receipt. Both old and new fixture journals are cleared, all temporary ordinary
mixes and the separately held internal sink are removed, and saved state,
default roles, microphone identity, installed R351/R352 and GPU CSS remain
verified. All 18 native checks pass in 19.30 seconds. Exact R388 sources,
executables, normal-token report, sink cleanup and final installed-state proof
are retained in group-capture-r388. This proves the automatic attempt/rollback
path, not successful movement of explicit-endpoint or arbitrary third-party
clients, nor audio continuity during a successful internal-sink handoff. Initial
store creation, public broker sink provisioning and ordinary mute-original edits
remain gated pending the full acquisition path's positive end-to-end proof.

R389 supplies positive native acquisition proof with two independent instances
of a fresh owned executable. Both start through automatic render activation and
submit only silent buffers. Runtime first prepares grouped capture and a connected
public microphone, then acquires the durable per-app route. Complete active
session observations verify that both instances actually move from their initial
outputs to the exact account internal sink, with all three preference roles also
verified. A preference getter alone is not used as movement proof.

Only after that observation does each marker stop/reset its automatic silent
client and open an explicit hardware-identified virtual sink client for nonzero
tones. A 24,000-frame public stereo recording measures 440 Hz at 0.03000001 and
660 Hz at 0.03000014 on each channel; absent 880 Hz and another executable's
1100 Hz tones remain below 6e-10. There are zero recorded silent frames,
discontinuities and timestamp errors. This separates safe silent automatic
movement from subsequent virtual-only waveform proof; it does not claim a
continuous nonzero waveform across the automatic client handoff. Each marker
then clears its nonzero buffers and acknowledges silent restart before desired
routing is deselected and recovery restores the original preferences. After
client exit/relaunch, the receipt is acknowledged. All temporary roots, source
processes, directories and the desktop task are removed; saved document bytes,
default roles, installed R351/R352, original microphone and GPU CSS are verified.

All 18 native checks pass in 19.21 seconds. The fixture adds a controlled,
always-silent automatic-client helper; production Runtime remains the R388
controller source. Exact source, live reports, native check executables and
LastTest log with independent hashes are retained in group-capture-r389. This
proves native successful acquisition, subsequent grouped waveform delivery and
restoration on the Win10 Dell. Public broker provisioning, safe initial-store
creation, ordinary interface edits, installed controller integration and broader
application/platform/stress acceptance still remain open; no candidate is deployed.

R390 connects safe first-use journal preparation to the Runtime owner loop.
A saved authoritative request may initialize private history only when its whole
storage directory is new and the exact account PnP root is proven absent.
The root check includes inactive/phantom devices, so an unmapped endpoint does
not imply first use. Existing databases always open without initialization;
passive recovery never creates storage. A pre-existing directory with a missing
database, a present internal device, or an incomplete root observation refuses
initialization. While history or complete recovery is unavailable, Runtime
submits Unknown sink demand: ordinary mix setup continues, but it cannot request
a new routing sink or removal from that incomplete state. Existing coordinator
Unknown semantics preserve sink intent rather than deriving lifetime from mixes.

A new isolated native startup check exercises explicit first use, missing-history
and present-root refusal, failed observation, passive recovery and receipt-preserving
reopen; its root-probe injection does not create real devices. All 19 focused
Windows checks pass in 19.80 seconds. The separate normal-desktop-token live test
uses the real current-account PnP observer with the held endpoint present and
verifies that a fresh history directory is not created. It then repeats positive
routing of two automatic silent app instances, subsequent explicit virtual-only
public-microphone waveform proof and original-role/receipt restoration. Both
channels measure 440 Hz at 0.03000000 and 660 Hz at 0.03000011; absent 880 Hz and
unrelated 1100 Hz remain below 5e-10. The 24,000-frame recording has zero silent
frames, discontinuities and timestamp errors. Source/executable manifests,
focused test binaries/logs, native/live results, cleanup and installed-state
verification are retained in group-capture-r390. Temporary devices and tasks
are removed and installed R351/R352, saved settings/defaults, original microphone
and GPU CSS remain verified. Public broker provisioning and ordinary mute-original
edits remain gated; this is not installed end-to-end setup acceptance or a full
Windows release claim.

R391 enables the protected service store's validated typed AppRoutingSink
requests. The portable codec still fixes sink format/identity and forbids public
mix fields; peer tokens supply ownership, shared revisions enforce compare-and-set,
and operation replay remains idempotent. Administrative store/reconciliation
fixtures now use the real acceptance path instead of seeding future state directly
into SQLite. They cover invalid sink channels/mix identity, forged owner fields,
shared revision ordering with normal mixes, stale deletion and superseded work.
All 22 focused native checks pass in 24.35 seconds, including pipe identity and
uninstalled-binary rejection. All three isolated administrative fixtures pass
and leave no new ProgramData fixture directories.

The verified R391 service host and embedded-payload maintenance helper are now
installed on the Dell. Previous host/helper and a service-stopped database copy
are retained for rollback. Maintenance stages the exact existing signed
0.4.0.20/oem50.inf driver package: migratedDevices=0, no reboot, unchanged SYS,
INF and catalog hashes. Installed host SHA256 is
A136194271D2003CAB9360F0582DB3AAB640917210A967D4B1A999E26ADD74E3;
installed/staged helper SHA256 is
813FEA0A8E3F4B56D134F8C81E26DAD777C43B04584F594EEBC94614EB244183.
The desktop/control binaries remain R351/R352 and the R349 GPU CSS remains.

A new explicitly opted-in normal-token fixture calls the actual installed service,
refuses a pre-existing account sink, provisions one internal render-only endpoint,
verifies its hardware identity and lack of capture endpoint/default role/client,
checks idempotent replay and rejection of stale/forged-owner requests, then removes
only its own sink. Ordinary mix intents and all prior LoopZ endpoint identities
and six default roles are preserved. The first desktop attempt stopped before
native execution because its script tried to hash the intentionally protected
service executable; a separate normal-token diagnostic confirms access denial
and no root/request was created. The corrected fresh attempt keeps that hash
check in administrator preflight and retains the native client's installed-SCM
server verification. It passes actual normal-user provisioning and removal
(accepted revision 611), with unchanged saved document bytes. Failed-preflight,
diagnostic and successful tasks are removed; no file permissions are relaxed.
Exact R391 sources, all checked binaries, admin/live reports, rollback material,
service installation and final installed-state checks are retained in
broker-sink-r391. This proves installed service provisioning by a normal user;
ordinary app mute-original controls, installed controller/first-use integration,
full-format/stress, wider client/platform and installer/release gates remain open.

## Measured workbench latency

Latest scoped result: **R329**, the installed R327 app (deployed R328) and
loaded development driver 0.4.0.20, measures **68.4558 ms median**,
**66.7595–78.36075 ms** across 20 markers. Both publication waiting and streaming
chunks are **OFF**, matching normal installed options. All markers arrive with
zero capture position gaps, discontinuities or timestamp errors. Raw QPC values
independently recompute the reported range and median.

The path is application submission → LoopZ Feed → half-gain engine mix → public
microphone application receipt, at 48 kHz stereo, exclusive PCM16 public clients,
480-frame (10 ms) input/output buffers and private float32 v3 transport. It includes
client buffers and receiver scheduling; it excludes selected-app capture,
physical hardware, acoustic and network paths. This is a scoped measurement,
not a worst-case latency bound or full format/stress acceptance. Saved document,
existing microphone UID and all six defaults are preserved; temporary mix, root
and task are removed. Evidence: latency-r329 report, verification,
post-verification and summary in artifacts/windows.

Earlier diagnostic result: R283, driver 0.4.0.17 and diagnostic app R278 with both
publication waiting and streaming chunks ON, measures **68.354825 ms median**,
**67.0997–78.11815 ms** observed marker-midpoint range across 20 trials. All
markers arrive with zero position gaps, discontinuities or timestamp errors.
This uses 48 kHz stereo exclusive PCM16 public clients with 480-frame (10 ms)
client buffers and private float32 v3 transport. It measures app submission to
LoopZ Feed → half-gain mix → public microphone app receipt; it excludes selected-app
capture, physical hardware, acoustic and network paths. It is not a worst-case
latency bound or a latency measurement of the restored normal app/options.
Raw QPC timestamps independently recompute the reported median/range. Original
app R246, both options OFF, saved mix, microphone and six defaults are restored
and verified afterward. Evidence: latency-r283 report/verification/post-verification
and long-and-latency-r283-summary.json in artifacts/windows.

Earlier PCM16-driver measurements:

R174/R175 measure application submission to LoopZ Feed, through a half-gain mix,
to receipt by an application recording the public microphone. At 48 kHz stereo
exclusive PCM16 with 480-frame (10 ms) playback and capture client buffers, two
independent 20-marker runs have medians 77.0 ms and 68.4 ms. The total observed
range is 66.6–78.3 ms. All 40 markers arrive without capture-position gaps,
discontinuities or timestamp errors; the final recorded runtime snapshots, after all marker receipts, show zero
publication misses and driver underruns in both runs. Cleanup restores the saved mix and defaults.

The probe brackets each ReleaseBuffer call with QPC timestamps and timestamps
microphone packet receipt. Markers are 8 ms, 1 kHz bursts with 1 ms edges,
separated by varying 500/517/534 ms intervals; detection uses an absolute 0.005
threshold after 5 ms quiet. Results include marker onset, both client buffers,
engine/driver buffering and receiver scheduling. They exclude selected-app
capture, physical hardware, acoustic and network paths. They are observations
from these runs, not a worst-case latency bound or high-rate continuity proof.
These earlier measurements used PCM16 transport/0.4.0.15. The R283 measurement
above is separate evidence for the current driver and specified diagnostic settings.
The first report used the upper central order statistic for its median; its raw
timestamps independently recompute to 77.047375 ms with the conventional median.
The final probe uses the average of the central pair (R175: 68.36845 ms).

Reproduce with the isolated acceptance harness, after starting an eligible engine:

```bat
set LOOPZ_FORMAT_PROBE=--latency-roundtrip-exclusive
set LOOPZ_FORMAT_RECORD_MS=15000
python tests\windows_format_acceptance.py APPDIR FRESH_REPORT single 48000 2
```

This opt-in administrative harness creates/removes its own test adapter and
preserves existing mixes/defaults. It never sends markers to physical speakers.
Evidence: `artifacts/windows/latency-r174-verification.json`,
`artifacts/windows/latency-r175-verification.json` and their recording reports.

## Build

Install Visual Studio 2022 Build Tools with Desktop development with C++, the
Windows SDK (10.0.26100 or later), CMake 3.24+, Python 3 and Node.js. The newer
SDK supplies the process-capture declarations; it does not change the Windows
10 deployment target. Microsoft Edge WebView2 Runtime is required on end-user
machines. It is separate from the SDK downloaded during configuration.

From a command prompt:

```bat
scripts\build_windows.cmd
```

The script builds, tests and creates `LoopZ-<version>-windows-x64-preview.zip`.
Extract the entire archive and open `LoopZ.exe`. Runtime C++ DLLs are packaged
beside the application. The archive is unsigned and contains no driver or
system-wide installer. There is no automatic installation or update in this
preview.

Alternatively, from a Visual Studio x64 developer shell:

```bat
npm ci --ignore-scripts --prefix interface
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOOPZ_BUILD_MACOS_BACKEND=OFF -DLOOPZ_BUILD_WINDOWS_BACKEND=ON
cmake --build build/windows --parallel 4
ctest --test-dir build/windows --output-on-failure
cpack --config build/windows/CPackConfig.cmake -C Release
```

For offline native builds, set `LOOPZ_WEBVIEW2_SDK` to the extracted pinned
NuGet package and `LOOPZ_PREBUILT_INTERFACE` to an interface build containing
`index.html`, `loopz.js`, and `loopz.css`. Normal builds generate the interface
from the local source and locked npm dependencies.

The WebView2 SDK is pinned to Microsoft.Web.WebView2 1.0.2903.40 with SHA256
verification in CMake. JSON for Modern C++ 3.11.3 is vendored with its license
and source hash. The SingZ snapshot remains unchanged.

## Runtime behavior

The engine stores `routing.json` and its previous version in
`%LOCALAPPDATA%\LoopZ`. Saves flush a temporary file and atomically replace the
primary with a backup. A corrupt primary can recover from a valid backup;
if both are unreadable, they are preserved and engine startup fails.

Closing the window leaves its per-user engine running. Opening the app again
reconnects to it. **Settings → Stop routing and quit** explicitly stops routing. The diagnostic
command `loopz-windows-control.exe --stop-engine` does the same without a window. Before replacing the preview files, close its windows and stop the
engine with that command. Settings can register the per-user engine to start at login. A per-user supervisor restarts a failed engine with bounded backoff, but honors
explicit shutdown. Ten consecutive short failures stop the restart loop.

The named pipe permits the current user and SYSTEM and rejects remote
connections. The window only loads bundled interface assets; remote navigation,
new windows and browser microphone permissions are disabled. Audio is captured
by the native engine. Requests are bounded and correlated; edits carry an
engine epoch and document generation to reject stale clients.

The engine uses each mix's configured rate. WASAPI shared mode negotiates physical endpoint formats;
the common timestamped histories convert between independent clocks/rates.
The monitor queue covers the actual negotiated endpoint buffer and reported
stream latency before adding filter/scheduling headroom; the advertised period alone is not sufficient.
The preview uses a conservative source-history delay and monitor queue. Buffer
duration is not a measured end-to-end latency claim.

Device and application recovery continues while the window is closed. App
selectors use executable paths, with capture including the selected process's
children. Multiple independent instances of the same executable still need
explicit policy and acceptance coverage.

## Validation

```bat
loopz-windows-probe.exe --devices
loopz-windows-probe.exe --capture 3
loopz-windows-probe.exe --silence 3
loopz-windows-probe.exe --process <pid> 3
loopz-windows-control.exe --self-test <fresh-test-directory>
loopz-windows-control.exe --state
```

The capture probe reports counters/levels without saving microphone audio.
`--silence` exercises the output callback and routing core without audible
output. `--tone` and `--duplex` explicitly produce audio; use headphones for
microphone monitoring. `loopz-windows-process-isolation.exe` starts two quiet
marker processes, captures only one and checks that the other's frequency is
excluded. Audio tests must run in an interactive Windows desktop session;
running through a service/SSH session alone does not establish endpoint access.

## First-party Windows microphones

LoopZ uses its own WaveRT driver. Capture and mixing remain in userspace; each
mix has an independent root adapter and FIFO. Its stable device identity is a
SHA-256 key of the owning Windows account SID and the durable mix ID. Renaming
or reordering a mix does not change that identity; another Windows account does
not accidentally publish into the same adapter.

Create mixes, then use **Settings → Install or repair microphones**. The native
setup helper requests Windows administrator approval, stages the supplied driver
using Windows installation APIs, and reconciles the exact device instances. It
does not enable test signing, import certificates, change Secure Boot or reboot.
New device instances are removed if setup fails. Existing devices are retained
for repair. Windows must accept the signed package before normal installation.

Each enabled stereo mix publishes through an owner-authorized private KS transport
to its matching microphone, even without hardware monitors. The public playback
endpoint is ingress only; it has no direct path to recording. A Pass-through
source reads that ingress into the saved mix before microphone publication. Saved monitors remain independent output consumers.
Internal Feed/Microphone endpoints are excluded from physical-device choosers;
use virtual mix sources to connect mixes. All owned microphones are rejected as
hardware sources to prevent feedback. Missing microphone publication is reported
separately from physical monitoring and does not count as restored audio.

Deleted mixes stop publication; their devices are retained rather than silently
reassigned to another mix while a recorder is open. Setup's **--uninstall** removes
all LoopZ adapters and the matching package, including devices from other accounts.
This is an explicit machine-wide uninstall, not routine mix deletion. Endpoint
names and identity through an actual driver update still need acceptance coverage.

Public microphones currently support stereo at 48 kHz. Unsupported edits are
rejected before saving. Processed playback uses driver protocol v1 (driver INF
version 0.3.0.1), separate incoming and processed queues, an exclusive engine
generation, and a 100 ms lease. Expired or disconnected engines produce silence.
Private calls run on a dedicated COM worker, outside graph and WASAPI callbacks.
Ingress timestamps follow the exact frame position from each playback run
anchor, rather than copying DPC receipt jitter into the resampler clock.
The Windows graph reserves 1024 input frames for pass-through transport and
resampler lookahead. Muting an application's original playback remains unavailable.
Older 0.2 development archives use the previous direct cable and cannot provide
this processed playback path.

## Signed distribution

`scripts/package_windows_release.ps1` builds a combined distribution from an app
folder and an already Microsoft-signed driver package. It verifies application
signatures, the Microsoft catalog signer, kernel signing policy and INF/SYS
catalog membership, and writes a SHA-256 payload manifest. Development certificates
are rejected. `packaging/windows/LoopZ.iss` is an Inno Setup 6.3+ installer source
for that verified payload, targeting real x64 Windows 10 22H2 and Windows 11.
It installs under Program Files, supports reinstall/repair and calls the native
helper on uninstall. A failed driver removal keeps app files available for retry.
`scripts/build_windows_installer.ps1` verifies the payload, compiles it and signs
both Setup and Uninstall through Inno's signing hook, then verifies the installer.
It requires the signing tool, Inno compiler, current-user certificate thumbprint
and HTTPS timestamp service as explicit inputs. End-to-end signed installer acceptance is still pending.
Microsoft Hardware Dashboard signing and application-signing credentials are
external release prerequisites; neither is generated or bypassed by these scripts.

## Experimental virtual cable driver

The separate [driver source](../native/platform/windows/driver/README.md) builds
an unsigned WaveRT cable with LoopZ Feed playback and LoopZ Microphone recording
endpoints. Its INF targets Windows 10 and KMDF 1.31. The pinned VS2022/WDK build
and INF verification pass on Windows 10. The shared FIFO has portable lifecycle
and randomized wrap/overflow and exact-frame clock tests.
Development-signed loading has been checked on physical Windows 10: stereo
markers reach WASAPI recording clients, producer stop yields silence, and restart
works while recording stays open. The full app-capture → shared mix → cable →
recording path passes, including rejection of a direct microphone feedback route
and zero missing source/monitor frames in the short checked interval. This does
not establish long-run continuity or measured end-to-end latency. Those early
direct-cable results describe the 0.2 driver. Desktop-only ZIPs exclude the driver;
the separate development bundles include a workbench-signed package. The machine
owner explicitly chose to retain test-driver mode on the dedicated workbench.

Build it separately with `scripts\build_windows_driver.cmd`.
`scripts/sign_windows_driver_development.ps1` can sign a new development package
using an existing certificate thumbprint and explicitly selected certificate store.
It does not configure the workbench, import trust, alter boot settings or install
devices. Workbench test-driver mode is an explicit machine-owner choice and is
separate from normal-user installation. The build script produces an unsigned
binary; the development signing script signs it with the selected existing
certificate. Neither changes boot security, installs devices or reboots. Loaded testing
requires a trusted signed driver or an explicitly approved test environment.

## Earlier 0.2 multi-microphone development acceptance

The earlier direct-cable development driver passed a Windows 10 22H2 check with two
independent adapters. A 440 Hz application marker reached its intended microphone
at approximately 0.03 amplitude while the other mix's microphone remained silent.
Hardware monitoring ran simultaneously; muting it did not mute publication. Direct
owned-microphone feedback was rejected. The steady checked interval had zero
missing source and monitor frames. Renaming the other mix retained the live output
callback counter and endpoint identity. This does not establish long-duration
waveform continuity, stable identity across an actual driver upgrade, or Windows 11.

Native setup removal and a repeated removal both returned success; the typed
endpoint probe reported no remaining LoopZ endpoints afterward. Production-signed
installation and upgrade acceptance still require Microsoft-signed artifacts.

The per-user supervisor also passed forced-child-crash recovery and explicit-stop
acceptance. The test checks a fresh engine epoch after recovery and verifies that
an explicit stop exits the supervisor without restarting audio.

## Processed pass-through development acceptance

Driver 0.3 uses separate ingress and processed recording queues and a private KS
transport. Physical Windows 10 checks passed independent channel markers, 0.5
saved-mix gain, playback stop and restart. The refined 0.3.0.1 clock anchors
presentation timestamps to frame positions; its checked recording peak was
approximately 0.025 for a 0.05 input at half gain. An explicit transport-disconnect
check on 0.3.0.0 produced silence even while playback continued.

The current setup helper installs each device independently and skips a healthy
adapter already bound to the staged package. Creating and deleting the pass-through
test mix preserved the existing microphone ID. The previous helper's global
hardware-ID update rebound unrelated adapters. A real workbench upgrade from
0.3.0.0 to 0.3.0.1 installed the expected binary without reboot, but changed the
ordinary Windows endpoint ID. External-app selection preservation across upgrades
is therefore still an open gate. These checks do not establish long recordings,
crash-time waveform continuity, production signing, or Windows 11 behavior.

## Upgrade and original-playback investigation

`tests/windows_upgrade_acceptance.py` records published mix identities, actual
installed driver versions, and all six Windows default-device roles before and
after a separately authorized upgrade. A version-only 0.3.0.1 → 0.3.0.2 update
through per-device setup preserved the mix identity and every default role, but
still changed the ordinary microphone endpoint ID. The endpoint-ID gate remains
open; the earlier global update call was not the only cause.

The opt-in `loopz-windows-process-isolation --mute-probe` mutes only its own marker
session and restores it before process cleanup. Fresh, isolated 440 Hz capture
measured approximately 0.03 before mute, zero during mute, and 0.03 after restore
on Windows 10. A simple `ISimpleAudioVolume::SetMute` implementation therefore
cannot provide original-playback muting while retaining the captured source on
this baseline. The production control still rejects that setting.

`app_audio_policy.cpp` is an experimental, thread-confined adapter for two known
private WinRT interface revisions. The ABI/encoding is attributed to the pinned
MIT-licensed EarTrumpet 2.3.0.0 source. `--policy-probe` activates the interface
and reads only an owned active marker's process preferences; it never sets them.
On Windows 10, all three role reads succeeded for that marker. Querying the probe's
non-audio process instead returned `E_INVALIDARG`, so activation alone must not be
treated as proof that arbitrary process preferences can be read or restored. This adapter is
not connected to production routing.

`--route-probe RENDER_UID` uses a uniquely copied marker executable, snapshots all
three opaque preferences and restores them before removing the marker. On Windows
10, setting and restoring those preferences succeeded, but neither the existing
stream nor a newly started marker process moved from its explicitly opened
hardware endpoint to the requested LoopZ Feed. Capture stayed valid throughout.
Preference readback is therefore not evidence of effective playback suppression.
This is a measured limitation of this WASAPI marker path, not proof of every
Windows app's behavior. Effective suppression, user overrides, process-exit/crash
recovery and Windows 11 remain unverified; the feature stays disabled.

The current shared core, public headers and native tests were rebuilt on Windows
10 after the latest audio-history changes: all 40 native tests passed. The opt-in
`windows_cable_acceptance.py` now runs alongside saved mixes, verifies their
microphone IDs and saved document remain unchanged, and removes only its own two
temporary mixes and root adapters. Its identity check covers ordinary mix edits
and provisioning, not driver-version upgrades.

## Variable-format work in progress

The source now has a version-2 private transport and per-adapter format records
for all six shared-core rates (44.1, 48, 88.2, 96, 176.4 and 192 kHz) and every
even channel count from 2 to 64. Recording queues allocate storage for the actual
format, clocks preserve rational frame carry at that rate, and each miniport owns
its format/mode/range descriptors. The engine chooses a render rate at least as
high as the saved mixes and publishes all their channels. Format changes are
recorded atomically by the privileged setup helper and restart only the affected
adapter; an interrupted change remains pending for the next repair.

The 0.4.0.0 driver passed the Windows compiler and Universal API validator, and
its engine passed all 40 native Windows tests. Shared queue/clock tests also
passed AddressSanitizer/UndefinedBehaviorSanitizer across all 192 combinations.
The first 0.4.0.0 run crashed: matching private driver symbols identify
`DuplexBridge::takePlayback` calling `__chkstk` for a 61,464-byte aggregate
temporary. The 0.4.0.1 fix clears the caller-owned packet in place instead.
Microsoft's analyzer rejects the preserved original source with C6262 and
accepts the fixed source with a 1 KiB limit. A compile-only analyzer target now
runs with Windows builds. The fixed driver loaded without another reboot and
passed the formerly failing private read and all five public/private stereo
signal, silence and lease-recovery phases. All 40 native Windows tests passed
again. No 0.4 package has been released; the 0.3.0.2 ZIP is the previous tested
build, not a compatible app-only downgrade over a version-2 transport driver.

`windows_format_acceptance.py` and
`loopz-windows-cable-probe --format-roundtrip KEY RATE CHANNELS` check actual
channel spectra, exact public formats, saved-mix preservation and all six
default roles. Stereo and a fresh four-channel adapter pass real half-gain
recording. The first matrix attempt failed when resizing an existing adapter:
its private format changed, but Windows retained the old public channel count.
The setup helper uses the documented `IAudioEndpointFormatControl` and a scoped
private `IPolicyConfig` format setter when Windows retains an obsolete factory
format. Both operate only on endpoints belonging to that mix; the helper never
writes MMDevice format properties or changes global defaults. With driver
0.4.0.2, real half-gain recording passed every even channel count from 2 through
20 at 48 kHz. Resizing to 22 channels fails with `AUDCLNT_E_UNSUPPORTED_FORMAT`
at the policy setter, despite the driver accepting the exclusive format. A
fresh 64-channel adapter also fails there, so this is not proven to be only a
resize problem. Neither the complete channel range nor the six-rate matrix is
accepted yet. The channel/rate parity gate remains open until all formats pass.
Further boundary tests rejected both 22 channels at 44.1 kHz and six channels at
192 kHz. Inspection found a stale 38,400-byte limit in
`AllocateBufferWithNotification`, which constrained all profiles to the old
stereo/48 kHz memory allowance. Driver 0.4.0.3 keeps the same 200 ms duration
bound while scaling bytes to the configured rate and channel count, uses
64-bit duration arithmetic, and fails allocation cleanly if mapping fails.
All 192 format combinations now cover normal notification buffers and the
allocation boundary in portable regression tests (also passing ASan/UBSan).
The strict WDK build, Universal API validation, stack analyzer, and all 40
native Windows tests passed. The signed 0.4.0.3 driver loaded on Dell with its
installed hash verified and no reboot required. The next physical matrix passed
13 profiles (all even channel counts from 2 through 26 at 48 kHz), then stopped
on an endpoint-replacement race at 28 channels. The setup fallback now returns
transient invalidation to its bounded rediscovery loop, including invalidation
during activation, exclusive probing, and policy selection. A subsequent fresh
32-channel mix passed every channel's actual signal test. Fresh 34-channel and
64-channel adapters configure exact nominal endpoints successfully, but shared
duplex stream initialization fails with `E_INVALIDARG`; that remains unresolved.
These 0.4.0.3 attempts retain reports and pass cleanup and saved-mix preservation.
Direct WASAPI diagnostics reproduce the failure on both endpoints, with PCM16
and the returned float mix format, with or without event callbacks and with a
fresh session GUID. The same 64-channel endpoints initialize successfully in
exclusive PCM16 mode. This proves initialization, not a 64-channel recording
or a general Windows channel-limit claim.

The private publication worker now starts on the common 10 ms sample grid.
All six supported rates have whole frames in this interval, avoiding a needless
fractional-delay filter when publishing from the same render clock. In the
32-channel/88.2 kHz comparison, the maximum observed publication render time
fell from 9.11 ms to 2.19 ms while the signal test passed. Per-phase timing
diagnostics are exposed in the runtime snapshot. The subsequent rate sweep
passed 32-channel recording at 44.1, 48, 88.2 and 96 kHz; 176.4 kHz still had
missing audio and render-bus discontinuities despite a healthy private worker.
Wide accumulation specialization and explicit SSE2 candidates did not improve
the wide-channel benchmark and were removed; filter precision is unchanged.

History payload samples now use release stores and acquire loads, retaining
sequentially consistent frame tags and revision checks. Reading a new payload
orders its writer's invalidation before the final reader check, so a partially
overwritten frame cannot validate. A two-reader/one-writer wraparound test covers
2, 32 and 64 channels and checks every accepted channel value. Spectral quality,
callback allocation checks, ASan/UBSan and ThreadSanitizer pass. On Dell, median
256-frame history publication fell from 82.1 to 47.9 microseconds at 32 channels
and from 154.8 to 86.8 microseconds at 64 channels. This is publication cost,
not end-to-end audio latency, and it does not resolve the high-rate wide-mix loss.

`PlaybackClock` reconstructs continuous nanosecond anchors from the software
driver's exact nominal frame count. It retains the first observed timestamp,
accepts only the wire format's 100 ns quantization error, and resets on real
gaps or explicit discontinuities. Randomized tests cover all six rates,
different starting phases, explicit/genuine gaps, resets and invalid inputs.
The pre-convolution build passed actual stereo half-gain recording at all six rates
from 44.1 through 192 kHz, preserving the saved mix, endpoint IDs and defaults.
Its final native Windows suite passes all 40 tests.

Before block convolution, graph timing counters identified the 176.4 kHz/32-channel deficit:
one measured run averaged 1.99 ms per 256-frame block against a 1.45 ms period,
with ten timeline reanchors and 152,320 skipped frames. A CPU-checked AVX2 build
and exact fractional-phase coefficient caching provided insufficient wide-mix
benefit; both experiments were removed, and the default build settings and
original resampler arithmetic were restored. The wide-format gate remains open.
These wall times from the session-0 test engine were later supplemented by
the CPU and source-read measurements below. Signed-in desktop scheduling remains
a separate validation requirement.

Rate transitions also exposed a public-endpoint readiness race. Setup now
requires three matching polls with unchanged feed/microphone IDs before
returning. Runtime connection status additionally requires an enumerated public
microphone with the requested channels and rate. After this correction, the
32-channel sweep again passed 44.1, 48, 88.2 and 96 kHz; at that point, 176.4 kHz
still failed. Cleanup preserved the original mix and defaults. All 40
native tests pass with the readiness correction.

A readable saved document whose format cannot run on Windows now opens with
an audio diagnostic and remains editable. An isolated Windows runtime test
verifies loading preserves those saved bytes and changing the rate to a
supported value recovers routing without replacing the document.
Virtual-driver signal tests can run in session 0; signed-in desktop application
capture and UI acceptance remain separate requirements.

## Owner-scoped microphone reconciliation

Settings → Install or repair microphones uses the helper's `--reconcile`
operation. After all desired adapters start, it removes canonical LoopZ roots
whose protected owner SID matches the requesting account and whose mix is no
longer saved. Other accounts and unclaimed roots are excluded. `--sync` remains
provision-only for compatibility. All setup/removal operations share a mutex
restricted to administrators and SYSTEM; overlapping setup fails without
mutating devices. Reconciliation verifies the saved bytes again and holds a
read lock during removal so an edit cannot change which roots are desired.
Deletion still requires this repair action; automatic provisioning remains open.

`windows_reconcile_acceptance.py APPDIR REPORT` passed on Dell with one owned
orphan and a separately provisioned synthetic foreign owner. It verifies removal
of the orphan, preservation of the foreign adapter, mutex contention without
removal, idempotent repair, and unchanged existing mix documents, microphone IDs
and all six default roles. A subsequent empty-document repair removes the
synthetic owner's last adapter while preserving the real account's microphone.
Both temporary adapters are removed afterward. The final implementation passes
all 40 native Windows tests, including expanded 32-channel spectrum coverage.

The subsequent CPU-instrumented 176.4 kHz / 32-channel run still fails signal
acceptance (`format043-single176400-32-cpu-r23-report.json`). MMCSS registration
succeeds with error zero. The render thread consumes 2.719 seconds of CPU while
measured scene calls take 2.775 seconds of wall time across 1,407 blocks, with
151,808 skipped frames. CPU accounting includes the whole render thread, so it
does not isolate individual DSP operations, but these measurements point toward
compute cost rather than scheduler preemption. Cleanup succeeds and the owned
test engine exits. Raising scheduler priority is not supported as a fix by this
measurement.

Per-scene source timing in the next run
(`format043-single176400-32-source-r24-report.json`) attributes 2.298 seconds of
the graph's 2.803 seconds to 1,388 pass-through input reads (about 82%). The
wrapper forwards meter, sparse-channel and selectable-filter capabilities and
owns independent counters for each prepared scene. These are elapsed-time
diagnostics; together with the preceding CPU measurement they identify the
history/resampling input path as the main optimization target. The 176.4 kHz /
32-channel signal test still fails and its cleanup succeeds.

Further Dell microbenchmarks rejected three changes without retaining algorithm
changes (`resampler-layout-experiments043-r25-r27.json`). Tile sizes 8, 16, 64
and 128 did not improve the 32-channel fractional unity-rate case over 32.
Transposing the double accumulators increased its median from 412.1 to 484.3
microseconds per 64 frames. A separately compiled AVX implementation, selected
on control through Windows' CPU capability API, measured 433.2 microseconds.
Both AVX and AVX2 capability flags were true. The AVX candidate passed a
bit-for-bit comparison against separate scalar double multiply/add operations
for every width 0–64, unaligned buffers, tails, extreme finite floats and 257
taps; resampling correctness tests also passed each candidate. None supplied
the required speedup, so their code and per-file instruction-set flags were
removed. These microbenchmarks are not new recording acceptance. At the end of
those rejected experiments, the generic resampler source was restored to SHA-256
`376e54c7b2c050a94d894ec3b7aa7717216c4901031af57819969b4be767b501`.

## Block convolution and current format evidence

The retained resampler now evaluates the same symmetric sinc kernel using a
control-prepared 512-point FFT plan for exact unity-rate, fractional reads of
128–256 frames and at least 16 channels. Other requests retain the FIR path.
The filter support, normalization and missing-frame rules remain unchanged;
double-precision transforms introduce ordinary rounding differences rather
than changing the filter. Each read owns its scratch, under 160 KiB on the
user thread's stack, without allocation, locks or shared mutable transform data.
Two comparable-magnitude channels share a complex convolution; widely different
magnitudes are processed separately, and silent channels remain exactly silent.

Tests explicitly prove the fast path runs by checking provider-read counts,
then compare it against independent 32-frame FIR requests. They cover 16, 17,
32 and 64 channels, positive/negative offsets, two fractional phases, 128/193/256
frames, planar/strided output, gaps, missing tail channels, nonfinite inputs,
silence and extreme magnitude differences. Availability and missing-frame counts
match exactly; sample differences are bounded by `2e-7` of channel input peak
(with a subnormal floor). Callback allocation checks, ASan/UBSan and
ThreadSanitizer pass. The final Windows build passes all 40 native tests in
11.77 seconds (`loopz-windows-format043-r10-regression.log`).

Across three Dell runs of the 256-frame high-rate benchmark, median 32-channel
unity-rate input conversion falls from 1,652.6 to 614.3 microseconds at 176.4 kHz;
64 channels falls from 2,545.5 to 1,072.3 microseconds. These are input-read costs,
not end-to-end latency. The first Windows attempt did not select the fast path
because MSVC's integer addition rounded fractional bits. Selection now admits
only `1e-12` source frames of that arithmetic noise while still requiring exact
unity step and identical integral positions.

Actual 176.4 kHz/32-channel half-gain recording now passes
(`format043-single176400-32-fft-r31-report.json`): channel target amplitudes are
0.0249766–0.0249840 against 0.025 expected, with maximum measured cross-channel
leakage 0.0000605. A fresh 44.1 kHz/32-channel test also passes (`r34`). However,
both six-rate sweeps (`r33`, `r36`) pass 48 kHz and then fail after changing to
44.1 kHz, with transient peaks and/or target amplitude errors. Graph timing
reports zero reanchors/skipped frames in those failures. Rate-change continuity
therefore remains open; a standalone pass does not accept the transition.

The independent 192 kHz/32-channel run (`r35`) fails shared WASAPI initialization
with `0x8007000e` on both endpoints before recording starts. Exact mix-format
support reports success, as does exclusive PCM16 initialization. This proves
neither recorded 192 kHz/32-channel audio nor a Windows-wide rate/channel limit.
Post-test available memory is about 14 GiB; the HRESULT alone does not establish
general RAM exhaustion. All these tests report successful cleanup, preserving
the existing saved document, microphone IDs and system defaults. Driver 0.4.0.3
and transport version 2 are unchanged. See `format043-fft-verification.json`.

## Independent endpoint recording and guarded allocation (0.4.0.4)

The opt-in cable probe now has `--format-roundtrip-raw KEY RATE CHANNELS`, which
uses independent shared-mode WASAPI render/capture clients and copies capture
packets directly, without the SingZ duplex FIFO. It requires the exact public
float32 mix format and retains the same gain, peak and channel-leakage limits.
The format acceptance script selects it with process-local
`LOOPZ_FORMAT_PROBE=--format-roundtrip-raw`; the default duplex probe is retained.
Both modes report buffer sizes and capture/queue diagnostics. This direct
recorder does not request Windows' separate RAW signal-processing option.

On driver 0.4.0.3, raw sweep `format043-rates32-raw-r37-report.json` passes
48, 44.1, 88.2, 96 and 176.4 kHz at 32 channels, then fails to initialize at
192 kHz. Each completed profile reports zero capture-position gaps and timestamp
errors. A fresh duplex sweep (`r38`) passes the same five rates with no FIFO
underflows/overflows, xruns or deadline misses, but also fails at 192 kHz.
These successful sequences do not erase the earlier intermittent failures.

Expanded diagnostics at 192 kHz/32 channels (`r39`) show both endpoints expose
IAudioClient3 and a fixed legal 1,920-frame shared period. Initializing that
period still returns `0x8007000e`, as do legacy shared float32/PCM16 requests.
The capture endpoint accepts RAW-processing properties but still fails to open;
the feed rejects the RAW-processing option during preparation. Exclusive PCM16
initialization succeeds. The diagnostic output now distinguishes preparation
failures from calls that actually attempted initialization.

A neighboring 192 kHz/30-channel raw recording passes (`r40`), with 4,224-frame
client buffers and a maximum signal peak of 0.0250244. The successful buffer is
506,880 bytes at float32/30 channels. A suspected size boundary is an
investigation lead, not proof of a documented Windows-wide limit.

Driver 0.4.0.4 adds a check that the MDL actually contains every requested byte
before mapping, clearing or advertising its buffer. A short allocation is freed
and rejected. This follows Microsoft's explicit
[AllocatePagesForMdl contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-allocatepagesformdl),
which permits partial allocations. The software endpoint also removes the
unnecessary below-4-GiB allocation restriction and advertises 64-bit addressing.
No filter, PCM format or transport-version change accompanies this update.
Strict WDK compilation and Universal API validation pass with zero warnings;
the 1-KiB kernel-stack check still compiles. Allocation-boundary tests cover
every advertised channel/rate combination and pass ASan/UBSan.

The existing workbench certificate signs the new package. Installation requires
no reboot and preserves the saved document byte hash and all six user default
roles. The completed `LoopZUpgrade044` one-time task is removed. Test-signing
remains enabled. The installed signed SYS hash is
`CF4236E9B6ACB8FC9A578AC717804C01521261EB3E6FE83774CE2E0D41811F1E`.
Windows regenerates the existing mix's feed/microphone IDs during the upgrade,
so stable endpoint identity is still unaccepted. Its new microphone UID is
`{0.0.1.00000000}.{81640d0e-5ad5-41c1-9f90-40ed7b25582d}`.

Post-upgrade 176.4 kHz/32-channel direct recording passes
(`format044-single176400-32-raw-r43-report.json`). The 192 kHz/32 shared opening
failure persists (`r41`), so 64-bit addressing is not a proven fix for that issue.
The raw rate sweep (`r42`) passes 48 kHz but reproduces 44.1 kHz distortion with
no capture-position gaps or timestamp errors. This rules out attributing all
distortion solely to the duplex adapter. Rate-change stability remains open.
All test cleanups preserve the post-upgrade document, current microphone IDs and
system defaults. See `format044-allocation-verification.json` for current scope.

## Cached FIR and coherent packet position (0.4.0.5)

The retained linear-phase reader now caches a source window of at most 512
frames for eligible 128–256-frame reads of at least 16 channels. Each channel's
contiguous samples feed double-precision dot products with independent partial
sums (SSE2 on x64, portable scalar partial sums elsewhere). The coefficients,
normalization, filter support and missing/partial-channel rules are unchanged;
rounding order differs. Oversized windows and other request shapes retain the
original tiled FIR, while eligible unity-rate reads still prefer FFT convolution.
Reader helpers do not inline their large scratch into the caller's stack frame.

Expanded reference tests compare both 44.1/48 kHz directions and 48/96 kHz
directions against independent 32-frame original-FIR requests. Provider counts
prove caching is exercised when the window fits and that oversized windows
fall back. Tests cover widths 16/17/32/64, positive and negative offsets,
integer/fractional phases, gaps, missing tail channels, strides, nonfinite inputs,
extreme magnitudes and no callback allocation. Metadata matches exactly and
sample differences retain the `2e-7` input-peak bound. ASan/UBSan and
ThreadSanitizer pass (`cache044-r45-asan.log`, `cache044-r45-tsan.log`).

Three-run Dell medians for 256-frame 32-channel conversion decrease from
2,120.5 to 1,415.1 microseconds for 44.1→48 kHz and from 2,311.3 to 1,526.6
microseconds for 48→44.1 kHz. This is processing cost, not audio latency.
The direct recording sequence `format044-rates32-cache-r46-report.json` still
fails at 44.1 kHz despite the reduced cost, so this optimization alone is not a
signal-continuity fix.

Additional diagnostics count incoming packets/frames, explicit discontinuities,
timestamp reconstruction resets and completed all-channel-zero runs bounded
by nonzero audio. Probe output also reports all-channel-zero frames and the
maximum consecutive-sample step in the collected recording. In a failing
duplex sequence (`r47`), the maximum recorded step is 0.0222473 while FIFO
underflows/overflows, capture balance and deadline-miss counts remain zero.
Private ingress has no completed zero runs or timestamp resets. A fresh raw
sequence (`r48`) passes the first five rates before the known 192 kHz opening
failure, underscoring that the distortion is intermittent.

Inspection identifies a driver consistency defect: `UpdatePosition` can advance
DMA between timer notifications without updating the packet counter queried by
other stream methods. Capture can select an older packet whose ring storage is
being overwritten, and render commit validation can use a stale counter.
Driver 0.4.0.5 publishes completed packet count with every DMA position update
under the position lock. Capture refreshes position before taking its packet
snapshot. Render commit validation and publication now share one lock, removing
the validation/commit race. The notification callback no longer independently
assigns packet count. Tests cover all six rates, queries between notifications,
64-bit packet counts and invalid buffer geometry; ASan/UBSan passes.

The strict WDK build and Universal API validator pass without warnings, and all
40 native Windows tests pass in 14.26 seconds. The signed workbench upgrade
installs without a reboot and preserves the saved document and user defaults.
Windows again regenerates virtual endpoint IDs, leaving upgrade identity open.
The installed SYS hash is
`F8E4931B877ECC3F8DC1675B7F9CB6FDC36DB69279E70ED34C36149EB0E46811`;
the current original microphone UID is
`{0.0.1.00000000}.{7373b5c2-f292-4caf-96b6-f9f20016517e}`.

The opt-in `transitions CHANNELS` acceptance mode repeats 48→44.1 kHz five
times (ten profiles). It preserves the existing signal limits and does not
replace or shrink the full 192-format gate. Physical results for the synchronized
driver are recorded separately; compilation and the arithmetic tests alone do
not establish that intermittent distortion is fixed.

### Repeated-format results and input-history diagnostics

Driver 0.4.0.5 does not yet pass repeated-format acceptance. Raw WASAPI run R49
passes nine profiles and fails the tenth at 44.1 kHz/32 channels. The instrumented
R51 run passes three and fails the fourth; R52 passes seven and fails the eighth.
All three restore the original document/defaults and remove their temporary mix.
R50 duplex also fails at 44.1 kHz, but is comparison evidence only: it ran the
older diagnostic binary and overlapped a build, so it is not clean acceptance.

Input-history diagnostics are exposed under `microphone.inputHistory`, including
missing reasons and unpublished/expired/racing sample reads. Acceptance reports
retain counters before and after each probe. R52's failing recording has 3,791
unpublished sample reads, no expired/racing reads, no ingress zero runs or timestamp
resets, and a maximum private render duration of 5.932 ms. Passing recordings also
contain unpublished reads; these intervals include playback startup. The counters
do not yet prove that late input explains the steady recording glitch. No signal
thresholds or advertised format gate have been relaxed.

### Drain private input between publication chunks

The raw recording probe now reports QPC ticks for playback start, the recording
boundary and stream end. Acceptance samples engine diagnostics twice while the
probe runs and verifies each snapshot lies wholly inside the recorded interval.
This avoids attributing startup resampling misses to a steady-state glitch.

Unchanged-worker R53 passes all ten transitions; the repeat R54 fails its second
profile at 44.1 kHz/32 channels. R54's two verified recording snapshots show 43
additional frames missing resampling support and 42 unpublished sample reads, with
no expired/racing reads. Its passing 48 kHz profile has no such misses.

The private COM worker now drains incoming driver packets between the same
256-frame chunks MonitorProgram already used internally. It retains the 10 ms
publication size, 20 ms intended FIFO prefill and existing scene/monitor delays.
Chunk timestamps and absolute frame positions follow the original render schedule;
no extra buffering or relaxed signal thresholds are introduced. Cancellation or
transport failure prevents publication of a partially rendered block.

With that change, R55 passes ten raw-WASAPI transitions and R56 passes ten
SingZ-duplex transitions. Both have a maximum signal peak of 0.0250245, below the
unchanged 0.026 limit. All R55 verified recording intervals have zero unpublished,
expired/racing or missing-frame increments. R56 verifies signal/FIFO behavior but
has no probe QPC boundaries, so its interval flags do not establish timing scope.
Both reports complete cleanup. This is short-run evidence; extended recordings
and more stress remain release gates.

The subsequent 32-channel six-rate sweep R57 passes 44.1, 48, 88.2, 96 and
176.4 kHz. At 192 kHz, shared initialization still returns `0x8007000e` before
recording. The sweep fails overall and completes cleanup, preserving the saved
document, original microphone identities and all six system default roles.

A core regression compares complete 10 ms monitor renders with externally split
256-frame renders at all six sample rates, including a changing signal and heap
checks. Portable, ASan/UBSan and TSan runs pass. The final native Windows suite
passes all 40 tests in 13.06 seconds (`loopz-windows-format045-r15-regression.log`).

### Windows 10 shared-initialization allocation boundary

Targeted audio traces R59/R60 reproduce 192 kHz/32-channel shared initialization
failure. The adjacent 192 kHz/30-channel recording passes in R61. All reports
complete cleanup; targeted traces have zero lost events/buffers. The tested
`AudioEng.dll` version is `10.0.19041.4717`, SHA256
`08F2EBD66A344DA62A2CE8E523D9583505E24DCDDD75ABBF85319D074985AC8F`.
These observations describe that Dell installation, not every Windows version.

Resolved stacks identify `CDeviceGraphObjectCache::AllocateConnectionBuffer` →
`AERTAllocate` → `RtlAllocateMemoryBlockLookaside`. The configured connection heap
requests a maximum block of 245,760 bytes. At 32 channels, both 270,463-byte
allocation attempts return `STATUS_INVALID_PARAMETER` (`0xc000000d`); extending
the heap by 5,409,260 bytes succeeds but does not make the request valid. Static
inspection shows the caller reports a repeated null allocation as `E_OUTOFMEMORY`.
At 30 channels, the 253,567-byte request succeeds.

A separate owned-process diagnostic reproduces the exact heap configuration:
262,144 bytes succeeds, while 262,145 and 270,463 fail with `0xc000000d`, including
after successful heap extension. All successful allocations and the test heap are
freed. This establishes a configured block-size boundary rather than exhausted
system RAM. It supersedes the earlier unproven 512 KiB client-buffer hypothesis;
it does not establish a universal Windows allocation limit. Relevant events,
resolved call stacks, the diagnostic source and results are retained in
`artifacts/windows/format045-allocation-root-cause-verification.json` and its
referenced local evidence. No Windows DLL, audio-service setting or boot option
was modified.

Static inspection of this binary also finds a 1–32-channel guard in
`CreateConnectionHeap`, consistent with the existing shared-mode opening failure
above 32 channels. Exclusive initialization is still not 64-channel recording
acceptance. The wider-channel requirement remains open.

`tests/windows_audio_initialization.wprp` is an opt-in targeted tracing profile:
Windows audio events plus event stacks and process/image metadata, without pool
or CPU sampling. Its profile parses and records on the Dell. An elevated terminal
can start it with `wpr -start tests\windows_audio_initialization.wprp!LoopZAudioInitialization -filemode`
and save the owned capture with `wpr -stop <fresh-output.etl>`. Profile construction
follows Microsoft's [event-stack tracing documentation](https://devblogs.microsoft.com/performance-diagnostics/authoring-custom-profile-part3/).

The next experiment is to expose and validate shorter shared periods using the
[documented driver packet constraints](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-_ksaudio_packetsize_constraints2)
and [low-latency WASAPI path](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio).
This must include actual 192 kHz/32-channel recording and ordinary external-client
behavior, rather than accepting an initialization-only or probe-only workaround.
No fix for the 192 kHz failure is claimed yet.

### Short processing periods and the 8 ms OEM default (workbench 0.4.0.8)

The driver now publishes `DEVPKEY_KsAudio_PacketSize_Constraints2` on both wave
interfaces: a 2 ms transport floor with explicit 4 ms processing constraints for
default/raw modes. The initial 0.4.0.6 header-only constraint did not expose a
short shared period; 0.4.0.7's complete mode payload does. `initguid.h` precedes
the driver's GUID definitions so the SDK property key is instantiated. Both
strict WDK builds and Universal API validation pass without warnings.

R64 opens 192 kHz/32 channels with `IAudioClient3` at 768 frames (4 ms), but its
recording fails because of silence. R65 passes a short recording after fixing an
unrelated source-refresh interruption. Ordinary `IAudioClient::Initialize`
still fails in R66. Keeping separate short-period clients active does not repair
that ordinary-client initialization in R67; the opt-in lease experiment is not a
production fallback.

Microsoft documents [PKEY_AudioEngine_OEMPeriod](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/pkey-audioengine-oemperiod)
as a per-endpoint default-period override. Workbench 0.4.0.8 sets an 8 ms VT_BLOB
in the two existing topology endpoint property groups, leaving other devices
alone. R69/R71 now open ordinary 192 kHz/32-channel shared streams; the buffer is
3,378 frames and R71 reports an 80,000-HNS default on both endpoints. This removes
the initialization failure, but does not establish recording acceptance: R69 and
the 10-second R71 still fail waveform continuity/gain.

All three signed workbench upgrades install without reboot and preserve the
saved document and six audio-default roles. Endpoint GUIDs regenerate each time,
so upgrade identity remains unresolved. The installed 0.4.0.8 SYS hash is
`BA280BDB0D357067D016775B2E9E8514886575FB1BD26F9E9A0970DEB2002833`;
0.4.0.8 changes the INF rather than the SYS from 0.4.0.7. The original microphone
UID is now `{0.0.1.00000000}.{df4a558e-b308-458d-8258-8429e1d7b8a7}`.
No new production ZIP or Microsoft-signed release exists.

### Refresh isolation and stronger waveform acceptance

The runtime previously rebuilt all scenes whenever any application appeared or
disappeared, and retried an unchanged unavailable saved application on every
three-second inventory poll. `sourceRefreshRequired` now compares only enabled,
unmuted configured application bindings, including their process IDs. A missing
application remains pending until it appears; actual capture errors for an
available binding and non-application failures retain their retry behavior.
Regression coverage includes unrelated additions/removals/reordering, source
appearance/disappearance/restart, persistent missing applications, enabled/muted
states and capture errors. Portable and native Windows runs pass.

A private cable retains its input history and monitor bus across restart. If
inventory/topology did not change, recovery now restarts an unhealthy cable in
place, rather than rebuilding every healthy mix. Transport diagnostics report
restart attempts. R71 demonstrates why this matters: an unrelated saved-mix
worker misses its deadline, and the former recovery path resets the test graph's
render counters. Isolation does not remove the underlying deadline/load problem.

The opt-in format probe accepts an optional 1,500–30,000 ms recording duration;
`LOOPZ_FORMAT_RECORD_MS` selects it in acceptance. Longer runs sample engine state
throughout the recording, with QPC interval validation. Existing peak/gain/leak
limits remain. New continuity checks also reject multi-frame common-channel
silence and sample steps exceeding the generated sinusoids' bound plus PCM16
quantization allowance. A short gap can no longer pass merely because its
average spectral gain remains close to the target.

R70 passes a 10-second ordinary stereo/48 kHz recording with 16 validated runtime
snapshots, monotonically increasing render counts, no zero frames, no public
position/timestamp gaps and maximum sample step 0.0016175 below 0.0016944. Both
endpoints report the 8 ms default. R68's 10-second short-period 192 kHz/32-channel
recording fails the new continuity checks despite stable scene counters and no
additional input-history misses during the sampled interval. R71 ordinary
high-rate recording also fails, with graph lateness reaching 33.34 ms and a
separate scene reset from failed-worker recovery. High-rate timing, waveform
continuity and broader format regression remain open; do not label the format
complete merely because it now initializes.

R72 exercises the retained isolated-recovery path: the saved microphone worker
restarts three times while all sixteen sampled test-graph counters remain
monotonic. It still fails high-rate waveform acceptance and reaches 38.1 ms graph
lateness. R73 tests Pro Audio/high thread scheduling without system registry
changes; registration succeeds, but waveform acceptance still fails and graph
lateness reaches 32.8 ms. That scheduling experiment was removed. The normal
Audio scheduling policy, source-refresh isolation, per-cable recovery, 8 ms
endpoint default and stricter tests are retained.

### Packet-completion notifications (workbench 0.4.0.9)

The final 0.4.0.8 stereo/48 kHz 10-second check R74 passes. R75 then
reveals a 44.1 kHz regression with 59 capture-position gaps and 60 discontinuity
flags. The old timer floors notification intervals to milliseconds even when
packet frames require a fractional period. The 0.4.0.9 timer updates DMA on each
1 ms tick and signals only after the coherent packet counter advances. Separate
notification progress preserves completions already observed by position queries;
STOP resets both counters, while pause/resume retains their relationship. EOS
continues to update DMA and signal the final partial buffer.

The strict WDK/Universal build passes with zero warnings; all 41 Windows native
tests pass in 13.15 seconds. Portable and native tests check
actual whole-frame packet boundaries immediately before/at completion across all
six sample rates. The signed upgrade needs no reboot and preserves the saved
document and default audio roles, but again regenerates endpoint IDs. Loaded SYS
SHA-256 is `619A3D3752E7BF2F3E832B607F40EA1D010595C5EDB12327307D72F11C867C4A`.

R76 passes stereo recordings at 48, 44.1, 88.2 and 96 kHz with no zero frames
or capture-position gaps. The 176.4 kHz recording still fails waveform continuity
(2,950 zero frames, longest run 1,074) despite zero capture-position gaps. R77's
ordinary 192 kHz/32-channel 10-second recording also opens normally and has no
capture-position gaps, but fails with 20,940 zero frames (longest 18,406) and a
0.02499 sample step above its 0.001414 bound. Both tests restore their temporary
mix and adapter. Notification timing is improved; high-rate continuity and full
format acceptance remain unresolved.

### Cross-mix conversion cost and exact integer decimation

R78 temporarily disables the saved mix through normal edit controls, then
restores it in cleanup. The 192 kHz/32-channel ordinary shared recording now
passes for 10 seconds: no zero frames or capture-position gaps, and sample step
0.0013428 below 0.0014138. This diagnostic alone does not establish concurrent
mixing; it identifies an interaction with the other mix's publication workload.
The opt-in `LOOPZ_FORMAT_PAUSE_SAVED_MIXES=1` acceptance flag records the isolation
mode and verifies restoration of the document, endpoint IDs and audio defaults.

The shared FIR reader now has a separate bounded workspace for at most eight
channels during exact 2:1/4:1 downsampling. It caches the complete source window,
reuses the constant-phase coefficients, and skips dot products for channels
whose cached samples are all zero. It still validates every required frame and
channel; missing-data behavior and availability masks remain intact. Noninteger
ratios retain their existing path. No audio-thread allocation or shared scratch
is introduced. Wider-channel workspace bounds remain unchanged.

Independent 32-frame original-filter comparisons cover 1/2/8 channels, both
ratios, fractional/negative/integer phases, partial blocks, strides, missing
frames/channels, NaN/infinity and extreme magnitudes. Portable optimized,
AddressSanitizer/UndefinedBehaviorSanitizer and all 41 Windows native tests pass
(the complete native suite takes 14.88 seconds). Dell's 256-frame 96→48 kHz
benchmark falls from 1,700.3 to 359.3 microseconds at stereo and from 2,422.6 to
637.2 microseconds at eight channels.

R80 repeats the actual 10-second ordinary 192 kHz/32-channel recording with the
saved mix enabled: it passes with no zero frames or capture-position gaps and
maximum sample step 0.0013428. Graph lateness peaks at 4.29 ms, compared with
34.10 ms in the prior concurrent failure. Test cleanup preserves the saved
mix, endpoints and defaults. This proves that one concurrent high-rate case;
the complete format matrix, repeated/longer runs and other release gates remain
open. R81's sequential 32-channel sweep passes 48/44.1/88.2/96/176.4 kHz,
then fails at 192 kHz with 59,980 zero frames (longest run 53,632). The two in-recording snapshots have no capture-position gaps, no additional
ingress-history misses and only 1.53 ms maximum graph lateness. However, the final
failure-state snapshot reaches 52.54 ms lateness; those earlier samples do not
rule out a later scheduling stall. R80 must not be presented as repeatable
acceptance.
R81 removes its test mix/adapter and preserves the saved document and defaults.
R82's fresh-start 30-second 192 kHz/32-channel recording also fails: 17,024
zero frames (longest 13,056), no capture-position gaps or ingress zero runs,
and maximum graph lateness 27.90 ms. Cleanup restores the saved mix/defaults
and removes the test adapter. Sustained high-rate reliability remains open.

### Locating intermittent publication gaps

The private worker now counts quantized outgoing frames and all-channel zero
frames before writing to the driver. Runtime snapshots also include
`microphone.publicationRead` from the monitor program (callbacks, missing frames,
graph failures and invalid blocks). Acceptance captures those counters during
recording. These diagnostics do not change buffering, gain or driver transport.

R83 passes a fresh 30-second 192 kHz/32-channel recording with the saved mix
active. R84 repeats the same workload and fails with 58,878 zero frames. The
final monitor publication counter reports exactly 58,878 missing frames, with
no graph failures, invalid blocks or input ingress zero runs. The transport
worker's maximum lateness is 1.26 ms while the graph reaches 48.29 ms. Thus this
failure is already present in reads of the graph's monitor history; a continuous
input stream does not imply a continuous published microphone. Both runs clean
up their temporary mix and preserve the saved document/defaults.

An explicit SSE2 FFT butterfly experiment passes reference tests, but improves
32-channel unity conversion only from 605.6 to 596.8 microseconds (192 kHz).
That experiment was removed. A separate coverage optimization checks a fully
valid source window once, rather than repeating identical per-tap validity
checks for every overlapping output frame. Partial windows retain the original
per-tap rules, including zero-weight support. The fixed-ratio reader likewise
reuses normalization and coverage when its complete cached window is valid.
Both coverage paths pass the original-FIR comparisons, the full portable
AddressSanitizer/UndefinedBehaviorSanitizer resampling suite, and all 41 Windows
native tests (14.93 seconds). On the Dell, 32-channel 192 kHz unity conversion
falls from 605.6 to 484.6 microseconds per 256-frame block. Stereo 96→48 kHz
conversion falls from 359.3 to 113.7 microseconds; eight-channel conversion falls
from 637.2 to 416.1 microseconds. These are processing-time measurements;
recording reliability still requires separate acceptance. R89 passes a 30-second
ordinary 192 kHz/32-channel recording with the saved mix enabled: no zero frames,
no capture-position gaps, zero monitor-history missing frames in sampled status,
and sample step 0.0013428 below 0.0014138. Graph lateness peaks at 5.88 ms in
recorded snapshots. Cleanup restores the saved document/defaults and removes the
temporary adapter. R90 repeats the 30-second case from a separate engine start
and also passes with no zero frames, position gaps or monitor-history misses;
its recorded graph lateness peaks at 3.85 ms. Both checks preserve the saved mix
and audio defaults. Current-build rate-transition and full-matrix acceptance
remain open. Build hashes are retained in `coverage-r90-build-hashes.json`.

### Per-root endpoint references (workbench 0.4.0.12)

R91 passes the current 0.4.0.9 driver's complete six-rate, 32-channel recording
sweep, including the transition to 192 kHz. Cleanup preserves the saved mix,
existing endpoint IDs and all default audio roles. This covers one width across
six rates, not the full 192 advertised profiles.

Microsoft's [audio migration guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/operating-system-upgrades)
identifies duplicate reference strings on multiple root-enumerated software
adapters as a migration problem on Windows 10. The driver now derives a
vendor-prefixed registration name from each durable root instance and filter
kind. It retains the static names as INF property templates and per-adapter
cache keys. Reference strings remain identical across subsequent versions for
the same mix and filter. This is a prerequisite for migration, not proof that
Windows preserves every external application's cached MMDevice ID.

The initial 0.4.0.10/11 trials start the adapter but fail to publish endpoints.
The retained 0.4.0.12 initializes the KS proxy and display-name values on every
INF-declared interface category, in addition to template EP/FX subkeys. Existing
root values remain intact. It also keeps registered-name storage in the adapter
object, rather than a returning stack frame: [PcRegisterSubdevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcregistersubdevice)
retains its name pointer. PortCls manages activation and teardown of the category
interfaces; no manual global audio-service or endpoint-ID modification is used.

0.4.0.12 restores both LoopZ endpoints without reboot and preserves the saved
document and all six physical default roles. Earlier failed attempts are retained
as failed evidence. The first transition to these new reference names cannot
prove upgrade identity; a later real version upgrade, including multiple installed
mixes and existing recording selections, is still required. Portable optimized
AddressSanitizer/UndefinedBehaviorSanitizer checks cover deterministic names,
case normalization, distinct roots/filter kinds, legacy root IDs, invalid paths,
empty/oversized/unterminated names, and exact output-buffer boundaries. Strict
WDK compilation and Universal API validation pass with zero warnings. All 41
Windows native tests pass in 14.15 seconds. R92 then passes recordings at all six
sample rates at 32 channels on 0.4.0.12. A post-cleanup inventory exactly matches
the recovered baseline, including the original capture/feed endpoints and both
physical endpoints. No identity-through-version-upgrade claim is made yet.
The installed SYS hash is
`90749DEBD819B2D39DC6C890E30367902969C9F3053896C0C51C4833110DBF87`.

### Real two-mix upgrade and remembered selections (0.4.0.13)

The opt-in `windows_upgrade_fixture.py` journals the original saved document,
audio defaults and microphone IDs before creating a second isolated mix. The
journal supports cleanup after a failed test, validates the current owner's
fixture identity, and removes only that mix and root adapter. R93 captures two
installed microphones on 0.4.0.12, then installs the version-only 0.4.0.13 package.
The strict driver build/Universal checks pass with zero warnings. This is a real
package upgrade; its registered per-root names and SYS code remain unchanged.

Both durable mix keys, the document and all six default roles survive. However,
all four public capture/feed IDs regenerate. The enhanced upgrade check attempts
to reopen each pre-upgrade microphone selection; both fail as inactive. Unique
reference names therefore do not prove stable MMDevice IDs on this Windows 10
installation. Keep the external-selection preservation gate open. Do not rewrite
Windows endpoint IDs or claim a fixture pass from preserved mix keys alone.

Cleanup removes the temporary mix/root and restores the original document and
defaults. R94 separately opens the current original microphone ID through normal
shared-mode WASAPI and reads a 384-frame capture packet; the new selection probe
is not merely failing every ID. The probe only opens active, PnP-verified LoopZ
microphones and records packet metadata rather than audio content.

Desktop microphone setup now compares known selections before and after repair.
If an existing mix receives a different active microphone ID, the successful
reply reports that reselection is needed. The Windows Settings view then tells
users to select LoopZ microphones again in recording/calling apps. New/deleted
mixes and unavailable snapshots do not produce a false notice; snapshot failures
do not prevent setup repair. Unit cases cover these distinctions. The obsolete
Windows UI claim that all mixes run at 48 kHz is replaced with the saved-rate and
channel-layout behavior. The two new messages include Russian and Chinese copy.

All 41 Windows native tests pass in 13.71 seconds; all 68 interface unit tests
and the production-interface browser fixture pass.
The browser check verifies notice appearance and clearing, and retains
`setup-selection-notice-r93.png`; it does not test an actual UAC click or establish
native Dell visual acceptance. The Dell's previous interface bundle is backed up
before installing the tested prebuilt assets. Stage hashes match the tested local
bundle, and the routing file returns to its original byte hash
`99D9104E08F9DA9385A603E50E6E0007A342F75196D73A860F38620840C1F55D`.
Broader native setup/locale QA and
external application selection persistence remain open.

### Actual wide-channel exclusive recording

The opt-in `--format-roundtrip-exclusive` probe opens only the isolated mix's
LoopZ feed/microphone in exclusive PCM16 mode and records the complete public
feed → private ingress → saved half-gain scene → private output → public capture
path. Existing shared-mode probes remain unchanged and never silently fall back.
The exclusive client uses complete event-driven buffers and direct `GetBuffer`
for capture; Microsoft's [GetNextPacketSize documentation](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getnextpacketsize)
restricts packet-size polling to shared-mode streams. Alignment retries reopen
the client with its negotiated buffer duration. The probe reports its MMCSS
registration and retains the same waveform, gain, leak and continuity limits.

R95 passes an actual 48 kHz/64-channel recording: no zero frames, capture
position gaps, discontinuity flags or timestamp errors; target gain is
0.024970–0.024973. R96 passes 64 channels at 48/44.1/88.2/96 kHz, then fails at
176.4 kHz with 219,234 zero frames (longest run 70,656). Capture position remains
continuous, but the graph reaches 86.27 ms lateness, reanchors five times and
skips 75,520 render frames. Its thread consumes 2.83 seconds of CPU, and source
reads account for 1.65 seconds of elapsed render time. Both tests restore the
saved document/defaults and remove their temporary adapter. Shared mode above
32 channels and reliable high-rate 64-channel processing remain open.

The retained transform now fuses adjacent radix-2 stages into three-twiddle
butterflies and combines the trivial first two stages. Its immutable control
plan contains a full 512-root table; per-reader scratch, source support, double
intermediates, normalization and fractional-phase eligibility stay unchanged.
Original 32-frame FIR comparisons cover full/partial windows, gaps, odd and wide
channel counts, negative/fractional phases, strides and extreme float magnitudes.
The complete portable ASan/UBSan suite and all 41 Windows native tests pass
(14.78 seconds for native). Fresh Dell measurements reduce 64-channel 176.4 kHz
conversion from 972.5 to 654.9 microseconds and 192 kHz from 943.3 to 654.3;
32-channel 192 kHz falls from 485.1 to 351.1 microseconds. Processing speed alone
is not recording acceptance. R98 repeats 176.4 kHz/64-channel exclusive recording
and passes with no zero frames, position gaps, discontinuities or timestamp
errors. Its maximum sample step is 0.0025025 below the 0.0026290 bound. R99's
192 kHz/64-channel recording still fails with 203,034 zero frames (longest run
161,792), despite continuous capture positions. Both remove their temporary mix
and preserve the saved document/defaults. R100 then passes a 10-second
192 kHz/32-channel ordinary shared recording with the new common transform.
Reliable highest-rate 64-channel processing and shared mode above 32 channels
remain unresolved.

### Measuring and reducing steady-state mixing cost

R101 adds opt-in `LOOPZ_SCENE_STAGE_TIMING=1` diagnostics, prepared on the
control thread and off by default. Device snapshots expose cumulative input,
graph, output/history/meter and sink nanoseconds. Ordinary rendering reads no
stage clocks. The exclusive 192 kHz/64-channel recording still fails: in its
second observation, graph work averages 276 microseconds/block, input work 823,
output work 153 and publication 94. Total render work averages 1.384 ms against
a 1.333 ms block budget. These are cumulative workbench measurements, not a
latency guarantee.

A second pinned build overlay preserves the upstream archive and changes only
steady-state gain/limiter traversal to contiguous planes. Pending automation or
ramps retain the upstream sample-accurate path. R102 lowers measured graph work
to 221 microseconds/block (about 20%), but the 192 kHz/64-channel recording still
fails, with continuous capture positions and missing graph publication data.
Do not interpret this optimization as acceptance of that format.

All 42 Windows native tests pass in 14.14 seconds (R27). The permanent
`loopz-singz-planar` test compares 480 exact output/diagnostic
cases against the original traversal, covering all supported width boundaries,
non-finite values, signed zero, saturated counts and empty/partial/full blocks.
It passes under address/undefined-behavior sanitizers. The upstream graph suite
also passes with sanitizers, including automation across partitions and golden
offline output (`planar-zdsp-graph-asan-r102.log`). R103 repeats the short
176.4 kHz/64-channel exclusive success; R104 repeats the 10-second shared
192 kHz/32-channel success with no all-channel zeros or capture-position gaps
(one reported capture-discontinuity flag remains in that shared probe).
Both restore the saved document, existing microphones and all defaults.
`planar-stage-r104.json` records the staged hashes and byte-for-byte restoration
of the original saved mix. Driver 0.4.0.13 and transport2 remain unchanged.

### Meter processing cost on the Dell

The next measured cost was channel metering. The x64 implementation now uses
mandatory-baseline SSE2 to sanitize four samples at once, find peaks and sum
squared samples in double precision. It keeps bounded local scratch and the
existing atomic observation publication. Other architectures retain the scalar
implementation. This changes meter reduction order only; it does not change
mix samples, filter support, audio clock tolerances or waveform acceptance.

`loopz-audio-meter` compares 400 cases with scalar reference observations:
1/2/8/17/64 channels, aligned and unaligned input, 1–1025 frames, finite extremes,
NaNs/infinities, subnormals, silence, partial layouts and invalid empty blocks.
Peaks compare exactly; RMS allows two float rounding units of relative error.
The checks pass on Windows and under address/undefined-behavior sanitizers in
both native ARM and x86-64 builds. The latter executes the actual SSE2 path.
The Dell's 64-channel/256-frame meter benchmark improves from 70.85 to
11.15 microseconds per call (`meter-before-r105.json`, `meter-after-r105.json`).
All 43 Windows native tests pass in 14.64 seconds (R28).

R106's actual 192 kHz/64-channel exclusive recording still fails with 226,816
all-channel-zero frames. Capture positions remain continuous; publication
missing-frame counters rise. The second sampled graph render averages 1.370 ms,
still over its 1.333 ms deadline. Its source read now averages 797 microseconds;
stage measurements from separate runs include scheduling/clock variation and
must not be treated as a fixed improvement in whole-graph throughput. The meter
benchmark proves reduced meter cost, not acceptance of the failing profile.
R107 repeats the short 176.4 kHz/64-channel exclusive success after this change.
R108 passes the 10-second shared 192 kHz/32-channel recording again. Both
restore the saved document, existing microphone IDs and all audio defaults;
`meter-stage-r108.json` confirms the original saved-document byte hash and the
staged application/driver hashes. No boot settings were changed.

### Paired forward/inverse transform ordering

The fractional-delay convolution now uses a forward decimation-in-frequency
transform and an inverse decimation-in-time transform. Both share bit-reversed
spectra, so frequency multiplication is unchanged and neither needs an explicit
reordering pass. The control-prepared reversal table is removed. All arithmetic
remains double precision; pair packing, extreme-magnitude separation, filter
coefficients, source coverage, frame eligibility and clock tolerances are
unchanged. No shared scratch or callback allocations are introduced.

R109's fresh Dell before/after benchmarks improve 64-channel unity conversion
from 656.9 to 578.0 microseconds at 192 kHz (about 12%); the 32-channel case
improves from 341.2 to 301.0 microseconds. Full FIR-reference/spectral tests pass
with address/undefined-behavior sanitizers on ARM and x86-64, including partial
windows, gaps, strides and extreme amplitudes. All 43 Windows native tests pass
in 14.57 seconds (R29). Evidence: `dif-before-r109.jsonl`,
`dif-after-r109.jsonl`, `dif-asan-r109.log`, `dif-x86-asan-r109.log`, and
`loopz-windows-format0413-r29-regression.log`.

R110 is the first passing short 192 kHz/64-channel exclusive PCM16 recording:
270 capture packets, no all-channel-zero frames, no capture position gaps,
discontinuities or timestamp errors. Maximum sample step is 0.0022889 below
0.0024205, with the same half-gain and leakage checks. Render observations have
no reanchors or skipped frames and maximum lateness 5.15 ms. The saved document,
existing microphone IDs and all audio defaults are restored. This short result
alone does not establish long-recording or transition reliability.
R112 subsequently passes all six short 64-channel exclusive profiles in one
48/44.1/88.2/96/176.4/192 kHz sweep, each with no all-channel zeros,
capture-position gaps, discontinuities or timestamp errors. R111's separate
30-second 192 kHz/64-channel check still fails with 8,689 zero frames (longest
1,753). No graph reanchors or skipped frames occur; average render work is
1.206 ms/block, but maximum scheduling lateness reaches 22.72 ms. Publication
missing counters rise while source missing counters remain stable after
startup. Extended reliability therefore remains open despite the short sweep.
An 8 ms preparation-lead experiment (R113), retaining all signal delays, also
fails its 30-second recording with 31,658 zero frames. Source missing counts
stay stable after startup, while publication misses rise; it does not establish
an improvement over R111. That scheduling experiment and its helper were
removed. The retained runtime still uses zero render lead and unchanged source
and publication delays. The failing R113 report is kept for diagnosis.



The retained configuration passes all 43 native tests in 13.68 seconds (R31)
after the scheduling experiment is removed. R115 also passes the 10-second
shared 192 kHz/32-channel recording. Current acceptance evidence must retain the
failed extended 64-channel runs as well as the passing short tests; this does
not establish completion of the Windows port. R114 repeats all six short
64-channel exclusive rates successfully after removing the experiment.
`dif-stage-r115.json` records final staged hashes and exact original-document
byte-hash restoration; both short sweeps also preserve all existing microphone
IDs and audio defaults. Driver 0.4.0.13/transport2 remain unchanged.

### Exact finite-sample containment on x64

R116 replaces repeated per-sample containment in scene input/output blocks and
resampler source frames with a bounded SSE2 loop. Every finite bit pattern,
including signed zero and subnormals, is preserved. NaNs/infinities still become
positive zero; unavailable source channels are still zero-filled. This does not
change FIR coefficients, coverage, clock tolerances, signal delays or packet
handling. Other architectures keep the scalar implementation.

The permanent `loopz-finite-samples` test compares 624 exact scalar-reference
cases (aligned/unaligned, empty, tails and full blocks) and 65,792 arbitrary
float encodings, checking untouched guards as well as output bits. It passes
under address/undefined-behavior sanitizers on ARM and x86-64. Full resampling
reference/spectral/concurrency tests pass under both sanitizer builds too.
All 44 Windows native tests pass in 14.56 seconds (R32).

The Dell's 16,384-sample containment benchmark falls from 55.26 to 3.94
microseconds per call. This is an isolated loop measurement, not a whole-route
throughput or latency guarantee. Evidence: `finite-reference-r116.json`,
`finite-optimized-r116.json`, `finite-{arm,x86}-asan-r116.log`,
`finite-resampling-{arm,x86}-asan-r116.log` and
`loopz-windows-format0413-r32-regression.log`.

R117's 30-second 192 kHz/64-channel recording still fails, but distinguishes the
remaining bottleneck: graph publication missing frames remain zero throughout,
and average render work drops to 0.867 ms/block (maximum lateness 14.37 ms,
no graph reanchors/skipped frames). The private transport worker restarts twice;
source history gaps then account for much of the 1,049,358 recorded zero frames.
The containment speedup therefore fixes graph throughput for this run without
establishing reliable recording. The failed report is retained.

### Private-worker monitor processing and PCM16 encoding

R120 applies the exact containment helper to MonitorProgram's private input and
output planes, retaining its unavailable-input/output-channel silence rules.
The private publication worker now converts planar float samples in 4-by-4 tiles
to interleaved PCM16 with baseline SSE2, preserving finite sanitization,
saturation, truncation, little-endian bytes and post-quantization all-zero-frame
counting. Scalar tails and other architectures retain equivalent handling. No
allocation, packet format, driver ABI, presentation time or queue delay changes.

`loopz-windows-pcm16` compares 1,215 exact byte/count cases across 1/2/3/4/6/8/16/
32/64 channels, partial/empty/full frames, unaligned buffers, arbitrary float
encodings, clipping, non-finite samples and values around one PCM16 quantum.
Guard bytes must remain unchanged. PCM16 and monitor tests pass with address/
undefined-behavior sanitizers on ARM and x86-64. The Dell's 64-channel/1,920-frame
encoding benchmark improves from 662.24 to 128.17 microseconds. This is an
isolated conversion benchmark, not recording acceptance. Deadline miss counts
and the last missed interval now survive in-place worker restarts, while the
30 ms failure threshold remains unchanged.

All 45 native Windows tests pass in 14.63 seconds (R33). R121 then passes a
30-second 192 kHz/64-channel exclusive PCM16 recording through the actual saved
half-gain scene: 3,120 capture packets, no capture-position gaps, discontinuities
or timestamp errors, and no private-worker restarts/deadline misses. Publication
missing counts stay zero. There are 60 isolated single-frame all-channel zeros
at ordinary common tone crossings; the longest run is one frame. Maximum sample
step is 0.0022889, below the unchanged 0.0024205 bound, and every channel passes
the gain/leakage checks. Maximum observed graph lateness is 3.281 ms and worker
render time is 8.955 ms. This is the first extended 64-channel success, not proof
of the complete format/lifecycle matrix.

R122 then passes all six short 64-channel exclusive rates in one sweep; R123
passes the 10-second shared 192 kHz/32-channel recording. Each has zero private
worker restarts/deadline misses and no capture-position gaps. The short sweeps
also have no all-channel-zero frames. Both restore the saved document, existing
microphone IDs and all system audio defaults.

R124 independently repeats the 30-second 192 kHz/64-channel success after those
format checks: no all-channel zeros, capture-position gaps/discontinuities,
publication misses, worker restarts or deadline misses. The sample-step bound
and all gain/leakage checks pass unchanged. Maximum observed graph lateness is
6.56 ms; the largest worker render interval is 12.55 ms, absorbed by the existing
queue without changing its delay. These two successful runs establish this
30-second exclusive PCM16 scenario, not longer stress/lifecycle coverage or
shared-mode support above 32 channels.


### Intermediate-width high-rate coverage

Before the full matrix, R125 exposes a previously untested 192 kHz/14-channel
failure: 229,502 zero frames in a short exclusive recording, with continuous
capture positions. The double-precision convolution path was unnecessarily
limited to at least 16 channels. It now accepts every validated channel count;
its frame range, exact unity-step/fractional-phase conditions, filter support,
normalization, coverage rules and extreme-magnitude separation remain unchanged.
No acceptance thresholds or audio delays change.

Unity FIR-reference coverage now includes 1/2/4/6/7/8/10/12/14/16/17/32/64
channels, including gaps, partial windows, strides and extreme amplitudes. The
full resampling suite passes address/undefined-behavior sanitizers on ARM and
x86-64. All 45 Windows native tests pass in 14.90 seconds (R34). Fresh Dell
192 kHz unity benchmarks improve stereo from 853.5 to 39.0 microseconds and
eight channels from 1160.5 to 79.4 microseconds. Wide-channel measurements are
approximately unchanged. Evidence: `narrow-unity-{before,after}-r127.jsonl`,
`narrow-unity-{arm,x86}-asan-r127.log` and R34's native regression log.
R128 then passes the previously failing short 192 kHz/14-channel exclusive
recording, with no all-channel-zero frames or capture-position gaps, unchanged
gain/leakage/sample-step checks, and successful document/default cleanup.
The full 192-format exclusive matrix R126 terminated after 41/192 passing
profiles: all 32 advertised 48 kHz widths and 44.1 kHz widths through 18 channels.
The 44.1 kHz/20-channel recording failed waveform checks, including a long zero
run. The report records successful cleanup; the original saved document hash
was restored and its engine is stopped. No full-matrix acceptance is claimed.
`format0413-full-exclusive-r126-report.json` is the terminal result;
`full-exclusive-r126-run.json` records staged hashes and the historical engine
identity. Earlier live-progress snapshots are superseded by this final report.

### General FIR coverage optimization (R129)

For complete cached source windows, the non-unity converter now sums filter
coefficients without rechecking per-tap frame validity/channel coverage. Partial
windows retain the existing coverage scan. Filter support, interpolation,
normalization order, sample calculations and acceptance thresholds are unchanged.
The resampling suite passes ARM and x86-64 address/undefined-behavior sanitizers
(`general-coverage-{arm,x86}-asan-r129.log`). The formerly failing 44.1 kHz/
20-channel physical exclusive recording passes a fresh 1.5-second test with zero
all-channel-zero frames, position gaps or discontinuities, and successful cleanup
(`format0413-single4410020-coverage-r129-report.json`). One short pass does not
establish that the earlier intermittent failure is resolved; full-matrix and
extended recording verification remain required.

R35 passes all 45 Windows native tests in 14.98 seconds. A new complete sweep,
R130, is live (session 1871, engine PID 11116); its captured progress has 14/192
passing profiles. `format0413-coverage-verification.json` records its exact staged
application hash and current evidence. This is a progress snapshot, not final
acceptance. Do not restart it, copy staged executables or run competing physical
acceptance until its process is verified terminal and cleanup is inspected.

### Automatic microphone management: remaining implementation plan

Astra's updated source review replaces the obsolete fixed-slot prototype plan.
Keep the existing per-mix adapters and per-user audio engine. Extract the current
privileged creation/binding/format/restart/removal operations from
`driver_setup.cpp` into a device-management module, then expose a narrow installed
SCM service. Routine format or mix edits must not upgrade the driver package.
The current elevated helper remains for installation and exceptional repair.

The service must accept bounded mix metadata and explicit deletion intent, never
caller-supplied SIDs, document paths, registry paths, driver paths or commands.
Authenticate local named-pipe clients through their Windows tokens and derive
owner SID server-side. Reject remote clients and pipe squatting. Preserve the
existing SID/mix-ID identity scheme and verify ownership before every mutation.
Place service binaries/driver package under protected Program Files and durable
per-owner journals under protected ProgramData; ordinary users cannot change
service configuration. Use revision/CAS tokens, idempotent operation IDs and
explicit deletion tombstones. Missing/corrupt intent cannot mean “remove all.”

Validate actual SYSTEM/Session 0 format finalization on an isolated test adapter
before completing integration: elevated-user Session 0 acceptance is not proof
that LocalSystem MMDevice/AudioClient and private policy calls behave identically.
The per-user engine should asynchronously coalesce reconciliation after durable
edits, startup and reconnect, without holding audio/control locks through PnP
waits. Expose per-mix creating/applying/ready/removal-pending/reboot-required/error
states and keep unaffected streams alive. Install, sign, verify and recover the
service through the existing installer; do not run a privileged service from a
user-writable development staging directory.

Acceptance must cover standard-user create/rename/format/delete/undo without
another elevation prompt, cross-owner isolation, forged/stale/oversized IPC,
interrupted operations, concurrent edits, service/engine crash recovery, other
mix recording continuity, and unchanged defaults/selections after no-op sync.
Driver-upgrade MMDevice-ID preservation remains a separate documented failure;
the service cannot present the existing reselection notice as a fix.

### Device broker request contract (R131)

`device_intent.hpp/.cpp` implements a bounded versioned request contract for
single-mix upsert and explicit removal, operation UUIDs and exact integer CAS
revisions. It accepts only mix ID/name and supported nominal rate/channel metadata;
owner SID, paths, driver commands and implicit remove-all requests are absent and
unknown fields are rejected. Decoding also rejects duplicate JSON keys, malformed
UTF-8, fractional/negative/overflowing integers, unsupported formats, excessive
nesting and payloads above 64 KiB. IDs/names retain the document's 4096-byte limits.
The encoder validates its output with the same decoder. ARM and x86-64 sanitizer
tests cover all 192 advertised formats, Unicode, large exact revisions, explicit
removal and adversarial inputs (`device-intent-{arm,x86}-asan-r131.log`).

This is the protocol component only: token authentication, durable journal/CAS
application, protected SCM service, actual device operations, installer and engine
integration remain unimplemented. It is not yet in the running Dell build; no
service is installed or exposed by this change. A revision in a validated request
must still be checked and durably committed by the future service.

### Device intent journal model (R132)

`device_intent_journal.hpp/.cpp` adds the per-owner state transition and recovery
format needed by the broker. New operations require the current exact revision;
identical retries return their original revision without changing desired state,
while changed content under a retained operation ID is rejected. The last 64
receipts are retained, and unchanged desired records also recognize older retries.
An evicted operation with a stale revision is rejected. The journal stores up to
32 desired microphones plus a bounded backlog of explicit removal tombstones
(256 total records). No absent-record or missing-document deletion rule exists.
Only an acknowledgment matching the latest removal revision can erase a tombstone;
a delayed acknowledgment cannot erase a newer upsert or removal.

ARM and x86-64 sanitizer tests pass CAS conflicts, lost-reply retries, serialized
before/after-commit snapshots, deletion/undo races, receipt eviction, live-device
and tombstone capacity, and malformed recovery state. Evidence:
`device-intent-journal-{arm,x86}-asan-r132.log`. Recovery decoding checks bounded
size/depth, duplicate keys, receipt continuity, unique IDs/revisions, supported
metadata and agreement between retained receipts and desired records. Protected
storage is still required; this format is not an authenticity mechanism.

This is a portable journal model, not disk durability or an installed service.
The future broker must prepare a copy, atomically persist it under the authenticated
owner's protected journal, then publish it and reconcile its latest records.
It must never apply a superseded incoming retry directly. Atomic Windows file
storage, token authentication, the SCM host, actual device-operation integration
and installer/engine wiring remain outstanding. Neither R131 nor R132 has been
copied into the running Dell audio-test build.

### Shared SQLite setup storage (R134, replaces R131/R132 prototypes)

The request model now lives in `native/include/loopz/device_intent.hpp` and
`native/core/device_intent.cpp`, with no Windows headers or driver-format dependency.
It accepts the shared six nominal rates and 1–64 channels; the Windows adapter
separately enforces its advertised even 2–64-channel driver formats. Owner identities
are opaque authenticated platform identities in shared code (for example a macOS
UID identity or a Windows SID), supplied by adapters rather than request fields.
The shared component does not yet provision devices on either platform.

`device_intent_database.hpp/.cpp` replaces the custom JSON journal implementation
and its recovery codec. The removed R132 model/tests and the prior Windows-prefixed
request paths above are historical prototypes, superseded by this implementation.
SQLite tables hold per-owner revisions, desired records/tombstones, and bounded
retry receipts. Composite primary/unique keys scope operations and mixes to their
owner. `BEGIN IMMEDIATE` transactions atomically update the owner revision, desired
metadata and retry receipt; stale revisions and reused operation IDs fail without
partial changes. Explicit removal acknowledgments match the current revision and
cannot erase a later upsert. Capacity remains 32 live devices, 256 pending/desired
records, and 64 recent receipts per owner. Retained desired records recognize older
identical retries; evicted stale operations fail the revision check.

SQLite is pinned to the official 3.54.1 amalgamation, with its archive SHA3-256
verified against the download page and recorded in `vendor/sqlite/README.txt`.
It is built as a static library with extension loading disabled and double-quoted
string literals disabled. The database uses rollback journaling, synchronous EXTRA,
foreign keys, defensive mode, an application ID and explicit schema version.
Unsupported/corrupt databases fail closed; initialization only permits an empty
explicitly provisioned database. Revisions use ordered eight-byte blobs, preserving
unsigned 64-bit values above SQLite's signed integer range. Bound parameters handle
all owner/request metadata. No audio-thread code uses SQLite.

The portable request and database tests pass ARM and x86-64 address/undefined
sanitizers, including the SQLite C compilation. Tests cover all shared request
formats, real child-process exit after flushing uncommitted changes, recovery on
reopen, injected transaction failure after earlier updates, competing connections,
owner isolation, retries, deletion/undo races, capacity and large exact revisions.
Evidence: `shared-sqlite-{arm,x86}-asan-r134.log` (two tests each).

The Windows wrapper in `device_intent_store.cpp` is a separate, unverified adapter:
it provisions/validates a protected fixed ProgramData folder and database file,
rejects reparse points/hard-linked files and unexpected ownership/ACLs, and validates
canonical Windows SIDs. It replaces the discarded temporary-file/rename writer.
It still needs Windows compilation and administrative permission tests. No database,
folder or service has been installed on the Dell. SCM/token/pipe integration,
device reconciliation, installer/engine wiring and macOS adoption remain outstanding.
The active R130 audio sweep uses the earlier audio build and is unaffected.

Primary references: [SQLite download](https://sqlite.org/download.html),
[transactions](https://sqlite.org/lang_transaction.html),
[durability pragmas](https://sqlite.org/pragma.html#pragma_synchronous).

### Windows SQLite permission fixture prepared (R135, not run yet)

`loopz-windows-device-store-acceptance` is an opt-in administrative executable,
not registered in CTest or installed in the package. It creates a unique sibling
`LoopZDeviceBrokerTest-<pid>-<tick>` under ProgramData and never opens the production
store or audio driver. It checks restricted-token read/write denial, independent
owner scopes, competing connection revisions, canonical SID and Windows-format
rejection, reopen persistence, hard-link rejection, refusal of inherited/insecure
directory permissions, and resistance to renaming an open store directory.
Successful runs remove their isolated folder; failures retain it for inspection.

This fixture and the Windows adapter still require a Windows build and actual
administrative execution. They have not been copied to or run on the Dell while
R130 is recording, to avoid adding build/test load or changing its active stage.
The portable SQLite tests above are separate evidence and do not prove Windows
ACL or service-token behavior.

### SQLite identity validation before persistent configuration (R136)

Startup now checks application ID/schema version (or an explicitly permitted empty
database) before setting persistent rollback-journal mode. It rechecks under the
writer transaction before initialization. A newer/unsupported database is rejected
without converting its journal mode. The regression fixture changes a database to
WAL and schema version 99, verifies startup rejects it, then verifies WAL remains
selected. Both shared tests pass ARM and x86-64 address/undefined sanitizers after
this fix (`shared-sqlite-{arm,x86}-asan-r136.log`). Earlier R134 evidence remains
historical. Windows compilation/administrative storage acceptance are still pending.

### Isolated Windows storage build staged (R137, not executed)

A source snapshot containing only the shared request/database code, the Windows
permission adapter, three fixtures and pinned SQLite sources has been staged under
`C:\Users\123\LoopZ-driver-tools\sqlite-fixture-r137`. It has an independent
CMake build and does not modify the app stage or main Windows build tree. Existing
portable declaration/JSON headers are read from the Dell source checkout. Archive
SHA-256 is `AAE4EE5C63B3B7F29B38B348916AFA24C44C2B6492E0CBE135D0CAB89B40D1C2`;
`sqlite-fixture-r137-source-hashes.json` records the snapshot source hashes.

`C:\Users\123\LoopZ-driver-tools\loopz-build-sqlite-fixture-r137.cmd` configures
and builds that isolated project, runs the two shared tests, then explicitly runs
the administrative permission fixture. The script is prepared only: do not execute
until the R130 recording process is verified terminal and its cleanup inspected.
No SCM service, production database or real microphone operation is part of this
fixture. The main Windows build/package still needs separate integration checks.

### R130 terminal full-sweep result

R130 terminated after 159/192 passing profiles. All widths 2–64 at 48, 44.1,
88.2 and 96 kHz pass; 176.4 kHz widths through 62 pass. The 176.4 kHz/64-channel
1.5-second exclusive recording fails with 2864 all-channel-zero frames (longest
run 1198), a sample-step failure, and no capture-position gaps or discontinuities.
The graph reports a maximum scheduling lateness of 57.84 ms and maximum render
call of 5.29 ms. The private worker has no restarts or deadline misses. Those
maxima do not yet locate the scheduling stall within the recording and are not
proof of its cause. Publication missing-frame counters require startup-relative
interpretation. Do not lower waveform thresholds or mark this format accepted.

`format0413-full-exclusive-r130-report.json` is the authoritative terminal report;
older progress snapshots are superseded. Cleanup succeeds with no recorded cleanup
errors, and the engine/job are verified terminal. R130 does not cover any 192 kHz
profile. Saved mix/default restoration still needs an independent final-state check.
The isolated R137 storage build can now run without competing with recording.

### Windows SQLite storage acceptance passes (R139)

The isolated Windows build now passes strict `/W4 /WX` compilation after including
the snapshot's missing driver-format header and making SID narrowing/invalid-UTF8
test-byte conversions explicit. R137/R138 failed build attempts are superseded by
`sqlite-fixture-r139-build-test.log`; no warning policy or test check was relaxed.
The two shared request/database tests pass on Windows 10 in 7.35 seconds, including
actual child-process crash recovery. The opt-in administrative fixture also passes:
restricted-token read/write denial, owner scopes, competing connection revisions,
canonical SID/Windows-format rejection, reopen persistence, hard-link rejection,
insecure-directory refusal and open-directory rename protection.

`sqlite-fixture-r139-source-hashes.json` records the final sources. R139 applies the
explicit-conversion patch and R138's `driver/audio_format.hpp` addition to the R137
isolated snapshot. No application, driver, production database or service is
installed by these tests. The normal app build/installer still need integration.
Storage acceptance does not prove named-pipe authentication or LocalSystem device
format finalization; those remain separate service gates.

The full audio sweep remains failed at 176.4 kHz/64 channels, despite 159 earlier
passes. A fresh R141 30-second focused retest uses the same audio build and adds
pre-probe publication/transport/render counter snapshots. It is a diagnostic run,
not an accepted fix. Its cleanup and terminal report must be inspected before
another recording or competing Dell build starts.

### R141 focused 176.4 kHz/64-channel recording fails

The 30-second focused run also fails, with 1591 consecutive all-channel-zero
frames beginning at frame 969706 (about 5.497 seconds). Capture positions remain
continuous. All recorded publication missing-frame counters are zero, and outgoing
zero frames remain at the startup count 66360 throughout the measured recording.
The source has no unpublished sample reads in these observations. This narrows
this failure to transport/recording after engine publication; it does not prove a
specific FIFO/DMA cause. Worker maxima rise near the gap to 9.66 ms lateness,
9.23 ms drain time and 8.10 ms render time, with no restart or deadline miss.
Graph maximum lateness reaches 14.11 ms, so the earlier R130 57.84 ms maximum is
not required to reproduce a failure. Timing maxima alone are not causal proof.

Evidence: `format0413-single17664-baseline-r141-report.json`. Cleanup succeeds.
The acceptance script now saves pre-probe publication, transport and render
counters in addition to input-history counters; thresholds are unchanged. The
159-profile R130 sweep and this failed recording remain release gates. No new
format sweep is running, and R137–R139 storage jobs are terminal.

### Incremental publication experiment rejected (R144/R145)

An experimental worker delivered each existing 256-frame render chunk immediately
instead of retaining the full 10 ms batch. It kept presentation timestamps,
render partitions, nominal rates, configured delays and PCM16 encoding unchanged,
but increased processed-transport calls. All 45 Windows native tests pass in
14.62 seconds (R36). R144 passes a 30-second 176.4 kHz/64-channel exclusive
recording with zero all-channel-zero frames, gaps or discontinuities and no
publication misses. R145 then fails 192 kHz/64 channels at 30 seconds with 6710
all-channel-zero frames, longest run 3510, and 3200 publication missing frames.
No restart or deadline miss is reported. Both tests clean up successfully.

This is not an accepted fix. The experimental worker is preserved only in
`experiment-stream-worker-r144.cpp`; production source is restored to the earlier
batch worker, rebuilt successfully and copied into the idle development stage. R145 counters identify
both publication misses and downstream gaps, so the R144 pass cannot establish
format-wide reliability. No thresholds, clocks or buffers were relaxed. The next
investigation should measure transport copy/scheduling costs rather than infer a
single cause from timing maxima. Evidence: R144/R145 JSON reports and R36 log.

### Optimized resident-buffer copies: driver 0.4.0.14 candidate (R147–R151)

The ring now copies its contiguous spans in at most two operations per read/write
and zeros only the unavailable output suffix. On MSVC x64/x86, the shared helper
uses `__movsb`/`__stosb` intrinsics, with no allocation, waiting, vector/FPU state or
large stack temporary. Other Clang/GCC targets use compiler memory builtins. The
private ingress bridge uses the same helper for packet copies and complete sample
buffer clearing. Non-overlapping resident buffers are required, as in the existing
kernel call sites. Ring capacity, prefill, accounting, packet layout, presentation
timestamps, format support and transport version 2 remain unchanged.

An isolated MSVC `/O2` Dell benchmark compares the original byte/frame loops with
the optimized ring. Median 192 kHz/64-channel 1920-frame write/read time falls from
1359.48 to 26.68 microseconds (50.96x); 176.4 kHz/64-channel 1764-frame time falls
from 1241.10 to 24.42 microseconds. This measures ring operations, not complete
kernel/DPC cost or waveform reliability. Evidence: `ring-copy-dell-r147.jsonl`.

Independent FIFO-reference coverage now spans all six rates at 2/18/64 channels,
with zero-sized operations, unaligned guards, wrap, oversized writes, overflow,
underruns, capture reset and exact missing/dropped counters. Ring and duplex tests
pass ARM and x86-64 address/undefined sanitizers (`ring-copy-{arm,x86}-asan-r149.log`).
The strict WDK driver build succeeds with zero warnings/errors (R150), and all
45 native Windows tests pass in 15.36 seconds (R37). The prior incremental worker
experiment remains reverted; this candidate only changes driver buffer copies.

The 0.4.0.14 development package is test-signed with the existing workbench key;
SYS and CAT signatures verify Valid. SYS SHA-256:
`7AAF1B661E42E02F3AB83B64AE20B3C82491F678572D7E7912B37C1C491FDD44`.
Evidence: `driver0414-sign-r151.json` and `format0414-copy-verification.json`.
It is not installed: the Dell still runs 0.4.0.13 and its prior development app.
No production package, boot settings or certificates changed. Install/upgrade
identity/default checks and physical retesting of the failing formats remain
required; no audio-gap fix or full format acceptance is claimed.

### Driver 0.4.0.14 installed; physical timing failure remains (R152–R157)

R152 installs the candidate without a reboot and verifies its SYS hash and driver
version. Saved routing bytes and all six physical default roles remain unchanged.
Windows regenerates the virtual endpoint IDs; original microphone UID is now
`{0.0.1.00000000}.{06ed9523-69c0-4225-99d8-b086584f9838}`. Upgrade identity remains
failed. Evidence: `transport-0414-upgrade-r152.json`.

R153 fails a 30-second 176.4 kHz/64-channel recording with 18939 zero frames.
Publication missing frames and additional outgoing-zero frames both total 18939;
worker lateness remains below 1.52 ms, drain below 1.79 ms, and there are no
restarts/deadline misses. Graph maximum lateness reaches 27.77 ms. This reproduces
an engine-publication timing failure; the faster driver alone is insufficient.

Runtime diagnostics now expose render TID, high-resolution timer creation success
and timer-creation error. The native runtime regression passes. R155 confirms a
high-resolution timer but fails recording; the built-in Windows 10 WPR tool fails
trace finalization with 0x80010106. Its collector is verified stopped; no usable
scheduler trace is claimed from that attempt.

Direct xperf capture is validated by a short dry run, then R156 saves an 88,080,384
byte trace with zero lost events/buffers. Its 30-second 176.4 kHz/64-channel recording
passes with zero zero-frames, position gaps and discontinuities, demonstrating
intermittence rather than proving resolution. Render TID 10980 has 20768 switch-ins,
16734 at priority 21, 4033 at priority 5 and one at priority 8. The selected thread's
largest off-CPU interval is 1.641 ms, with no intervals over 10 ms. This passing
trace does not establish the cause of a failing recording or causal significance
of priority changes. Evidence: R153/R155/R156 reports,
`scheduling0414-r156-stats.txt` and `scheduling0414-r156-summary.json`.

R157 completes a 192 kHz/64-channel, 30-second recording using the same driver
and worker. It fails with 17913 zero frames, matching publication-missing and
additional outgoing-zero counters; capture positions remain continuous. Cleanup
succeeds and the owned kernel trace is stopped and exported. Its 94,371,840-byte
trace has zero lost events/buffers. The render thread's largest off-CPU interval
is 1.828 ms, with none over 10 ms; the longest observed DPC is 1.814 ms. These
measurements do not explain the publication failure by one long scheduling or
interrupt pause. CPU attribution is being checked against an exact code-section
match before selecting another change. Full-port acceptance remains open.

CPU attribution R158 verifies an exact `.text` hash/RVA match between the traced
stage executable and its relinked map before resolving samples. Of 24006 render
thread CPU samples, 18626 are in LoopZ and 4703 in ucrtbase; FFT forward/inverse
transforms account for 7272 samples attributed to their nearest map symbols.
This supports examining render cost, without proving a specific CRT function is
responsible. Evidence: `scheduling0414-r157-cpu-map.json`.

A focused R158 change removes a redundant `isfinite` check from FFT output:
the existing ordered `abs(sample) <= float-max` comparison already rejects NaN
and infinity. Filter support, double intermediates, buffering and scheduling
remain the same. Local resampling regression passes; all 45 Windows native tests
pass in 30.72 seconds, including FIR parity, extreme values and nonfinite inputs.
R158 then fails a fresh unprofiled 192 kHz/64-channel, 30-second recording
with 989 zero frames and no capture-position gaps or discontinuities. This is
still a continuity failure; differing run conditions prevent claiming a measured
CPU improvement from the smaller gap count alone. Evidence:
`format0414-single19264-range-r158-report.json`.

R158 diagnostics distinguish its failure from R157: the test microphone has zero
publication-missing frames, while the worker's maximum render duration reaches
13.5414 ms for a 10 ms batch (maximum drain 8.071 ms, lateness 5.3123 ms).
Those maxima indicate insufficient delivery margin, but do not timestamp the
specific 989-frame recording gap; driver-queue telemetry is still needed for
causal confirmation.

R159 classifies IEEE-754 float exponent bits directly when AudioHistory publishes
atomic sample encodings. This preserves all finite bits, including signed zero
and subnormals, and maps NaN/infinity to positive zero without a CRT call per
sample. Tests compare 624 boundary/alignment cases and 65,792 arbitrary encodings;
local ASan/UBSan passes, as do focused resampling tests and all 45 Windows native
tests (15.83 seconds). The unprofiled R159 192 kHz/64-channel 30-second
recording passes with zero all-channel zero frames, capture-position gaps or
discontinuities; cleanup succeeds. The R160 176.4 kHz/64-channel follow-up also passes its 30-second recording
with zero all-channel zero frames, capture-position gaps or discontinuities and
successful cleanup.
This single pass does not establish a stable full matrix or long-run acceptance.

R161 starts the full exclusive PCM16 recording matrix on the exact R159/R160
application and driver 0.4.0.14. It covers all six rates and every even width
2..64 (192 profiles), using the existing 1500 ms per-profile continuity checks.
The live Python process is PID 6172 and execution session 13964; the first three
48 kHz profiles (2/4/6 channels) pass. This is an in-progress observation, not
acceptance. Do not restart the job, replace staged binaries or run competing
build/audio jobs until its process is terminal and cleanup is inspected.
Evidence target: `format0414-full-exclusive-r161-report.json`.

R162 verifies shared SQLite through the main CMake project on macOS arm64,
including a fresh Release build with both macOS and Windows backends disabled.
The request and database tests both pass (0.59 seconds), including recovery and
transaction cases. This establishes portable build integration, not broker or
application integration. Evidence: `shared-sqlite-core-only-r162-verification.json`
and `shared-sqlite-core-only-r162-tests.log`.

R161 remains live at 16/192 passing profiles (48 kHz through 32 channels), with
the original Python PID 6172 confirmed present. Cleanup is pending while the test
runs; do not interpret the report's interim `ok:false` as a terminal failure.

R163 prepares `broker_identity.hpp/.cpp` and an isolated named-pipe test fixture.
After a complete bounded message is read, the helper impersonates the client,
queries TokenUser, rejects anonymous and nested impersonation, and restores the
server identity before returning an owner SID. It rejects client-end/byte-mode
handles; callers must create local-only message-mode pipes. Failure to restore
identity terminates rather than processing another request under the wrong token.
The fixture checks actual process-owner equality, identification-level clients,
anonymous rejection, wrong pipe ends and preservation of a pre-existing token.
Windows compilation/execution are pending; do not claim security acceptance or
service integration. No service is installed or production pipe exposed.
The live R161 matrix now has 27/192 passing profiles (48 kHz through 54 channels);
Windows builds remain deferred to avoid competing with that test.

R164 saves and inspects an interim R161 report containing 42 passing profiles. All
32 even widths at 48 kHz are present with zero all-channel zero frames. All
completed profiles have zero capture-position gaps/discontinuities and meet the
existing maximum-step and longest-zero-run checks. The 44.1 kHz/10-channel case
has three isolated zero frames, longest run one, within the existing common-zero-
crossing allowance; it must not be described as zero-zero-frame acceptance.
The matrix remains live, with cleanup pending. Evidence:
`format0414-full-exclusive-r161-checkpoint-r164.json`.
The R163 authentication fixture is staged separately on the Dell at
`C:\Users\123\LoopZ-driver-tools\broker-identity-r163`; its build command refuses
to run while a LoopZ process exists. It has not been compiled or executed yet.

R161 terminates with 159/192 passing profiles, failing 176.4 kHz/64 channels:
1701 contiguous zero frames (about 9.64 ms), no capture-position gaps,
discontinuities or timestamp errors. Cleanup succeeds. The test microphone's
publication-missing counter is zero; worker maximum lateness is 7.4649 ms,
drain 6.9979 ms and render 8.4666 ms. This is downstream of engine publication;
queue telemetry is needed before attributing it to FIFO starvation. The 192 kHz
sweep was not reached. Evidence: `format0414-full-exclusive-r161-report.json`.

After the matrix exits, the isolated R163 broker authentication fixture builds
with MSVC 19.44 under /W4 /WX and passes in 0.05 seconds. The actual process-owner,
identification/anonymous modes, wrong pipe end, byte-mode rejection, nested token
preservation and restoration cases run on the Dell. No service or production
pipe is installed. Evidence: `broker-identity-r163-tests.log` and its verification
manifest. Main application/installer integration remains open.

R169 adds optional transport-v2 queue diagnostics and builds driver 0.4.0.15.
The owner-process/generation checks and lock remain those of the private transport;
old v2 drivers may reject the new property without breaking audio. The 72-byte
snapshot exposes queued/ingress counts, cumulative missing/dropped frames,
primed underruns, last underrun time/queued/requested counts, recording frames,
lease-silent frames and its monotonic sample time. Startup/re-prime missing frames
are distinct from underruns after the FIFO reaches its prefill. The private COM
worker polls every 500 ms; graph/capture callbacks do not query the property.
Buffer capacities, prefill, clocks and scheduling are unchanged.

New deterministic tests cover startup, priming, shortage, re-prime, overflow and
lease expiry. Local ASan/UBSan and focused tests pass; all 45 Windows native tests
pass in 15.10 seconds. The strict kernel build has zero warnings/errors and
passes the universal API validator. R170 test-signs SYS/catalog using the existing
workbench certificate. Candidate SYS SHA256:
`66C9C86E7EC250ED1BF31956CA0A479F9ECA50E224855ACCC79533C0425B0DE9`.
The candidate is not installed; the Dell remains on 0.4.0.14. Deployment and a new
physical failing-format recording are the next steps. Evidence:
`driver0415-diagnostics-r169-verification.json` and `driver0415-sign-r170.json`.

R171 installs the signed 0.4.0.15 diagnostics package without reboot. The installed
SYS matches the signed candidate hash; saved routing bytes and all six physical
default roles are preserved. LoopZ MMDevice IDs regenerate again, so external-app
selection preservation remains a failure. The original microphone is now
`{0.0.1.00000000}.{81cbd352-d00e-4a2d-a522-1d570030973f}`.

R172's 30-second 176.4 kHz/64-channel recording fails with 3342 zero frames and
3342 publication-missing frames. Driver telemetry is available: underruns,
dropped frames and lease-silent frames remain zero, while the missing-frame
counter stays at its startup value 1857 throughout recording. Worker maximum
lateness is 1.419 ms and render 7.4066 ms; graph maximum lateness reaches
21.62324 ms. This run rules out recording-FIFO starvation as its gap source and
identifies missing engine publication. It does not retroactively explain R161's
different failure. Cleanup succeeds. Evidence:
`transport-0415-upgrade-r171.json` and
`format0415-single17664-queue-r172-report.json`.

R173 re-evaluates an opt-in graph-thread Pro Audio/HIGH scheduling class after
the DSP optimizations, keeping buffering and filters unchanged. Registration and
priority setting both succeed, but its 30-second 176.4 kHz/64-channel recording
fails with 4544 zero frames: 1598 publication-missing frames and a driver underrun.
Graph maximum lateness is 17.771025 ms; worker lateness reaches 13.8214 ms and
render 20.8046 ms. The experiment does not establish a robust improvement and is
rejected. Cleanup succeeds. Its source is archived and the exact baseline
runtime source restored. The baseline is rebuilt, passes the runtime regression,
and replaces the experiment in the workbench stage. Evidence:
`format0415-single17664-proaudio-r173-report.json` and
`proaudio-r173-verification.json`. Do not adopt priority escalation as the fix.

R176/R177 compare two exact-unity FFT arithmetic variants against a preserved
baseline executable in baseline/candidate/candidate/baseline order. An inner-loop
zero-root branch regresses wide unity-rate medians about 4%; peeling the zero-root
butterfly outside the loop gives no useful 64-channel gain (about 0.1% slower).
Both pass local FIR-parity regressions but are rejected on performance evidence;
the original FFT source is restored. These are isolated benchmark observations,
not physical continuity results. Raw JSONL runs and source snapshots are retained
in `fft-branch-r176-verification.json`, `fft-peeled-r177-verification.json` and
their referenced files.

R178 re-evaluates an opt-in 8 ms graph preparation lead after the ingress/history
optimizations, leaving source/output delays, prefill and DSP unchanged. A
30-second 176.4 kHz/64-channel recording passes with zero zero-frames,
publication misses and driver underruns; input-missing counters are stable.
R179's 192 kHz/64-channel follow-up fails with 614 zero frames, matching
publication misses, while input counters stay stable and driver underruns remain
zero. Both jobs clean up successfully. The lead is not adopted: baseline runtime
source is restored, rebuilt and staged; runtime, resampling and cadence regressions
all pass (three tests, 8.35 seconds). Cadence tests
now additionally exercise the non-block-aligned 8 ms frame counts at all six
rates, including 1411 frames at 176.4 kHz; the local test passes. Evidence:
`render-ahead-r178-verification.json` and R178/R179 physical reports.

## Remaining release gates

- Accept the installed .24/R560 pair across all 192 advertised exclusive-float
  rate/width combinations: R577 passes all 192 profiles and the independent
  native-flow audit; R563 passes five minutes at 192 kHz / 64 channels.
  R566 stops after 95 successful profiles on a report-file permission error;
  R577 resolves the fixture failure with a tested bounded report-replacement fix.
  Complete current PCM16 coverage, rate transitions and supported shared-mode
  client behavior separately; format initialization alone is insufficient.
- Verify continuous nonzero app-playback handoff, surviving descendants, churn,
  abrupt process/engine exit, receipt recovery and user playback overrides.
  R451 establishes fresh-history acquisition/release and silence-safe movement,
  not those broader lifecycle guarantees.
- Preserve external recording-app selections through actual driver upgrades, or
  finish and validate an explicit reselection experience where Windows replaces
  endpoint IDs. Both recent upgrades regenerated microphone IDs; same-test
  identity preservation is a separate, passing check.
- Establish production driver and user-mode signing/distribution. Installation
  must not silently weaken Windows security settings. Test-signing on the Dell
  is a user-authorized workbench configuration, not a release installation path.
- Validate clean Windows 10 22H2 x64 installation, update,
  repair, uninstall and UAC behavior with the actual signed package. Verify the
  per-user login option, saved graphs, audio defaults and interrupted recovery.
- Validate hardware microphones and simultaneous independent monitors, third-party
  recording/playback clients, unplug/replug and sleep/resume. Verify waveform
  continuity through rapid edits and device failures, beyond unchanged callbacks.
- Complete native-window visual/accessibility, DPI and locale QA. Idle Dell GPU
  readings are zero; interaction load and the reported high-GPU case still need
  reproduction on the affected machine.

Do not describe core tests, a successful build or process-capture activation
alone as a complete Windows audio-routing product or application-isolation proof.

### Publication readiness experiment R180–R181

The optional bounded publication wait remains disabled by default and is not
adopted. The 30-second 176.4 kHz/64-channel run passed, but the subsequent
192 kHz/64-channel run failed with 3,378 all-channel zero frames. The final
in-recording snapshot reported no publication-read misses, one driver FIFO
underrun, two wait timeouts and a maximum observed wait of 4.0806 ms.
These snapshots do not account for every zero frame through the end of capture.
Both runs restored saved state. Evidence is retained in
`artifacts/windows/publication-wait-r180-verification.json` and its referenced
recording reports. The two focused ASan/UBSan tests passed in 11.97 seconds.
Additional opt-in MonitorBus history counters are being prepared to classify
read failures; they have not yet been validated in the installed Windows build.

### Publication history diagnostics R182

The Windows output bus now enables the existing preallocated AudioHistory
read counters and exposes them under `microphone.publicationRead.history`.
Other platform callers retain disabled diagnostics by default. The added
regression verifies that diagnostic reads allocate no memory and report offline
reads only when enabled. The monitor ASan/UBSan test passes; the Dell build
and all 45 native tests pass (29.08 seconds). Source hashes match the Dell,
and the exact tested executables are archived there.

A fresh 30-second 192 kHz/64-channel recording with publication waiting disabled
fails with 2,994 consecutive all-channel zero frames. Both the final in-recording
snapshot and the post-probe snapshot report zero publication-history failures
across all six reason categories and all sample-lookup counters, while driver
diagnostics report one FIFO underrun. Driver missing frames rise from 3,536
at startup to 6,530: the 2,994-frame increase exactly matches the capture zeros.
Maximum worker render time is 13.1337 ms,
maximum worker lateness 11.7343 ms and maximum input drain 6.6294 ms. This run
redirects investigation toward worker rendering/delivery rather than missing
MonitorBus history; it does not establish that all earlier failures share this
cause. Cleanup succeeds and all acceptance processes exit. Evidence:
`artifacts/windows/publication-history-r182-verification.json`.

### Chunk delivery experiment R183 and combined follow-up R185

R183 sends each completed 256-frame render directly to the driver rather than
waiting for the full 10 ms batch. It is opt-in and preserves sample encoding,
filtering, configured delays and presentation timestamps. The focused monitor,
PCM16 and driver bridge tests pass. A 30-second 192 kHz/64-channel recording
fails with 3,072 zero frames matching publication missing frames. History
diagnostics identify unavailable resampling support and 3,484 unpublished source
sample reads; driver underruns stay zero. Cleanup succeeds. Chunk streaming
alone is therefore not adopted. Evidence: `streaming-r183-verification.json`.

R185 combines chunk streaming with the existing bounded readiness wait, applied
separately to each chunk. Its reserve uses observed individual chunk rendering
time rather than the full batch duration. Both experiment flags remain off by
default. The build and three focused Windows tests pass. The 30-second 192 kHz/64-channel
recording passes with zero captured zero frames, publication/history failures,
FIFO underruns or wait timeouts. There are 1,152 bounded waits, maximum 1.4186 ms;
maximum chunk render is 2.0918 ms. Cleanup succeeds, source hashes match the
Dell and tested binaries are archived. The 30-second 176.4 kHz/64-channel follow-up R186 also passes with zero captured
zero frames, publication misses or FIFO underruns and successful cleanup;
827 waits have no timeout, maximum 1.9595 ms. The combined change remains
unadopted pending broader validation. R187 starts the full 192-profile exclusive
PCM16 matrix with 1.5-second recordings per profile. See
`artifacts/windows/stream-ready-r185-verification.json` for the exact snapshot
and follow-up execution handle. R184 was prepared but never launched.

### Device operation extraction R188 (build pending)

`device_operations.hpp/.cpp` extracts five existing setup primitives: hardware
identity validation, installed package lookup, devnode started-state lookup,
pending format recording and hardware-validated removal. The elevated setup
helper now calls this module, which can also be linked by the planned broker.
All five bodies match the previously tested setup implementation after symbol
renaming and formatting. This is source-level equivalence, not a Windows build
or device-operation acceptance result. Account ownership authorization remains
a separate requirement for service callers. Driver staging, document handling
and whole-document pruning remain in the elevated helper.

The new target is not deployed to the Dell while R187 records. Build, link and
installed-helper regression checks are pending until that matrix is terminal.
No service or production named pipe is installed. Snapshot evidence:
`artifacts/windows/device-operations-r188-verification.json`.

### Owner-checked device upsert R189 (build pending)

The reusable device module now prepares per-mix root creation, owner recording,
naming, format persistence, binding and restart operations. The elevated helper
calls it with package-maintenance permission; routine service callers default
to refusing changes to an existing adapter's installed package. Matching but
stopped adapters use property restart rather than package reselection in routine
mode. Driver package staging remains outside this module. Existing adapters
must have an exact protected owner SID record before owner/name/format mutation;
unclaimed existing nodes fail closed. Only newly registered nodes may be claimed.
Owner text must be canonical, and format, bounded text, UTF-8 and package-name
preflight occur before device creation. The helper reserves rollback tracking
before creating any device.

The new `windows_device_operations_tests.cpp` fixture exercises pending-format
retry, acknowledged no-op, malformed format/pending recovery, invalid-format
nonmutation and invalid-request preflight. It uses an isolated HKCU registry
key and must not create or modify devices. The test has been written but not
compiled or executed; R187 is still using its frozen tested binaries. The
original five-function equivalence check in R188 describes its earlier snapshot;
R189 intentionally adds format validation and account/package checks.

Windows build/link, test execution, installed-helper no-op/create/format/removal
checks and actual LocalSystem endpoint finalization remain pending. This does
not install a service or complete broker integration. Snapshot evidence:
`artifacts/windows/device-upsert-r189-verification.json`.

### Per-mix finalization and removal R190 (build pending)

The device adapter adds explicit `removeMixDevice` and `finalizeMixDevice`
operations. Removal derives one root identity from authenticated owner plus
mix ID, checks LoopZ hardware and the exact protected owner record, closes the
registry handle before removal, and treats an already absent node as success.
Finalization checks ownership, the stored requested format and started state
before refreshing the two public endpoint formats and acknowledging pending
work. The elevated helper now uses that finalization path. Routine service
operations still require the shared setup lock and durable desired-state
reconciliation; neither service nor IPC integration exists yet.

The isolated fixture now also covers missing/wrong/truncated/wrong-type owner
records, noncanonical SID rejection, invalid explicit removal preflight and
malformed UTF-8/embedded-NUL names. It remains unexecuted. Its prepared build
script refuses to run while LoopZ is active, links the frozen capture library
and does not modify devices. Both module and helper need actual Windows
compile/link and installed regression validation after R187 exits. Evidence:
`artifacts/windows/device-adapter-r190-verification.json`.

### R187 full-matrix terminal result

The combined readiness/chunk experiment fails its full exclusive PCM16 matrix
after 60 passing profiles, at 44.1 kHz/58 channels. Capture reports 9,663 zero
frames. The final runtime reports the private worker stopped after missing its
deadline by more than 30 ms. The last in-recording snapshot predates the stop
and must not be used to rule out later FIFO starvation. Cleanup succeeds and
the test engine exits. The two earlier focused high-rate passes do not establish
a general continuity fix, and both experiment flags remain disabled by default.
Evidence: `format0415-full-ready-r187-report.json`, referenced by the terminal
`stream-ready-r185-verification.json`.

### Owner discovery for setup recovery R191

The portable SQLite database adds `ownersWithDesiredState()` for startup
reconciliation without waiting for a client login. It includes explicit removal
tombstones and excludes owners with only retained retry receipts. Absence never
authorizes deleting a device. The regression verifies discovery after reopening,
stale removal acknowledgments, completed deletion and preserved owner revisions.
The ASan/UBSan database test passes in 1.00 second. The Windows adapter validates
each stored owner as a canonical SID, but that wrapper has not yet been rebuilt
on Windows. Evidence: `owner-recovery-r191-verification.json`.

R190's isolated MSVC device-operation fixture passes in 0.06 seconds. The main
Windows helper and new test executable compile/link with the extracted module.
The Dell CMake change is a focused patch of its existing build; its exact source
is retained as `windows-cmake-adapter-r190.txt`, rather than claiming the newer
local SQLite/broker target definitions have already been integrated there. R192
runs the full native suite, then installed owner-scoped reconciliation acceptance
only if that suite passes. See the device-adapter manifest for pending live status.

### R192 installed helper regression passed

The rebuilt helper links the new adapter module; all 46 Windows native tests
pass in 27.56 seconds. Installed reconciliation acceptance then passes all six
checks: concurrent setup rejection without removal, deleted-root cleanup,
foreign-owner preservation, idempotent repeated repair, explicit empty-owner
cleanup and unchanged saved mixes/endpoint IDs/system defaults. Cleanup succeeds
and all acceptance processes exit. Source hashes match the deployed snapshot,
including its focused CMake patch. The exact helper is archived on the Dell as
`driver-setup-adapter-r192.exe`; its hash is retained with the report.

This verifies elevated-helper integration and owner-scoped reconciliation, not
the future SCM service, LocalSystem finalization, the new direct removal API's
installed path or a completed Windows port. Evidence:
`artifacts/windows/device-adapter-r190-verification.json` and
`device-adapter-r192-reconcile-report.json`.

### R193 Windows owner-recovery validation passed

The updated SQLite database and protected Windows store compile in an isolated
MSVC fixture. Both shared intent/database tests pass (7.48 seconds total), then
the administrative store fixture passes owner discovery through competing
connections and reopening, pending removal discovery, stale acknowledgment
rejection and receipt-only owner exclusion. Restricted-token access denial,
protected directory replacement resistance, SID/format rejection, hard-link
rejection and insecure directory checks still pass. The exact test directory
is confirmed removed; no production database is provisioned. All ten source
and pinned SQLite file hashes match the local snapshot.

Evidence: `artifacts/windows/owner-recovery-r193-verification.json`. These tests
validate isolated storage behavior, not service startup reconciliation or
LocalSystem device finalization. The next integration check must run the device
adapter under actual LocalSystem/Session 0 with uniquely scoped temporary roots.

### R194 actual LocalSystem/Session 0 adapter acceptance passed

The opt-in `windows_system_device_acceptance.cpp` fixture builds under MSVC
/W4 /WX and runs from a protected, unique ProgramData folder through a temporary
LocalSystem scheduled task. It verifies its process SID is S-1-5-18 and session
is 0 before device operations. Under the shared setup mutex it creates and
finalizes a unique 48 kHz/stereo adapter, renames and changes it to 96 kHz/eight
channels, repeats the same setup without package maintenance, then explicitly
removes it and verifies repeated removal is idempotent. The fixture passes
against the installed 0.4.0.15 package (oem45.inf).

The external wrapper verifies the saved routing file hash, all defaults and
existing LoopZ endpoint IDs are unchanged. The scheduled task exits with result
0, is unregistered, and its protected test folder is removed. The exact tested
executable remains archived on the Dell as `system-device-r194-tested.exe`;
its source and binary hashes match the recorded snapshot. Evidence:
`artifacts/windows/system-device-r194-preparation.json`,
`system-device-r194-report.json`, `system-device-r194-verification.json`.

This proves the tested adapter operations work under actual LocalSystem/Session
0. It does not install an SCM service, authenticate production IPC, validate
standard-user engine integration or prove format compatibility beyond these
profiles. The full audio format matrix remains failed (R187). The optional
CMake fixture target is off by default and is never registered with CTest.

### R195 bounded broker transport fixture passed

`broker_pipe.hpp/.cpp` adds a local message-mode server transport for the future
SCM broker. The production pipe name requires a LocalSystem process and uses
first-instance exclusivity, remote-client rejection and an authenticated-user
access mask excluding FILE_CREATE_PIPE_INSTANCE. The server handle remains
open between requests. A complete request is capped at 64 KiB before deriving
its owner through the tested token helper. Reads, writes and reply acknowledgments
have five-second limits and cancel through the service stop event. Every pending
OVERLAPPED operation is drained before its storage/event can be destroyed.
A bounded acknowledgment replaces unbounded FlushFileBuffers, preventing the
next disconnect from discarding a legitimate unread reply. Clients must request
the explicit access mask and send ACK byte 6 after reading their complete reply.

The isolated MSVC /W4 /WX fixture passes in 5.05 seconds: authenticated owner,
first-instance exclusivity, anonymous rejection, exact 64 KiB acceptance,
oversized rejection, reuse after rejection, actual idle-client timeout, idle
and connected shutdown, and shutdown while a client withholds acknowledgment.
The test caught and fixed an early-client race: an unconditional pre-accept
disconnect could drop a client that connected before ConnectNamedPipe. Source
hashes match the Dell; the tested executable is archived there. Evidence:
`artifacts/windows/broker-pipe-r195-verification.json`.

The fixture uses unique test names and grants its creator full access. This is
not proof of production authenticated-user access policy under a LocalSystem
server. No production pipe, SCM service or durable request handler is installed;
production client verification of the server identity also remains required.
The new main-build target definitions are prepared locally, while this Windows
validation uses the isolated fixture and frozen capture library.

### R196 durable broker handler passed isolated Windows checks

`broker_handler.hpp/.cpp` connects a token-owned BrokerMessage to protected
SQLite acceptance. Changes return `acceptedRevision` only after commit and
request a reconciliation wake even for an old idempotent retry; the worker must
read the latest durable snapshot, never apply the incoming retry directly. A
strict bounded status query returns the authenticated owner's revision and
desired-record count without changing state or waking reconciliation. Replies
describe durable acceptance rather than claiming device readiness.

`runBrokerRequests` owns the pipe loop, signals the separate reconciliation
event after acceptance, and sends the bounded reply. Lost replies do not undo
committed desired state. It compiles but has not yet been exercised as an
installed service or combined pipe-to-store integration test.

The extended protected-store fixture directly exercises the handler with
fixture-supplied owner metadata: initial status, commit-before-success, retry
through another connection, stale CAS rejection, forged owner rejection, duplicate
and unknown status-field rejection, unchanged foreign-owner records and explicit
removal persistence/acknowledgment. All checks pass under MSVC /W4 /WX. The test
folder is confirmed removed; source hashes match and the exact test executable
is archived on the Dell. Evidence:
`artifacts/windows/broker-handler-r196-verification.json`.

No production pipe or SCM service is installed. Production server verification,
LocalSystem authenticated-user access policy, coalescing PnP worker, installed
startup/recovery and standard-user engine/installer integration remain required.

### R197 complete pipe-to-SQLite request loop passed

The opt-in `windows_broker_loop_acceptance.cpp` administrative fixture executes
the actual BrokerPipe authentication, runBrokerRequests loop, durable handler
and protected SQLite store together. It observes the reconciliation event after
commit, then closes the client without reading the reply. Retrying the same
operation returns revision 1 rather than committing again. Stale and forged-owner
requests fail without signaling reconciliation. An explicit removal is accepted
at revision 2 and remains a tombstone when all server connections close and the
store reopens. The token owner's records are present; the forged System owner's
revision stays zero. The stop event cancels the listener and the worker joins.

The MSVC /W4 /WX build and integration fixture pass. Source hashes match, the
exact executable is archived on the Dell, and no LoopZBrokerLoopTest directory
remains. No audio devices or saved routing files are changed. Evidence:
`artifacts/windows/broker-loop-r197-verification.json`.

This uses a unique fixture pipe in an administrative process. It does not prove
production authenticated-user access under LocalSystem, server verification by
a standard-user client, installed SCM behavior or PnP worker integration.

### R198 latest-state reconciliation worker and shared setup lock

`BrokerReconciler` reads protected desired state outside device operations,
checks each record again before applying and after completion, and revision-tags
its progress. Superseded work is queued again rather than reported ready.
Removal acknowledgment uses the exact stored revision; reboot-required removal
keeps its tombstone. Completed revisions are cached to avoid repeatedly applying
unchanged successful work; device/maintenance notifications can invalidate that
cache. Failures request another pass while other owners continue independently.
The service host still must provide the background loop, events, retry timing
and revision-aware status presentation.

`WindowsBrokerDeviceAdapter` maps these records to the tested upsert, finalize
and explicit removal primitives. It acquires the extracted DeviceSetupLock,
shared with elevated maintenance. The lock constructor preserves the old
security descriptor, global name and nonblocking acquisition behavior. The main
helper builds with it and two focused native checks pass in 0.07 seconds; that
new helper is archived but not copied over the staged R192 helper.

The MSVC fixture uses actual protected SQLite connections and simulated device
operations. It passes newer edits arriving during apply, undo during removal,
retry after failure, one owner's failure alongside another owner's successful
update, reboot tombstone retention, cache invalidation, stop and cross-thread
lock contention. Sources match the Dell, the binary is archived and no test
directory remains. This does not prove the concrete adapter through this worker
under LocalSystem or an installed service. Evidence:
`artifacts/windows/broker-reconcile-r198-verification.json`.

### R199/R200 concrete worker under LocalSystem passed

The system-device fixture adds an explicit `--worker` mode that uses a protected
SQLite store, BrokerReconciler and WindowsBrokerDeviceAdapter together. Under
actual SID S-1-5-18/Session 0 it persists and applies a unique 48 kHz/stereo
intent, skips unchanged completed work, constructs a fresh worker that recovers
desired state, persists/applies a 96 kHz/eight-channel edit, then persists/removes
the adapter and acknowledges the exact removal revision. Both typed endpoint
roles are checked, and the test mix's Feed/microphone IDs survive worker recovery
and the format edit. This is separate from the still-failing driver-upgrade
endpoint-ID preservation gate.

R199's worker task exits successfully, but its observer waits incorrectly on a
wall-clock comparison. After confirming task state Ready, exit result 0 and the
protected success/cleanup report, only the observer is stopped and the completed
task/folder are removed. R200 repeats with a new unique root and corrected
observation: compare LastRunTime with its initial value or accept a report from
this unique directory, then require the task to be terminal. It passes fully,
including unchanged saved document, defaults and existing endpoint IDs. The
temporary task and folder are gone. Sources and tested binary hashes match.
Evidence: `artifacts/windows/system-worker-r200-evidence.json`.

The fixture is opt-in and never part of CTest. It runs through a temporary
scheduled LocalSystem task, not an installed SCM service, and does not validate
standard-user production pipe access or app integration.

### R201 threaded broker runtime passed Windows fixture

`runBrokerRuntime` binds its exclusive server pipe before starting device work,
then runs requests and reconciliation on separate owners with separate protected
SQLite connections. Startup scans saved desired state. The manual-reset change
event is reset before each pass so a commit arriving during device work remains
observable; failures retry after five seconds. Every exit signals stop and joins
the worker before stores, events, callbacks or adapter references can expire.

Status requests may include one bounded `mixId`. Replies use the current desired
record's revision and show worker progress only if owner, mix and revision match;
older applying/ready progress leaves the newer edit queued. No saved intent is
reported as `unrequested`, which does not assert physical absence or authorize
cleanup. Reply serialization replaces invalid diagnostic UTF-8.

The MSVC /W4 /WX runtime fixture uses actual pipes and protected SQLite with
simulated device operations. It passes startup recovery, responsive status/edits
while an operation is blocked, an edit arriving during apply, revision-aware
readiness, old idempotent retry without old PnP, removal acknowledgment, exclusive
pipe binding before work and shutdown waiting for an in-flight worker. The test
directory is removed, source hashes match, and the tested executable is archived.
Evidence: `artifacts/windows/broker-runtime-r201-verification.json`.

This is not an installed SCM host. Protected service configuration, production
LocalSystem pipe policy/server verification, installer and standard-user app
integration remain open. Interrupted creation before the protected owner record
is written also needs explicit recovery coverage; the current adapter fails
closed on an existing unclaimed root rather than claiming it without proof.

### R202 protected service installation parameters passed

`broker_installation.hpp/.cpp` defines the fixed SCM service identity and reads
its `DriverInf` value from the 64-bit HKLM service Parameters key. Only an
`oem` plus decimal digits plus `.inf` basename is accepted; paths, commands,
embedded NULs and oversized/wrong-type data fail closed. The installer-only
provisioner requires the service key to exist, creates Parameters with protected
SYSTEM/Administrators ownership and full access, validates existing security
before writing, and flushes the single registry value. It does not stage a driver
or accept installation paths through IPC. Portable per-owner device intent
remains in SQLite; this metadata describes Windows service installation only.

The isolated MSVC /W4 /WX fixture passes valid updates, rejected writes preserving
the previous value, restricted-token read and write denial, insecure-key rejection
before mutation and malformed value rejection. Win32 normalizes some missing
string terminators, so the fixture uses invalid content rather than assuming
malformed raw bytes survive RegSetValueExW. Generated fixture leaf keys are
removed, sources match the Dell and the exact binary is archived. Evidence:
`artifacts/windows/broker-installation-r202-verification.json`.

Only isolated HKCU keys are exercised. Fixed HKLM provisioning/read paths compile
but await an installed service. No SCM service or real Parameters key is created
or changed. Protected executable installation checks and SCM/installer wiring
remain required before running this as a persistent privileged service.

### R203 SCM host compiled; installation still pending

`broker_service.cpp` adds the actual Windows service dispatcher and control
handler. It requires LocalSystem/Session 0, reads protected installer parameters
and runs the threaded broker with the concrete device adapter. It reports
RUNNING only after pipe binding and worker initialization. Stop/shutdown signal
the runtime; pending-status heartbeats continue while device work drains, and
STOPPED is reported after worker/heartbeat threads join. There are no command-line
path, package or owner overrides. Runtime failures report service-specific errors
and write an Application event.

`BrokerBinaryGuard` requires the exact executable under the fixed local Program
Files/LoopZDeviceBroker directory. It validates the directory and executable
against the existing strict SYSTEM/Administrators file policy and retains handles
against rename through shutdown. The protected-file checks were extracted into
`broker_security.hpp` without changing their prior behavior. This guard does not
replace installer payload/signature validation or safe DLL/file provisioning.

The MSVC /W4 /WX host build, updated threaded runtime fixture, protected-store
fixture, build-directory rejection and standalone console refusal pass. The
console launch exits 1 with the expected service-controller connection error;
no service is registered. Sources match the Dell, the built executable is archived
and test directories are gone. Evidence:
`artifacts/windows/broker-service-r203-verification.json`.

The SCM lifecycle itself and positive installed binary guard have not run.
Protected installation, production pipe/client verification, service start/stop
and recovery, app wiring, interrupted-creation recovery and power/device
notification invalidation remain required.

### Protected service deployment — R204

The installer-only adapter verifies a trusted SHA-256 before destination mutation,
creates a protected temporary file, verifies copied bytes, and replaces the
service executable. Failed creation cleans up only a file created by that operation.
An isolated MSVC Release fixture passed on the Windows 10 Dell: bad digest before
mutation, first copy, identical-content no-op, locked replacement retaining old
bytes and cleaning staging, unexpected DLL refusal, successful replacement, and
cleanup. Harmless text payloads were never executed. Source and tested binary
hashes: `artifacts/windows/broker-deploy-r204-verification.json`. No service was
registered. Actual SCM lifecycle, installed binary guard, restricted-token
deployment, and trusted package metadata integration remain unfinished.

### Protected deployment permissions and real service lifecycle — R205/R206

R205 extended the deployment fixture on the Windows 10 Dell: an administrator-
restricted token was denied independent READ, WRITE, DELETE, and WRITE_DAC access
to the protected executable. A hard-linked installed payload was rejected.
Source and tested binary provenance: `artifacts/windows/broker-deploy-r205-verification.json`.

R206 exercised the actual SCM host from its fixed protected Program Files path,
using the fixed HKLM installation metadata and an empty protected ProgramData
store. The LocalSystem service reached RUNNING in Session 0, stopped with process
exit, and repeated the start/stop cycle. This provides positive installed-path
guard coverage in addition to the earlier build-directory rejection. The fixture
refused pre-existing production paths/services; afterwards SCM service, service
registry, binary directory and test-created store were absent. The saved routing
document retained SHA-256
`99D9104E08F9DA9385A603E50E6E0007A342F75196D73A860F38620840C1F55D`.

Evidence: `artifacts/windows/broker-service-r206-verification.json` and
`artifacts/windows/broker-service-r206-run.log`; exact host and acceptance binaries
are archived on the Dell. This test did not apply a device intent, exercise a
standard-user request through the production pipe, or validate app integration.
Trusted release payload metadata, install/update/repair integration, device/power
recovery and full audio acceptance remain incomplete. No service remains installed.

### Verified production-pipe client and access policies — R207–R210

The new control-thread client uses the fixed local pipe, checks the kernel-reported
server PID against the running SCM service, validates its LocalSystem configuration
and exact installed executable path, and retains a process handle during the call.
It sends no request before those checks pass. Requests/replies are bounded to
64 KiB, use a five-second operation deadline and cancellation event, and acknowledge
complete replies. Sent requests are not automatically retried.

R207/R208 exposed that the default System process DACL prevents a client without
administrator rights from inspecting the server. The service now grants only
PROCESS_QUERY_LIMITED_INFORMATION to authenticated users. R209 advanced beyond
process inspection but failed service access. Installer-only service-access
provisioning now explicitly grants authenticated users query configuration/status
and READ_CONTROL; control and configuration changes remain System/administrator
operations. The policy uses the Windows service access rights documented at
https://learn.microsoft.com/en-us/windows/win32/services/service-security-and-access-rights.

R210 passed on the Windows 10 Dell with an administrator-disabled, privilege-reduced
token: verified production-pipe status, forged-owner rejection, pre-cancellation,
plus denial of process termination/memory write/handle duplication and service
configuration/stop/DACL/deletion access. Both actual SCM start/stop cycles passed.
A fixture process binding the fixed pipe after service cleanup was rejected by
the client. No device intent was applied, the saved document hash stayed unchanged,
and service, registry, binary and store directories were removed. Evidence:
`artifacts/windows/broker-service-r210-verification.json` and corresponding run log.
Prior failed runs and their binaries remain retained as R207–R209 evidence.

This is restricted-token acceptance, not a separate standard-user desktop test.
Client in-flight cancellation, timeout/oversize replies, app integration and
installer integration remain unverified or unfinished.

### Complete client → service → driver acceptance — R211

The actual protected LocalSystem SCM host on the Windows 10 Dell now passes a
request-driven device lifecycle through the fixed production pipe. A client with
administrator membership disabled and privileges reduced submitted a unique test
mix at 48 kHz/stereo, received durable revision 1, and retried the same operation
without advancing it. The service published one Feed and one microphone. Direct
protected-store inspection confirmed the record belongs to the client SID; the
System owner had no desired records.

The service stopped and started again with that durable record intact. A fresh
worker recovered Ready at revision 1 and both physical endpoint IDs were identical.
The restricted client then submitted explicit removal at revision 2. Status became
unrequested after tombstone acknowledgement; independent endpoint enumeration
confirmed both test endpoints absent. All six Windows default roles, the existing
LoopZ endpoint role map, and the saved routing document were unchanged. Scoped
cleanup also removed the temporary service, configuration, protected binary and
test-created store. The fixture preflights absence of those paths and the unique
root device; cleanup is restricted to its exact owner/mix identity.

Evidence and tested-binary/source hashes are retained in
`artifacts/windows/broker-service-r211-verification.json` and corresponding run log;
exact host and acceptance binaries remain archived on the Dell. This does not prove
audio delivery, production-pipe format edits, a separate standard-user desktop
session, crash/power recovery, or automatic app/installer integration. Those and
the broader Windows port requirements remain unfinished.

### Production-pipe rename, format edit and second recovery — R212/R213

R213 extends the actual SCM/driver fixture to three service lifecycles. The
restricted client creates 48 kHz/stereo at revision 1, then after recovery renames
the adapter and requests 96 kHz/eight channels at revision 2. A distinct operation
using stale revision 1 is rejected. Both endpoints retain their IDs and their
IAudioClient::GetMixFormat results report the requested rate/channel count.
The PnP adapter friendly name matches the latest accepted name with the existing
`LoopZ — ` prefix. Protected SQLite inspection confirms the latest name/format.

A second stop/start recovers revision 2 with the same endpoint IDs, public
formats and PnP name. Explicit removal at revision 3 removes both endpoints and
acknowledges the tombstone. Default roles, existing LoopZ endpoints and saved
routing document are unchanged; temporary service, registry, binary and store
are removed. R212 stopped on an incorrect fixture expectation that omitted the
existing adapter name prefix; the expectation was corrected without weakening
the name or format checks.

Evidence: `artifacts/windows/broker-service-r213-verification.json` and run log.
MSVC Release source/binary hashes and exact binaries are retained. These checks
inspect public formats but do not establish audio delivery, continuity or
latency. Crash/power/device recovery, app integration and installer integration
remain unfinished, along with the other full Windows acceptance requirements.

### Build-time embedded service payload — R214

The Windows CMake integration now attaches the exact broker binary and SHA-256
as RCDATA resources to the maintenance helper. A generator first snapshots the
built broker, hashes that snapshot, and emits a resource script. Resource generation
depends on the broker executable itself, so signing or changing it requires resource
regeneration. The runtime reader reads only the current executable, with bounded
size and strict digest encoding; it accepts no external manifest or module override.
Release trust must come from signing the maintenance helper after payload embedding;
any desired broker signing must happen before the final helper resource build.

An isolated MSVC Release Windows 10 fixture loaded the actual archived R213 service
bytes and verified their digest independently using BCrypt and Get-FileHash. Missing
resources were rejected. Appending one byte to an isolated, never-executed host copy
caused Ninja to regenerate the resource, recompile it, and relink the consumer; the
updated resource passed independent verification. Initial and updated tested binaries
and source hashes are retained. The reusable CMake positive-test runner also passed.
Local checks covered output paths with spaces and rejection of a missing input.
Evidence: `artifacts/windows/broker-payload-r214-verification.json` and its referenced
initial/updated reports. No real service installation occurred in this fixture.

The main maintenance helper has not yet been rebuilt with this integration on the
Dell. Protected extraction, service install/update/repair/uninstall commands, app
integration and the complete signing pipeline remain unfinished.

### Direct protected deployment from embedded bytes — R215

The installer deployment adapter now accepts build-time resource bytes directly.
It verifies bounded nonempty data against the embedded SHA-256 before destination
mutation, then uses the existing protected publication path. Directory/file policy,
staging ownership, copied-file digest validation, and atomic replacement sections
were mechanically confirmed identical to the preceding file-based implementation.
The existing file-source entry point uses that same publication path. The production
`stageEmbeddedBroker()` entry reads only current-EXE resources and uses only the
fixed protected destination; no temporary user-writable extraction is needed.

MSVC Release fixtures passed on the Windows 10 Dell. Memory-source tests cover
bad hash/empty data before mutation, first copy/no-op/replacement and restricted
access denial, alongside the preceding file-source regression cases. The actual
SCM acceptance fixture now installs its bundled service through this entry point,
checks the bundled digest against the independently built host, then repeats the
three-lifecycle creation/rename/format/recovery/removal acceptance from R213.
Both public formats, endpoint IDs, desired-state ownership, defaults and original
endpoints pass their checks. Temporary service/configuration/binary/store and test
folders were removed; the saved routing document retained its pinned hash.

Evidence: `artifacts/windows/broker-deploy-r215-verification.json`,
`artifacts/windows/broker-service-r215-verification.json` and corresponding run log.
The exact acceptance and host binaries remain archived on the Dell. The native
maintenance helper's install/update/repair/uninstall command flow, full helper
build/package integration, app integration and audio continuity remain unfinished.

### Native service maintenance API and automatic crash restart — R216/R217

The new elevated maintenance module installs/repairs the fixed service from its
current-EXE payload, provisions protected state/configuration and read-only client
access, sets automatic startup with AudioEndpointBuilder/Audiosrv dependencies,
and configures restart actions. It validates existing executable/account/type
before mutation. Stop waits for process exit; unregister is idempotent and retains
files/intent for the encompassing driver uninstaller to remove only after physical
device cleanup. Invalid package receipts are rejected before mutation.

R216 exposed self-contention: maintenance held the PnP mutex through service
startup, causing recovery to report a busy setup. R217 fixes this with a separate
maintenance mutex and releases the PnP mutex before starting the service. Stop
cooperatively joins service work before installation changes. The existing
strict device lock still protects publication/configuration changes.

The actual Windows 10 Dell fixture passes fresh install, invalid receipt and
foreign registration rejection, repair with durable state, and live repair.
After Ready at revision 2, the fixture terminates only the verified temporary
service PID; SCM automatically starts another process, which recovers Ready with
the same endpoint IDs and public 96 kHz/eight-channel format. The previous
restricted-client creation, rename, stale rejection and removal checks still pass.
Repeated unregister/absent-stop succeeds; temporary service/registry/files/store
are absent, and original defaults/endpoints/saved routing document are unchanged.

Evidence: `artifacts/windows/broker-service-r217-verification.json` and run log;
failed R216 and exact tested binaries/source snapshots are retained. The crash
was after Ready, not during creation or active PnP work. Main-helper CLI and
installer wiring, full update failure rollback, file/data purge ordering, app
integration, interrupted-creation/power/device recovery and audio acceptance
remain unfinished. No service remains installed.

### Native helper install-service command — R218

The actual elevated driver helper now supports `--install-service` with no caller
package/owner/digest/path parameters. It stages its adjacent signed driver package
through the existing Windows verification path, records the published INF receipt,
releases the driver lock, and installs/repairs the service from its current-EXE
resources. The existing sync/reconcile path reuses the extracted package-staging
function; its staging operations were mechanically compared unchanged.

An isolated MSVC Release build of the real helper (including its administrator
manifest and embedded broker) passed two actual command invocations on the Dell.
Both fresh installation and repair reached Running, selected existing `oem45.inf`,
and installed a service binary whose SHA-256 matches the embedded build. The
fixture used the unchanged test-signed 0.4.0.15 package and restored its receipt
registry entry afterward. All six defaults, original root device IDs/status and
saved routing document remained unchanged. Temporary service, binary directory
and protected store were removed. Tested helper/source hashes are retained in
`artifacts/windows/broker-helper-r218-verification.json`; the exact helper is
archived on the Dell as `broker-helper-r218-helper-tested.exe`.

The isolated fixture links the existing portable/DSP libraries with snapshotted
maintenance code. Full main-tree Windows rebuild/package integration is still
pending. Legacy sync/reconcile commands were compiled but not exercised in this
run. Coordinated uninstall and owned-file/data purge must be completed before
wiring automatic service installation into Inno setup. App integration, production
signing and the remaining full Windows acceptance scope are unfinished.

### Native helper service controls and uninstall ordering — R219

The real elevated helper now exposes idempotent `--stop-service` and
`--unregister-service` commands. Its full `--uninstall` path stops the broker
before taking the driver-operation lock and removing physical devices, then
unregisters the broker only after driver/package removal succeeds. A driver
removal failure therefore retains the registration and recovery data for retry,
with the service stopped. Protected binary/store purge is not yet integrated.

The rebuilt MSVC Release helper passed actual install, repair, repeated stop and
repeated unregister commands on the Dell with the existing test-signed package.
The fixture restored the package receipt and independently verified unchanged
saved routing, six default roles and original driver root identities/status.
The wrapper removed the temporary protected binary/store after native unregister.
No service or temporary installation remains. Tested source/binary provenance:
`artifacts/windows/broker-helper-r219-verification.json`; exact helper archived
on the Dell as `broker-helper-r219-helper-tested.exe`.

The combined full-driver `--uninstall` branch was compiled but not executed
against the Dell's original microphones in this run. End-to-end uninstall,
guarded owned-file/data purge, Inno setup wiring, main-tree build/package
integration, app integration and broader audio acceptance remain unfinished.

### Guarded native service cleanup — R220

The helper now supports `--purge-service`. It serializes against service and driver
maintenance, requires an absent SCM registration, preflights both fixed protected
directories, rejects unknown entries/reparse points/hard links/insecure ownership
or ACLs, and checks every durable desired device for physical absence. It retains
handles, verifies file identity when acquiring delete access, then deletes through
those validated handles. Known binary names are the service executable and canonical
GUID pending payloads; the store currently permits only its primary database.
The non-reboot full-uninstall path calls cleanup after successful driver removal
and service unregistration. Reboot-required removal retains protected files/state
for later cleanup; scheduling that cleanup is unfinished.

The actual MSVC Release helper on the Windows 10 Dell passed rejection while
registered, rejection of a durable reference to the existing original microphone,
unknown-DLL rejection, hard-link rejection, native directory removal and idempotent
repeat. The opt-in reference fixture wrote only its own temporary store record;
it never operated the original microphone and explicitly validated that exact
fixture record before teardown. The positive purge used the empty store directory
after that teardown; positive deletion of a populated database whose referenced
devices are absent still needs coverage. Original root identities/status, defaults
and saved routing document remained unchanged; the package receipt was restored.

Evidence/source/helper hashes: `artifacts/windows/broker-helper-r220-verification.json`.
Exact helper and fixture snapshots remain on the Dell. No service or temporary
protected directories remain. The combined driver-uninstall path, hot-journal
cleanup, reboot-deferred cleanup, Inno and app integration, signing and broader
audio acceptance are still incomplete. Unknown journal files currently cause
cleanup to fail closed rather than deleting unvalidated recovery state.

### Populated-store cleanup and setup service hook — R221

Cleanup now has positive Dell coverage with a populated protected database whose
unique desired device was independently confirmed absent. The native helper
removed the database and binary directories, repeated cleanup succeeded, and all
registered-service, existing-microphone-reference, unknown-file and hard-link
refusal checks still passed. Defaults, root identities/status, saved document and
package receipt were preserved. Evidence:
`artifacts/windows/broker-helper-r221-verification.json`. This closes R220's
positive populated-database deletion coverage gap.

Inno setup now runs the tested `--install-service` command after extracting files.
It checks both process execution and exit status, reports failure, returns a
nonzero setup exit code and gates the optional app launch on successful service
setup. Production signature/catalog gates were not changed.

The official signed Inno 6.7.3 compiler was installed in the Dell build-tools
folder after publisher verification; provenance is retained in
`artifacts/windows/inno-6.7.3-provenance.json`. An isolated unsigned development
harness compiled the exact production Code and Run sections with an innocuous
helper stub. Helper exit 0 produced setup exit 0; helper exit 42 produced setup
exit 1 and the expected setup failure message. Evidence:
`artifacts/windows/installer-flow-r221-verification.json`. The harness uses a
unique fixture identity/path, writes no uninstall registration, and never operates
services or drivers. Exact installers, logs and stubs remain in the Dell fixture.

This is code-flow acceptance, not a signed production installer build. Interactive
launch-checkbox behavior, combined installer/real-helper acceptance, hot-journal
and reboot-required cleanup, main build/package integration, app integration and
broader audio/Windows compatibility acceptance remain unfinished.

### Owner-scoped desired-state readback for engine integration — R222/R223

The broker now handles read-only `action: snapshot` queries. An initial request
contains version/action; continuations include offset, revision and a 64-character
content tag. Replies return revision, tag, records and nextOffset (null when done).
Pages stay below 60,000 bytes including a conservative envelope budget, even when
maximum-size metadata needs JSON escaping. The owner comes exclusively from the
pipe token; reads neither mutate desired state nor wake PnP work.

Owner revision alone is insufficient for paging: successful removal acknowledges
a tombstone without advancing that revision. The snapshot tag therefore hashes
the complete revision/record set. An edit or tombstone acknowledgement invalidates
a continuation with `snapshot_changed`. The client has a bounded whole-snapshot
reader with restart/cancellation checks, strict reply fields, duplicate/depth
validation, monotonic record ordering and bounded record/cursor counts.

The protected-store Windows fixture passed worst-case escaped pagination, complete
continuation, concurrent-edit invalidation, same-revision tombstone invalidation,
owner isolation and malformed requests. R222 reached those checks but failed an
existing recovery assertion because the synthetic pagination record remained;
R223 explicitly removes/acknowledges its synthetic record before recovery checks.
The failed source/binary/database snapshot is retained and its temporary ProgramData
folder was removed. The corrected store fixture passed and cleaned its folder.

The actual LocalSystem SCM fixture also passed the new reader from an administrator-
disabled client and returned the correct desired metadata. Prior creation, format
change, live repair, crash restart, removal and preservation checks still pass.
Evidence: `artifacts/windows/broker-service-r223-verification.json` and
`artifacts/windows/broker-service-r223-store-verification.json`. Exact binaries
and source snapshots remain on the Dell. Engine Runtime integration and actual
production-pipe multi-page client retry coverage remain unfinished; this adds no
audio continuity, latency or full-platform acceptance claim.

### Portable saved-document intent planning — R224

The shared C++ intent library now provides `nextSavedDeviceIntent`. It compares
a complete authoritative saved document against an authenticated owner snapshot
and returns one explicit CAS operation. Missing/unreadable/unsaved documents,
represented by authoritative=false, never produce mutations. The complete desired
document is validated before selecting a removal. Obsolete live records are
removed first to free quota; pending removals are not duplicated, and undo
supersedes a pending removal with an upsert. Matching metadata produces no request
even if the previous acceptance reply was lost. Disabled routing keeps its public
microphone definition. Each emitted operation requires a fresh caller operation ID
and the current owner revision; callers must refresh the snapshot before the next.

The planner tests pass in the core-only Release build on macOS and in an isolated
MSVC Release `/W4 /WX /UNDEBUG` fixture on the Windows 10 Dell. Related portable
intent/database tests also pass (three focused tests total). Cases cover lost
replies, explicit removal, undo, field edits, live-quota replacement ordering,
invalid replacement metadata, malformed snapshots and exhausted revisions.
Provenance: `artifacts/windows/planner-r224-verification.json`; exact Windows
tested binary and source snapshot are archived on the Dell. The Windows fixture
uses the existing read-only core/DSP libraries; this is not a full main build.

The engine's asynchronous worker and Runtime/UI integration are not connected yet.
No hardware operation, saved document, boot setting or live audio path was changed
in this step. The full Windows port and its remaining acceptance gates remain open.

### Asynchronous engine-side coordinator — R225/R226

The new Windows BrokerCoordinator accepts immutable saved-document snapshots on
a control thread and performs all service I/O on its own serial worker. New
submissions cancel stale I/O; generation/pointer guards prevent stale status
publication. It compares fresh durable snapshots before each CAS operation, so a
lost committed reply does not cause duplicate setup. A non-authoritative document
pauses without mutation. Metadata acceptance is reported separately from device
phase; pending removals remain visible, including errors/reboot-required state.
Shutdown signals cancellation and joins before releasing transport/events/data.

R225's MSVC fixture passes superseded edits, lost replies, quota-safe comparison,
non-authoritative input, pending removal visibility and blocked-read cancellation.
R226 adds the actual protected SCM host/driver: worker transport callbacks use
administrator-disabled, privilege-reduced impersonation around the real verified
client. Starting with the parent's empty test store and its already absent unique
mix, the worker creates endpoints, accepts consecutive name/format edits, reaches
Ready for the latest 96 kHz/eight-channel state with unchanged endpoint IDs, and
explicitly removes the microphone. Durable state and independent endpoint
enumeration confirm completion. The previous repair/crash lifecycle and all
original document/default/endpoint preservation checks still pass; cleanup removes
the temporary service/configuration/binary/store.

Evidence: `artifacts/windows/coordinator-r225-verification.json` and
`artifacts/windows/broker-service-r226-verification.json` with exact tested
binaries/source snapshots retained on the Dell. This is restricted transport
impersonation, not a separate standard-user desktop process. The coordinator is
not yet instantiated or fed by Runtime, nor exposed in the actual app UI. Full
main-tree Windows build/package integration, that Runtime/UI wiring, and all
remaining platform/audio acceptance requirements remain unfinished.


### Engine coordinator integration — R227 (build and regression checks passed)

The production per-user engine now explicitly enables BrokerCoordinator for its
canonical routing.json. Arbitrary-storage Runtime instances (including control
self-tests) leave management disabled. A successfully loaded primary document is
authoritative; missing storage and backup recovery are not. Durable edits publish
the new document/generation before submitting work, including undo/redo. Setup
submission failures are diagnostic and do not turn a successfully saved edit into
a reported edit failure. Explicit retry resubmits the current authority state.

The state reply now includes microphoneSetup metadata acceptance, generation,
per-device phase/revision/removal/error, and submission/transport error. Old
worker generations are hidden. This is separate from audio restoration and is
not evidence of live audio. The existing engine recovery poll refreshes endpoint
inventory and rebuilds when microphones appear. Shutdown cancels and joins setup
before releasing audio resources. The desktop repair action and visible setup
status still require integration. R227 is a fresh full native-source snapshot
build, using the existing prebuilt web assets only; it does not validate the UI.

R227 rebuilt the control executable and coordinator tests from this complete
native source snapshot on the Dell (MSVC x64 Release, strict warnings). Both
`loopz-windows-runtime` and `loopz-windows-broker-coordinator` passed. Evidence:
`artifacts/windows/runtime-coordinator-r227-verification.json` and `r227-tests.log`.
This does not yet exercise Runtime with a live broker or validate installed audio.


### Live Runtime microphone management — R228

A fresh full native-source Dell build now links and runs the Runtime adjunct in
the protected service acceptance fixture. Missing primary storage pauses setup
without mutating the empty owner snapshot. A durable create makes the document
authoritative; polling independently observes broker Ready and Runtime microphone
connected after endpoint enumeration/rebuild. Rename retains both endpoint IDs.
After Runtime shutdown, corrupting the temporary primary exercises valid-backup
recovery: setup remains non-authoritative. A new durable edit resumes management,
retains endpoint IDs, and reconnects. Explicit durable deletion removes desired
state and endpoints. The fixture is silent and uses no hardware monitor/source.

The first full target link exposed a missing service-acceptance dependency on
broker_maintenance; CMake now supplies it. The first live run then exposed an
encoding mismatch: device_operations used /utf-8, but the acceptance target used
the Dell locale for its wide-string expected name. loopz_strict_target now applies
/utf-8 consistently on MSVC. The failed run cleaned up and preserved the saved
mix; its report/binary/log are retained separately. The corrected rebuild passes
all existing real SCM, restricted-client, format/name, live repair, automatic
crash restart, coordinator, spoof and cleanup checks, plus the new Runtime path.
Both persistence and coordinator CTest regressions also pass against the rebuilt
binaries. Original saved document, default roles and existing endpoint inventory
are unchanged; no temporary service, binary/store paths or SCM key remain.

Evidence: artifacts/windows/source-r228-verification.json, source-r228-run.log,
r228-tests.log and retained exact source/binaries on the Dell. This tests Runtime
with an explicitly enabled temporary document inside the privileged fixture,
not the production engine IPC path or a separate standard-user desktop process.
Connected silent transport is not audio continuity or latency proof. The actual
desktop repair action, visible status, installed app acceptance and remaining
platform/audio/release requirements are still unfinished.


### Desktop service setup and installed app — R229

The desktop repair command now invokes the helper's fixed --install-service
operation, then asks the engine to retry; it no longer passes a user-document
path or owner SID to a privileged reconcile operation. The interface projects
microphoneSetup from native state and shows asynchronous setup, backup recovery,
reboot-required, errors (technical details under a disclosure), and ready status.
Ready requires current generation, accepted metadata, no pending removal, and
connected audio transport for each desired microphone. Disconnected/stale engine
state cannot report ready. New setup copy has English/Russian/Chinese entries.
The preview notice describes automatic saved-edit updates rather than manual
repair after each new mix.

Fresh MSVC x64 Release desktop/helper/control and embedded host builds passed.
All 71 interface unit tests pass, and the browser fixture checks in-progress vs
connected, restart/error details, selection-change notices and their reset. The
actual Dell WebView2 desktop's repair button installed the protected service and
reached microphone-ready status for the existing saved mix. That click ran in an
elevated workbench desktop and does not prove standard-user UAC consent. The app
was then closed gracefully and reopened with a Limited interactive token; token
inspection proves desktop, supervisor and engine are all non-elevated. Its real
production engine IPC state accepts the saved desired graph and reports the
original microphone connected through the service. The saved routing hash,
original public microphone UID and all six default audio roles are unchanged.

Current Dell baseline now intentionally includes the running automatic LocalSystem
LoopZDeviceBroker service and protected desired-state database (unlike the earlier
fixtures, which removed them). The development package contains the exact R229
binaries and interface assets. The previous package is retained at
C:\Users\123\LoopZ-driver-tools\desktop-r229-before-stage. Installed broker SHA256:
7161524EF9B03371CA0EF124F19CE20B202B0A29CB46A3134E94A70A4ECD7230.
The signed driver remains 0.4.0.15/oem45.inf with its existing development cert.

The temporary UI debugger used an app-specific trusted HKLM override, because
current elevated WebView2 hosts ignore user environment overrides. It was removed,
the host restarted without debugging, the loopback listener and SSH tunnel closed,
and all temporary scheduled tasks unregistered. This follows Microsoft's documented
[WebView2 elevation/override behavior](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security).
No permanent debugger option was added to the app.

Evidence: artifacts/windows/desktop-r229-installed-verification.json,
dell-installed-setup-r229.json, dell-installed-setup-r229.png,
setup-service-status-r229.png and interface-r229-tests.log. Exact native source
and binaries remain in source-r229/build on the Dell; the final source/archive
and asset hashes are recorded. This is development/workbench acceptance, not a
production signed release. Live selected-app audio, broad format/stress and
platform compatibility, normal UAC consent/cancellation, remaining localization,
UI/DPI/accessibility and installer/release requirements are still unfinished.


### Silent Windows 10 process isolation — R230

The probe adds --feed-tone/--feed-tone-high seconds FEED_UID with exact LoopZ
PnP role validation, and the isolation fixture adds --silent-probe FEED_UID. The
parent refuses non-Feed endpoints or any saved hardware monitor before spawning
markers. Two sibling marker processes render simultaneously to the same virtual
Feed without opening physical playback devices. Capture each PID separately:
selected 440 Hz measured 0.0300155, unwanted 880 Hz 0.000058978; the independent
880 Hz control measured 0.030027, unwanted 440 Hz 0.0000680234. These are about
54.1/52.9 dB ratios from finite-window estimates (which include spectral leakage),
not a general crosstalk specification. Both 8192-frame windows have zero missing
frames after warm-up; lifetime discontinuities were 2/1, including startup.

The rebuilt MSVC fixture links immutable capture/core/device/DSP libraries from
R229; the production capture implementation is unchanged. It runs as a Limited
interactive user on Windows 10 19045 x64. Both parent and child reject a physical
render UID in separate negative checks. The saved document hash and all six
default roles remain unchanged, marker children exit, and temporary scheduled
tasks are removed. Evidence: artifacts/windows/isolation-r230-verification.json,
isolation-r230-signal.json and isolation-r230-guards-verification.json. Exact
source and tested executables remain in LoopZ-driver-tools/isolation-r230 on the
Dell. This proves current adapter isolation on this Windows 10 installation;
selected-app signals through the production graph, third-party apps, process-tree
transitions and long continuity still require acceptance.


### Installed selected-app mix recording — R231/R232

The reusable windows_app_mix_acceptance.py fixture uses ordinary per-user edit
IPC and the installed R229 engine/service, without an elevated helper. It refuses
saved hardware monitors, verifies an explicit virtual Feed, creates one unique
48 kHz stereo mix, and starts two distinct owned marker executables on that Feed.
Only the selected executable is bound as an application source; the mix gain is
0.5. Recording opens the public microphone, not a private transport shortcut.
After the run it deletes only its temporary mix, waits for service/inventory
removal, stops its marker children, and verifies original saved bytes, microphone
UIDs and all six audio default roles. Independent PnP enumeration confirms no
present temporary adapter. Temporary scheduled tasks are removed.

The native --record-app MIX_KEY 30000 recorder preallocates sample storage and
uses a callback that never allocates or waits. The control thread collects two
shared WASAPI mono clients for the stereo channels; per-channel sample counts
therefore differ slightly, and this is not a stereo phase-alignment test. R231
passed waveform/gain/exclusion checks but observed 797 driver missing frames over
the broader client lifetime, so it did not localize startup vs steady-state loss.

R232 adds native QueryInterruptTimePrecise collection bounds and samples the
existing engine's driver diagnostics (no extra driver lease/connection). Driver
samples are selected at least 50 ms outside both bounds to allow for kernel
interrupt-time tick precision; recording clients stay open while collecting the
post-window sample. Microsoft documents the respective clocks in
[QueryInterruptTimePrecise](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttimeprecise)
and [KeQueryInterruptTime](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kequeryinterrupttime).
The recorder links the documented Mincore import library.

R232 passed: the channels contain 1,440,000 / 1,444,224 frames, approximately
0.01498 selected-tone amplitude after half gain, and at most one consecutive
zero sample. The unwanted tone is below the 1% threshold in both channels.
Source/publication missing counts and graph/render failures do not increase.
Driver missing/drop/underrun/lease-silence counter deltas are all zero across the
conservatively bracketed recording interval. Driver missing count was already
934 before the interval and remained 934 afterward, so startup loss is explicitly
retained as a separate unresolved condition rather than hidden by warm-up.

Evidence: artifacts/windows/app-mix-r231-report.json,
app-mix-r232-report.json, app-mix-r232-verification.json and
app-mix-r232-cleanup-verification.json. Source/binaries are retained in
LoopZ-driver-tools/app-mix-r231 and app-mix-r232 on the Dell. The rebuilt recorder
links immutable R229 core/capture/device libraries; the installed app is unchanged.
This is one controlled 30-second graph, not third-party-app, multi-hour, broad
format or Windows 11 acceptance. Startup, process transitions, phase alignment,
remaining UI/installation and production release requirements remain unfinished.


### Startup interpretation and automatic format acceptance — R233/R234

Inspection of BridgeRing confirms that capture close clears pending processed
audio, writes while capture is closed are discarded, and a new recording starts
only after its 20 ms prefill is satisfied. Its missing counter includes this
unprimed silence; underruns count shortages after priming. Existing queue tests
explicitly distinguish these cases. The portable driver-ring and duplex-bridge
tests were rebuilt and both pass, including configured-format cases. This explains
why startup missing frames with zero underruns can precede R232's clean steady
window. The queue was not changed to replay old audio or remove its timing budget;
user-visible startup onset and transitions still need dedicated acceptance.

windows_format_acceptance.py adds opt-in LOOPZ_FORMAT_AUTOMATIC_SETUP=1 and a
pinned LOOPZ_FORMAT_PROBE_EXECUTABLE. In this mode it uses ordinary per-user edits,
waits for matching-generation metadata acceptance/device Ready/connected format,
and waits for automatic removal in cleanup. It never launches the privileged
setup helper or pnputil. Legacy setup remains available for historical fixtures.

The current R229 installed engine/service, with saved mixes left enabled and both
experimental processing flags off, passes separate 30-second exclusive PCM16
recordings at 44.1 kHz/58 channels (R233) and 176.4 kHz/64 channels (R234).
Both recordings have zero all-channel zero frames/runs, capture silent frames,
position gaps, discontinuities and timestamp errors, with expected per-channel
signal/gain checks. Original saved document and microphone IDs/default roles
are preserved; cleanup and independent PnP absence checks pass. These focused
passes do not supersede the unfinished full 192-format matrix or multi-hour gate.

R233's audio/cleanup report passed, but its limited-user wrapper could not finish
metadata collection from the protected broker file. The subsequent elevated,
read-only audit recovered the exact host/app/probe/script hashes. R234's wrapper
hashes the readable staged host instead and exits zero; installed-host equivalence
is already pinned by the R229 acceptance. This preserves the protected file ACL.
Evidence: artifacts/windows/format-r233-report.json,
format-r233-verification-audit.json, format-r234-report.json,
format-r234-verification.json and format-r233-r234-verification.json.


R235 also passes a separate 30-second 192 kHz/64-channel exclusive PCM16 profile
with zero captured silent/all-channel-zero frames, gaps, discontinuities and
timestamp errors. Original saved bytes/defaults and cleanup pass. Both experiment
flags remain off. Evidence: artifacts/windows/format-r235-report.json,
format-r235-verification.json and format-r233-r235-verification.json. R236 is a new
full 192-format sweep using the same unchanged native app/probe/service and
1.5-second measured windows; it is not complete until its terminal report and
cleanup are independently checked. The current live task must be verified by
process identity and report state, never inferred from this note.


### Windows Settings localization — R237 (local, not deployed)

The Windows Settings startup/routing/quit/privacy descriptions and both native
preview notices now use the existing English/Russian/Simplified Chinese catalog.
The browser fixture exercises Russian and Chinese Settings after its English
setup-state checks, verifies localized actionable buttons and descriptions, and
rejects the targeted leftover English messages. Its Windows source binding keeps
original playback unmuted, matching the actual platform contract. Screenshots
disable finite animations to avoid capturing the modal fade rather than its final
layout. Both 1280x900 localized dialogs were inspected and fit their controls.

All 71 interface unit tests, the build, focused formatting and Windows setup
browser checks pass. Evidence: artifacts/windows/settings-localization-r237-verification.json,
setup-localized-ru-r237.png and setup-localized-zh-CN-r237.png. This change has not
been deployed to the Dell: R236's audio sweep must keep its pinned R229 package
unchanged. Actual WebView2/DPI/localization verification remains pending after
that run. This does not prove full-app localization or accessibility.


### Format-transition failure and handle ordering — R236/R238/R239

R236 terminated after 14 passing profiles at 48 kHz, while changing the temporary
mix from 28 to 30 channels. It timed out before recording the next profile. The
captured production state shows accepted desired metadata but the broker phase
reboot_required at revision 26; inventory still reports the old 28-channel
microphone, and Runtime cannot attach its desired 30-channel transport. Cleanup
passes and independent PnP enumeration confirms the temporary adapter absent.
This is a transition/setup failure, not a failed waveform recording. Evidence:
artifacts/windows/format-r236-report.json, format-r236-verification.json and
format-r236-failure-state.json. The failed task was unregistered after terminal
state/cleanup; it was not restarted on an observation timeout.

Inspection found an independent race in Runtime's edit path: it submitted a new
format/removal to the asynchronous service before rebuild closed obsolete KS
transport handles. R238 moves submission after rebuild (including its failure
cleanup), so the service cannot act on that new intent while this engine still
holds the old format's transport. This ordering change is not yet proof that the
R236 failure is resolved; a fresh R239 full sweep is running against the rebuilt
app. No driver, buffer or DSP fidelity changes were made. Archive comparisons
prove native core/headers, SingZ source and overlays unchanged from R229.

Fresh MSVC desktop/control builds and the Windows persistence CTest pass. The
new package was deployed through a Limited interactive task after graceful engine
and desktop shutdown, retaining desktop-r238-before-stage as backup. The original
saved bytes, microphone UID/defaults and non-elevated desktop/supervisor/engine are
verified. The existing R229 protected service and driver remain installed. R237's
localized interface assets are now deployed; actual localized WebView2/DPI visual
acceptance remains pending. Evidence: artifacts/windows/desktop-r238-installed-verification.json,
runtime-r238-tests.log and r229-r238-core-comparison.json. R239 remains unfinished
until its terminal report and cleanup are verified.


### Local Windows Settings keyboard/small-window checks — R240

The Windows setup browser fixture now verifies both Russian and Simplified Chinese
Settings at 960x600 after its 1280x900 localization checks. Dialog bounds remain
inside the viewport; 24 Tab steps stay in the modal, reach repair and quit controls,
and keep each focused control visible. Escape closes Settings and restores focus
to the banner trigger. Small dialogs scroll vertically; they do not show every row
at once. Screenshots were inspected, and focused formatting passes. Evidence:
artifacts/windows/settings-accessibility-r240-verification.json and
setup-small-ru-r240.png / setup-small-zh-CN-r240.png. This is local Chrome fixture
coverage, not actual Dell keyboard/DPI or Windows Narrator acceptance. The test
extension changes no installed assets while R239 runs.


R240 also rebuilds the complete local portable-core Release configuration and
passes all 47 CTest tests, including resampling/routing/history, intent/database/
planner, driver ring/duplex models and PCM16/source-refresh tests. Source hashes
and logs are recorded in artifacts/windows/portable-r240-verification.json,
portable-r240-build.log and portable-r240-tests.log. This is local macOS portable
unit coverage, separate from installed Windows audio/driver acceptance; it does
not replace R239's still-running matrix or the remaining platform/release gates.


### Float transport groundwork — R241 (not integrated)

The installed transport-v2 driver still carries PCM16 privately. Full float
precision is not established by the shared-engine float mix format or the PCM16
format matrix. Preparing a float transport must preserve existing PCM16 clients,
explicitly negotiate versions/capabilities, use a compatible protected registry
format, and keep conversion/storage outside allocation or blocking in callbacks.
No live driver or app was changed while R239 runs.

The new driver/sample_bits.hpp supplies integer-only sample boundary conversion:
every signed PCM16 value normalizes exactly to IEEE float bits; explicit float-to-
PCM16 conversion matches existing saturation/truncation, and invalid samples map
to zero. Finite float validation preserves signed zero, subnormals and headroom
above unity rather than clipping the future float path. These primitives have no
floating-point types, library calls, allocation or synchronization.

Release tests cover all 65536 PCM16 round trips, neighboring values at every
quantization edge, exponent/sign extremes/NaNs and one million deterministic
float patterns against an independent numerical reference. Undefined-behavior
sanitizer execution passes. Clang Windows-x64 cross-target wrapper assembly has
no FP/vector registers, x87 operations or function calls. Evidence:
artifacts/windows/sample-bits-r241-verification.json and sample-bits-r241-x64.s.
This is groundwork only: MSVC/WDK, negotiated transport integration, public
PCM16/float32 stream support and actual installed float-fidelity acceptance are
still required. The current driver remains unchanged.


### Float bridge/storage model — R242 (not installed)

BridgeRing and DuplexBridge now expose sample-width templates with the original
PCM16 aliases/defaults retained. Additive float aliases use four-byte samples,
240-frame packets and 40 ingress slots: the payload remains 61440 bytes and the
ingress frame budget remains 9600 frames. Recording capacity stays 200 ms and
prefill stays 20 ms. Byte storage doubles where precision requires it; buffering
in frames/time was not increased. Protected AudioFormatRecord v1 remains 16 bytes.

The additive version-3 float request/capability/processed declarations preserve
v2 message sizes, offsets and defaults. This is a declaration/model only: the
current installed dispatcher still accepts v2, and no installed driver/runtime
selects v3. Compatibility conversion/negotiation and public float stream support
must be implemented before claiming a float path.

Legacy ring/duplex regressions pass after extraction. Float tests cover all 192
rate/channel shapes with guarded external storage, precise float bits beyond
PCM16 resolution, signed zero/subnormals/headroom, FIFO wrapping/overflow and drop
counts, capture-close stale-audio clearing, wrong generations and lease expiry.
Float tests also pass undefined-behavior sanitizer execution. Evidence:
artifacts/windows/float-bridge-r242-verification.json. No live Dell assets changed
during R239; actual MSVC/WDK, installed compatibility/fidelity and timing acceptance
remain required.

## R239 completed format sweep and R243 float codec checks

The R238 installed Dell build completed the automatic-service PCM16 sweep:
192 distinct combinations, six rates from 44.1 to 192 kHz and every even channel
count from 2 to 64. Each profile recorded 1.5 seconds after warm-up. All channel
signal checks passed, with zero capture gaps, discontinuities, timestamp errors
or WASAPI silent frames in those windows. Seven profiles contained three
isolated all-channel-zero samples each; the maximum zero run was one frame.
Saved mixes stayed enabled; publication waiting and streaming chunks stayed
disabled. These are short-window checks, not long-duration or transition proof.

The terminal process and task were checked, the completed task was removed, and
the temporary adapter and mix were absent. The original microphone identity and
six default audio roles were preserved; the saved document hash matched before
and after. Evidence: `format-r239-report.json`, `format-r239-verification.json`,
`format-r239-post-verification.json` in `artifacts/windows/`.

R243 adds guarded planar/interleaved float codec round-trip checks, including
stride padding, signed zero, subnormals, finite headroom and invalid samples.
The x64 SSE2 tests execute under Rosetta with both normal and FTZ/DAZ modes;
arm64 memory and undefined-behavior sanitizer checks also pass. Six focused
portable driver/codec/negotiation tests pass. A zero-channel test fixture pointer
was corrected to stay inside its allocated destination storage.

This float work remains local and has not been deployed or verified through
the Windows kernel. The installed driver remains PCM16 transport version 2.
Evidence: `float-codec-r243-verification.json`.

## R244 kernel float transport build (not installed)

The adapter now uses the float duplex bridge internally and accepts explicit
private transport versions 2 and 3. Capability replies describe the selected
version. Each successful connection stores its protocol version alongside the
existing process owner and generation; a request using a different version is
denied before refreshing the lease. Version 2 PCM16 writes split into bounded
240-frame float packets. Version 2 playback returns at most 240 PCM16 frames,
within the existing 480-frame limit. Version 3 carries float payloads and
sanitizes nonfinite processed samples at the driver trust boundary.

The existing public PCM16 streams convert at the DMA boundary. Conversion uses
integer IEEE bit operations and a scratch packet owned by the nonpaged adapter,
serialized by the existing audio lock. No maximum-sized packet is introduced on
a kernel stack. Configured FIFO durations stay unchanged; sample storage doubles.
Public float format advertisement and live acceptance remain unfinished.

A fresh isolated Dell WDK Release x64 build succeeds with zero warnings/errors.
The unsigned SYS, exact source archive and build log are retained as R244
artifacts. This binary was not signed, installed or loaded; the live Dell stays
on the R238 app and 0.4.0.15 driver tested by R239. Seven focused portable tests
pass, including all PCM16 values, one million float patterns, unaligned and
in-place boundaries. The new conversion test also passes arm64 address/undefined
behavior sanitizers and x64 execution under Rosetta. Evidence:
`artifacts/windows/driver-r244-verification.json`.

## R245 public float formats build (not installed)

Each WaveRT miniport now constructs float32 and PCM16 formats and corresponding
attribute-bearing data ranges from its adapter's configured rate and channels.
Float32 is the default mode and published OEM format; PCM16 remains an explicit
alternative. Range intersection selects the format belonging to the offered
range. Existing format validation iterates both formats. The configured format
record stays version 1 with its original rate/channel-only layout.

DMA callbacks pass the actual selected sample width into the adapter bridge.
Float samples remain float bit patterns through these conversions, with
nonfinite input sanitized. Silence storage covers the wider render packets.
Notification/allocated buffer validation uses the actual sample width, keeping
the original 200 ms duration bound and requiring whole frames. Legacy callers
of the shared buffer helpers still default to PCM16.

The isolated Dell WDK Release x64 build succeeds with zero warnings/errors.
Five focused portable tests pass after rebuilding affected targets. The OEM
property checks cover both widths across all 192 rate/channel combinations.
This is not installed audio evidence: float format enumeration, fidelity,
continuity and legacy compatibility still need live Windows tests. The unsigned
source build retains the old development version and must receive a new driver
version before installation. Evidence and exact source/binary hashes:
`artifacts/windows/driver-r245-verification.json`.

## R246 native float probe and Windows checks

The recording probe adds `--format-roundtrip-exclusive-float`. It requests exact
float32 WASAPI exclusive streams for both Feed and microphone, writes generated
float markers and reads float recordings for the existing gain/spectral and
continuity analysis. Unsupported formats fail instead of falling back. Existing
PCM16 and latency modes retain their prior sample formats. The format acceptance
harness accepts this new opt-in command. These signal checks do not establish
bit-exact float fidelity.

A fresh isolated MSVC Release x64 build on the Windows 10 Dell completes for the
desktop, control utility and cable probe, rebuilding the native dependencies.
Seven focused checks then run as native Windows executables and pass, including
the SSE2 codec's normal and FTZ/DAZ cases. The tests intentionally undefine
NDEBUG; MSVC emits D9025 for overriding the Release definition. Actual binaries,
source archive, hashes and logs remain in source-r246 and the local evidence
folder. Nothing from this build has been deployed. Installed broker compatibility,
legacy transport fallback and the new driver's live float recording remain open.
Evidence: `artifacts/windows/source-r246-verification.json`.

## R247 installed client fallback and R248 recording

The R246 app/control binaries are now installed in the Dell development stage,
with the previous app preserved in desktop-r247-before-stage. The driver and
privileged helper/service are unchanged. The normal interactive desktop,
supervisor and engine are all verified non-elevated. The existing broker
accepts the new runtime, and transport negotiation selects version 2 / PCM16
against the installed 0.4.0.15 driver. The original saved mix hash, microphone
identity, six audio defaults and absence of UI debugging overrides are verified.
Evidence: `desktop-r247-installed-verification.json`.

R248 runs a 30-second 48 kHz stereo exclusive PCM16 recording using this exact
app and the R246 native probe, with automatic service provisioning of its own
temporary mix. Signal/continuity acceptance passes. Cleanup verifies no test
Python process, temporary adapter or mix remains, removes the completed task,
and checks the original microphone and six default roles again. The saved
document hash matches before and after. This establishes the legacy-driver
fallback path, not the new float driver's live behavior or fresh latency.
Evidence: `format-r248-report.json`, `format-r248-verification.json`,
`format-r248-post-verification.json` in `artifacts/windows/`.

## R249 explicit maintenance migration (built, not deployed)

The elevated install/repair service path now reads the fixed protected SQLite
store after stopping the broker and taking the existing device setup lock.
For desired upserts whose roots already exist and use a different package, it
uses the maintenance-only package-change path. Missing roots remain for startup
reconciliation; explicit removal tombstones never cause recreation. The existing
hardware and protected owner checks run before mutation, and no user document
path or IPC field can authorize a package change. Roots are not removed/recreated.

All successful non-reboot upserts now verify that Windows selected the intended
published package, including maintenance calls. Maintenance finalizes ready
endpoints, reports migrated device count, and propagates a requested reboot as
exit 3010 for the desktop/installer's existing restart handling. Routine broker
reconciliation still rejects package changes.

Fresh isolated MSVC Release x64 builds complete for helper, broker/service
acceptance fixture and device operation tests. The native registry persistence,
ownership and preflight test passes using its isolated registry fixture; no
devices are modified. The service fixture was built but not run against the
existing production service/store. This change is not deployed and cross-package
migration, endpoint identity stability and reboot recovery remain acceptance
gates. Evidence: `artifacts/windows/source-r249-verification.json`.

## R250–R252 installed float driver and first recording

Driver 0.4.0.16 is built from the R250 source archive and signed with the unchanged
Dell workbench certificate (FEC5B498B1777655AF90E665A0856E2755212078). SYS and
catalog Authenticode signatures validate; catalog generation reports no errors
or warnings. This remains development signing. Windows PnPSignedDriver reports
IsSigned=false, so it must not be described as Microsoft production signed.

R251 invokes the R249 native helper's elevated install-service path with the new
package after gracefully closing LoopZ's own desktop/engine. It reports one
migrated device, no reboot required. The original root remains and now binds
oem46.inf / 0.4.0.16. The protected service host is the exact R249 binary. The
staged helper, driver package and service payload are refreshed; the previous
entire stage is retained under maintenance-r251/previous-stage. The R246 app
reopens non-elevated and negotiates transport version 3, 32 bits. Saved document
hash and six physical default roles are preserved; no debugging overrides remain.

The original microphone endpoint GUID changed from
{81cbd352-d00e-4a2d-a522-1d570030973f} to
{f3e26d69-6ba7-4cac-9568-3d37a0d368fc}. The logical mix reconnects, but external
recording apps may need selection again. Endpoint identity stability across
package upgrades remains an explicit failure/open gate, not a passing check.

R252 records 30 seconds at 48 kHz stereo with explicit float32 exclusive clients
on Feed and microphone, through the half-gain native mix and float private
transport. Signal acceptance passes with no capture gaps, discontinuities,
timestamp errors or silent frames. Cleanup, original logical mix, post-upgrade
microphone identity and defaults are verified. This is signal/continuity evidence,
not bit-exact fidelity or latency evidence. New-driver full-matrix, legacy client
and long recovery acceptance remain unfinished. Evidence:
`float-installed-r252-summary.json`, `format-r252-report.json`,
`format-r252-verification.json`, `format-r252-post-verification.json`,
`desktop-r251-installed-verification.json` under `artifacts/windows/`.

## R253 PCM16 public compatibility and R254 active float sweep

With driver 0.4.0.16 and the R246 app/probe, R253 records 30 seconds at 48 kHz
stereo through explicit PCM16 public clients while the native engine uses
private float transport version 3. Signal checks pass, with no capture gaps,
discontinuities, timestamp errors, silent frames or all-channel-zero frames.
Cleanup confirms terminal process, deleted test mix/adapter, removed task,
unchanged saved document, preserved post-upgrade microphone and six defaults.
This verifies public PCM16 compatibility, not private version-2 client acceptance
against the new driver. Evidence: format-r253 report/verification/post-verification.

R254 starts the 192-shape float32 exclusive sweep with the exact same installed
app/driver and R246 probe. The Python process is confirmed live in session 1,
PID 9792, command line points to format-r254, mode full. The scheduled task is
LoopZ-R254-Format with a two-hour deadline. This is active work, not a passed
matrix. Do not build, replace binaries, reboot or run overlapping audio tests
until its process is terminal and cleanup has been inspected.

Microsoft's [Endpoint ID Strings documentation](https://learn.microsoft.com/en-us/windows/win32/coreaudio/endpoint-id-strings)
states that endpoint IDs may change on driver/OS upgrades. Its newer
[PKEY_AudioEndpoint_StableId](https://learn.microsoft.com/en-us/windows/win32/coreaudio/pkey-audioendpoint-stableid)
is available from Windows 11 24H2, not Windows 10. Thus raw endpoint-GUID stability
across package upgrades cannot be assumed as a platform contract. LoopZ's logical
mix/root identity and reconnection work in R251; clear upgrade reselection
notification/recovery acceptance remains open for external app selections.
Do not attempt to preserve GUIDs by rewriting undocumented Windows audio state.

## R254 terminal failure; R255 precision fixture; R256 active build

R254 is terminal, not a passing matrix. Its first 48 kHz stereo float recording
passes, then changing the same mix from 2 to 4 channels fails setup. The broker
reports driver acceptance but Windows shared/feed format rejection 0x88890008
while the legacy format selector requests PCM16. The report contains one passed
profile, ok=false, cleanup=true. Cleanup is independently checked: test process,
adapter and mix are absent, completed task removed, post-upgrade original
microphone and six defaults preserved. The failure state is retained separately
as format-r254-failure-state.json.

The local endpoint selector now tries explicit float32 first and falls back to
PCM16 only when exclusive-format support returns AUDCLNT_E_UNSUPPORTED_FORMAT.
This is an attempted fix awaiting the real failing transition; compilation alone
will not resolve this acceptance gate.

R255 prepares --format-precision-exclusive-float with a 1e-6-amplitude marker,
below one PCM16 step. Expected signal and continuity bounds scale with amplitude,
while reports retain the actual measured amplitudes. The portable codec fixture
checks exact float preservation and an independent negative control: the same
marker becomes all-zero through PCM16. Rebuilt CTest, x64 SSE2 execution and
arm64 memory/undefined-behavior sanitizers pass. This does not prove installed
precision until the probe is rebuilt and exercised through the Windows app.

R256 is a fresh isolated Windows build of the selector fix, native helper, probe
and codec test. The active local execution handle is 89627; build log is
C:/Users/123/LoopZ-driver-tools/source-r256-build.log. Nothing from R256 is yet
deployed. The installed baseline remains R246 app, R249 broker/helper and
0.4.0.16 float driver. Evidence: precision-r255-local-verification.json.

## R256–R258 built selector repair and low-level live recording

The R256 helper, broker and cable probe build on Windows; its native float codec
test passes including the below-PCM16-step negative control. R257 installs only
the new helper/broker using the same 0.4.0.16 package. The native helper reports
zero migrations and no reboot. The app/driver remain unchanged; original
microphone identity, saved document and all defaults are verified. This repair
does not itself prove the formerly failing channel-width transition.

R258 runs the explicit precision command for 30 seconds at 48 kHz stereo with
input amplitude 9.999999974752427e-7. Measured target output amplitudes are
5.000000295662061e-7 and 5.000000083018893e-7, with the expected half gain.
The public clients are float32 and private transport is version 3 / 32 bits.
No capture gaps, discontinuities, timestamp errors, silent or all-zero frames
are reported. Cleanup, post-upgrade microphone identity, document hash and six
defaults are verified. This detects PCM16 truncation; it does not assert bit-exact
waveform delivery or every float mantissa bit. Evidence: precision-r258-verification.json
and format-r258 report/verification/post-verification in artifacts/windows.

R259 starts a new full 192-shape float sweep after this actual selector change,
using the pinned R256 probe. Do not overlap audio tests or replace builds while
its confirmed process is active. The old R254 result remains a failed matrix.

R259's last live observation confirms Python PID 6308 in session 1, four passed
profiles (48 kHz / 2, 4, 6 and 8 channels), running 10 channels with no error.
This verifies the original width-transition fix at those shapes only, not the
remaining matrix. No restart or overlapping audio test was performed.

## R260 uninstall enumeration preflight (local, not built)

The full uninstaller previously skipped a recognized LoopZ adapter when its
instance-ID query failed, then continued package/service removal. It now raises
a visible error instead. The uninstall inventory is owned by an RAII device-set
wrapper so enumeration failures also release the setup handle. This is a local
change, formatted but not Windows-built or deployed. It does not solve deferred
cleanup after reboot or constitute uninstall acceptance. The installed R256
helper remains unchanged while R259 records. Evidence:
artifacts/windows/uninstall-preflight-r260-local.json.

R259's latest checked process is still PID 6308 in session 1. Thirteen profiles
have passed, running 48 kHz / 28 channels, with no recorded error. This is an
active sweep, not final matrix acceptance; cleanup has not run yet.

## R261 setup reboot result handling (local, not fixture-tested)

The production installer previously accepted only helper exit 0, so a successful
install-service return of 3010 was presented as failure. Its local script now
accepts both successful results, carries the restart flag through NeedRestart,
and prevents the post-install launch predicate when a restart is required.
This uses Inno's documented [NeedRestart event](https://jrsoftware.org/ishelp/topic_scriptevents.htm)
and preserves the failure path for other exit codes.

The harmless installer-flow fixture now covers helper success, reboot-required
success and failure. An extra fixture-only Run entry exercises the production
CanLaunchLoopZ predicate even in silent mode; it launches only the harmless stub
on success and must stay blocked for reboot/failure. /NORESTART prevents an
actual reboot, while /RESTARTEXITCODE=3010 checks restart propagation.
No production signing gate is bypassed.

This change is local and the Windows fixture has not run while R259 records.
It does not resolve full uninstall cleanup when driver removal needs reboot;
that remains open. Evidence: artifacts/windows/installer-restart-r261-local.json.

## R262 portable regression and uninstall cleanup errors

The current portable Release build (macOS arm64, platform backend disabled)
passes all 52 CTest checks after rebuilding. This covers portable routing/DSP,
database/intent logic and Windows ring/format/codec models, not Windows API or
loaded-driver acceptance. Exact portable source hashes and logs are retained in
artifacts/windows/portable-r262-verification.json.

The pending local uninstaller change also checks package receipt/value deletion
results. A cleanup access/error result now returns failure with its actual
Windows error code, allowing the installer to retain files for a retry instead
of falsely reporting complete removal. This helper remains unbuilt/uninstalled
while R259 runs. Reboot-deferred full uninstall cleanup is still unfinished.

The last verified R259 process remains PID 6308 in session 1, with 39 passing
profiles and 44.1 kHz / 16 channels running, no error. This observation is not a
completed sweep or cleanup result.

## R263–R267 prepared compatibility/cleanup and verified clock correction

The new private-v2 acceptance fixture provisions only one unique root from an
isolated document using --sync, never --reconcile, then runs five native phases
including explicit disconnect silence/reconnect. It checks actual negotiated
version/bits and preserves the saved document, existing microphones and six
defaults. Its fixture is accepted by the production decoder in a local check;
Python/format checks pass. The native probe fields and test have not run yet.

The pending full-uninstall path now passes only its successfully removed native
instance list, under the still-held setup lock, into broker cleanup. Pending
Windows removals may persist to reboot, but that should not orphan the stopped,
unregistered user-mode broker/store. Strict direct purge remains unchanged and
unknown files/security failures still block cleanup. This is pending Windows
build and real reboot-required uninstall acceptance, not a completed fix.

R259 fails after 90 profiles at 88.2 kHz / 54 channels with zero audio stretches.
Its sampled driver queue has no underruns and publication reads have no misses,
but input timestamp resets repeatedly clear source history. The float adapter
split timestamps added two separately rounded durations. A deterministic model
reproduces false resets at 44.1/88.2/176.4 kHz; one rounding from the original
stream frame coordinate yields none at all six rates, without loosening the
100 ns clock-gap tolerance.

Driver 0.4.0.17 carries the anchor/frame coordinate through packet splitting.
R265 WDK build and development signatures pass with zero warnings/errors. R266
installs it using the unchanged verified R256 helper/broker and R246 app, one
migration, no reboot. EnumDeviceDrivers/GetDeviceDriverFileNameW identifies the
loaded DriverStore image and its exact signed-package hash. Saved mix/defaults
remain intact; the microphone endpoint GUID becomes
{83617c24-23de-49c0-8bd0-2071c27d338f}.

R267 records 30 seconds at the failing shape. Input timestamp resets remain zero
and input-history missing counters stabilize, confirming the clock correction.
The overall recording still fails: 12257 all-channel-zero frames match 12257
unpublished mixer-output frames in the sampled publication counters. This is a
separate producer/output scheduling gate; no full matrix pass is claimed. Its
fixture/process/task cleanup, microphone identity and six defaults are verified.
No audio acceptance process remains running after R267. Evidence:
clock-fix-r267-verification.json, format-r259/format-r267 report and cleanup
files, driver0417-sign-r265.json, driver0417-loaded-r266.json, and
private-and-uninstall-r263-local.json in artifacts/windows.

The refreshed R264 source archive queues the pending helper/probe/installer
fixture changes plus the current clock regression source for a native build.
It has not been deployed. The live baseline remains R246 app, R256 broker/helper,
and 0.4.0.17 driver; investigate publication scheduling before new audio sweeps.

The R264 native build is now active via local execution handle 20902, logging to
C:/Users/123/LoopZ-driver-tools/source-r264-build.log. No audio test overlaps it.

## R264/R268–R270 native validation and remaining publication failure

R264 builds the pending native helper, cable probe, installer stub, device
operation and clock regression targets. Device persistence/ownership preflight
and split-clock checks run natively and pass. Full uninstall with pending kernel
removal is compiled but not yet accepted on real reboot-required removal.

The installer regression exposed a real ordering issue: NeedRestart is evaluated
before the previous ssPostInstall helper callback. The helper is now excluded
from the wildcard Files entry, installed last, and its guarded AfterInstall
procedure runs microphone setup before restart evaluation. The post-install
callback is an idempotent fallback. The corrected harness embeds the exact
production Files/Run/Code, plus a fixture-only completion hook with documented
<event> syntax to exercise the launch predicate. R268e passes all three cases:
0 permits launch, 3010 reports restart and blocks launch, and helper failure 42
produces setup failure 1 and blocks launch. The fixtures never install drivers
and always use /NORESTART; production signing requirements remain intact.
Evidence: installer-flow-r268e-verification.json.

R269 provisions one isolated root from a temporary document, without editing
the saved mix or using absence-based reconciliation. The R264 probe negotiates
private protocol 2 / PCM16 on the installed 0.4.0.17 driver. All five phases pass,
including stop/restart, disconnect silence and reconnect. Only its root is
removed; document bytes, existing microphone IDs and six defaults are preserved.
The completed task is removed. Evidence: private-r269-report.json and verification.

R270 enables the existing publication-wait and streaming-chunk options only for
a controlled 30-second 88.2 kHz / 54-channel test. The actual flags are verified
in the non-elevated engine. The recording still fails: 17 all-channel-zero frames
match 17 unpublished mixer output frames, with a 17-frame zero run and an invalid
sample step. Input timestamp resets and driver underruns remain zero in sampled
counters. Waiting occurs 214 times, with zero reported entered-wait timeouts;
that counter does not cover a pending read denied wait budget. The nominal 10 ms
batch limit and actual available FIFO reserve need investigation.

Both temporary options are restored OFF by restarting the original app normally.
The restored desktop/engine/supervisor are verified non-elevated, the microphone
identity and saved document/defaults are preserved, and no audio test is active.
No production default is changed or continuity threshold relaxed. Evidence:
format-r270 report/verification/post-verification,
desktop-r270-restored-installed-verification.json, and
installer-private-wait-r270-summary.json in artifacts/windows.

### R271 — local FIFO-budget publication-wait correction

The private worker publication wait now uses the existing 30 ms worker lateness
limit rather than rejecting all waits after a nominal 10 ms batch. It spends only
the observed recording FIFO time after reserving rendering, observed packet write
time, and a 1 ms margin. The same constant guards the worker failure deadline.
No FIFO size, audio timestamp, or continuity acceptance threshold changes.

A deterministic late-producer case and supported-rate queue/deadline invariants
pass alongside the duplex lifecycle suite, both optimized and with ASan/UBSan
on macOS. These are portable policy checks, not Windows API or installed audio
acceptance. Source hashes and limits are recorded in
`artifacts/windows/publication-wait-r271-local.json`. The modified app still needs
a fresh native Windows build and controlled Dell recording test before this can
be called a fix for the R270 gaps. Dell remains on R246 with both experiments OFF.

### R272 / R274 — native publication-wait acceptance and active full sweep

R272 rebuilt the current source snapshot natively with MSVC x64 Release; the
Windows duplex regression passed. The exact app SHA256 is
922C6417194CE4EA4BBD62BCCAD9A8DB0F925E35F9AFACE5D15EC3ACD2CD0AB7.
The non-elevated installed desktop, engine, float v3 transport, unchanged broker,
original microphone, saved mix and all six defaults were verified before testing.

The previously failing 88.2 kHz / 54-channel exclusive float path passed a fresh
30-second recording: no all-channel-zero frames, discontinuities, position gaps,
timestamp errors or silent-flag frames. Maximum sample step was 0.004276711,
below the unchanged 0.004508251 limit. The temporary test adapter and mix were
removed and original defaults/microphone were verified afterward. This test
used both publication-wait and stream-chunk experiments ON; it does not prove
normal-default acceptance or the full Windows port. Raw R272 report, binary
verification, cleanup proof and installed summary are in artifacts/windows.

R273's requested two-minute repeat was rejected by the existing 30-second
harness limit before audio or graph mutation; its terminal task was removed.
There is no two-minute continuity result. A bounded-memory long-duration probe
remains required rather than silently treating repeated short runs as sustained
continuity.

R274 now runs the complete 192-profile float sweep using the supported 1.5-second
recording windows. Its real Python process was confirmed as PID 7616 / Session 1,
with report format-r274/report.json, task LoopZ-R274-Format. Do not start a second
sweep on an observation timeout: inspect this job until authoritative termination.
Both experiment options remain ON for this active controlled test. After terminal
cleanup, restore the exact backed-up R246 app/control using
start-worker-restore-r272.ps1 and verify-worker-restored-r272.ps1. No driver or
protected broker was changed.

### R275 — longer recording probe with the original allocation ceiling

The local acceptance probe now permits 1.5–300 second recordings while retaining
the previous maximum storage allocation: 380,928,000 float samples, the original
31-second / 192 kHz / 64-channel capacity including its one-second guard.
Five-minute stereo and one-minute 88.2 kHz / 54-channel recordings fit; two-minute
54-channel recordings do not. Every accepted sample remains recorded and analyzed
for continuity, spectral gain, leakage, and sample steps with unchanged thresholds.
This is a fixed allocation ceiling, not a constant-memory streaming analyzer.

The Python coordinator validates the largest profile in each sweep before any
graph mutation, and its long-probe timeout includes recording and analysis time.
All previously supported rate/channel combinations still accept 30 seconds.
Native portable bounds tests pass through CTest and ASan/UBSan; five actual Python
preflight rejection cases pass without creating reports. This does not yet prove
Windows compilation or a long installed recording. Evidence and hashes are in
artifacts/windows/probe-duration-r275-local.json.

R274 remains the same active 192-profile sweep (PID 7616, Session 1); 15 profiles
were completed at the latest check. Its immutable copied harness, app R272,
probe R256, and installed .17 driver remain unchanged. Do not build or start a
second audio test on the Dell concurrently. Restore backed-up app R246 and both
experiment flags OFF only after this sweep terminates and its cleanup is verified.

### R276 — silent default-following routing fixture prepared

The existing routing experiment uses an explicitly opened endpoint. Microsoft
documents a different WASAPI automatic-stream-routing activation through
DEVINTERFACE_AUDIO_RENDER and ActivateAudioInterfaceAsync, supported since Windows
10 version 1607. A new opt-in --automatic-route-probe FEED_UID fixture exercises
that path using its own unique executable identity, and changes/restores only that
owned process's per-app preferences. All six system defaults are compared.

The child submits only AUDCLNT_BUFFERFLAGS_SILENT buffers, including during device
changes; no tone-generation branch exists. It identifies the process's active
session endpoint before routing, waits for the target Feed, restores preferences,
and checks return to the original endpoint. It reports movement and restoration
separately. Restore failures are propagated while the process is alive, before
owned-process cleanup. This fixture does not prove signal delivery, process
capture continuity, or original-app muting; it resolves whether default-following
streams behave differently from the earlier fixed-endpoint probe.

Formatting checks pass; native compilation and execution remain pending. The
source-r276 archive/hash manifest and build-source-r276.cmd preserve this fixture
and the R275 longer-recording probe. No Dell build was started during R274 audio
acceptance. After R274 terminates and cleanup is verified, build these probe
targets separately; they require no app, driver, or protected broker deployment.

References: https://learn.microsoft.com/en-us/windows/win32/coreaudio/automatic-stream-routing
and https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync.

### R277 — independently audit the complete float matrix

The new read-only scripts/verify_windows_float_matrix.py requires all 192 unique
rate/channel pairs, the pinned probe hash, successful terminal cleanup and the
expected float public/private transport. It independently checks each channel's
recording duration, gain/leakage/peak, capture continuity counters, and sample-step
bound recomputed from the generated sinusoid. It preserves the native isolated
one-frame common-zero policy and does not treat a single-profile report as full
coverage. Report auditing does not replace installed binary or cleanup proof.

The real R272 30-second profile passes this independent profile audit. Five
synthetic validator regression tests pass, including duplicate coverage, capture
gaps despite native ok, inflated step limits, wrong hash and active-run rejection.
Both the report and recording-limit tests pass through the portable CTest setup.

A source inspection also found raw/exclusive format probes recorded capture-gap
flags without incorporating them into native ok. The pending probe now fails on
capture discontinuities, position gaps, timestamp errors or silent-flag frames.
This source change has not been built or deployed to Windows and does not alter
the ongoing R274 measurement. The final R274 report will be audited independently
for those flags rather than trusting its overall ok alone.

verify-format-r274.ps1 was parsed by the actual Windows PowerShell parser without
execution. It verifies terminal process/task state, exact format coverage and
capture counters on success, adapter/mix cleanup, original microphone, saved mix,
app hash and six defaults before task removal. -AllowFailedRun permits cleanup
verification of a terminal failure without claiming matrix acceptance.

The exact pending combined probe snapshot is source-r277.tar.gz with its hash
manifest and build-source-r277.cmd, superseding R276 for future probe builds.
It includes the long-duration bounds and silent automatic-routing fixture. No
Windows build or new audio test was started while R274 ran. At the last check,
PID 7616 / Session 1 was live with 53 profiles complete (44.1 kHz / 44 channels
running). Wait for its authoritative terminal state; never restart on timeout.

### R274 terminal / R278 build — recording FIFO underrun

R274 is terminal, not an active sweep. All 57 completed profiles independently
pass the new profile audit. At 44.1 kHz / 52 channels, the next recording failed
with 759 consecutive all-channel-zero frames. The driver missing counter grew
from 696 in both in-recording snapshots to 1455, exactly 759 extra frames. It
reported one underrun (54 requested, 41 queued) followed by re-prime silence.
Publication missing frames and input timestamp resets stayed zero, so this
failure differs from R267/R270. Worker maximum lateness was 15.2427 ms, batch
render 17.218 ms and chunk render 7.3946 ms; those wall times alone cannot
distinguish computation from preemption. Prior Pro Audio escalation R173 is
rejected evidence, not a solution to adopt without measurement.

The failed run cleaned its adapter and mix; native verification confirms original
microphone, saved document and six defaults preserved. The exact R246 app/control
hashes are restored, all app processes are non-elevated, and both experiment
flags are OFF. Evidence: format-r274 report/verification/post-verification,
desktop-r274-restored-installed-verification.json and
float-sweep-r274-terminal-summary.json. No full-format success is claimed.

R278 adds worker lifetime CPU time and thread ID/MMCSS status to existing native
control diagnostics, sampling GetThreadTimes from the serialized control owner
rather than an audio callback/render loop. This allows matched interval CPU
comparison with the renderer. It also builds the previously prepared long-duration
and silent automatic-routing probes. Its fresh exact-source Windows build is in
progress (source-r278-build.log); none of these new binaries is deployed yet.

R278 build completion: native MSVC x64 Release succeeds for desktop/control,
cable probe, automatic-routing process fixture and recording-limit tests. Three
Windows CTest cases pass: runtime persistence/revision/history, probe limits, and
report-validator controls. Local archive and remote source hashes match for the
changed worker/runtime. Evidence is source-r278-build.log, source-r278-tests.log
and source-r278-verification.json. This validates compilation and these focused
checks only, not routing movement or long recordings. No R278 binary is installed.
Next: controlled 44.1 kHz / 52-channel diagnostic recording on exact R278 app,
keeping buffers and priority unchanged; restore original R246 afterward.

### R279 / R280 — CPU evidence and real automatic stream routing

R279 temporarily deploys exact R278 app/control with both scheduling experiments
ON. The same original driver/broker, buffers and Audio priority remain unchanged.
A fresh 30-second 44.1 kHz / 52-channel recording passes with zero all-channel-zero
frames, capture position gaps, timestamp errors, discontinuities or driver
underruns. The prior R274 underrun is not reproduced or declared solved.

Across 49 in-recording snapshots, worker and graph thread identities are stable
and both report available CPU timing and successful MMCSS registration. With the
Dell QPC frequency freshly verified as 10,000,000 Hz, the interval spans
28.771147–28.830666 seconds. Worker CPU consumption is
18.593750 CPU seconds; graph consumption is
14.218750 CPU seconds. These are separate thread costs on this
graph, not a system CPU percentage or proof of why R274 underruns. The 57-profile
full-sweep failure remains authoritative.

Cleanup and exact R246 app restoration are verified after R279. Both temporary
flags are OFF, app processes are non-elevated, and original microphone identity,
saved document and defaults are preserved. Raw report, installed/cleanup/restored
proofs are in the corresponding artifacts/windows R279 files.

R280 executes the newly compiled --automatic-route-probe on the real desktop,
non-elevated. Its unique owned process activates DEVINTERFACE_AUDIO_RENDER and
submits only silent buffers. Its active session moves to the LoopZ Feed when
per-app preferences change, and returns to its original endpoint after preference
restoration. Native assertions and wrapper checks pass; saved document, original
microphone and six defaults stay unchanged. The fixture process/directory and
scheduled task are removed. Evidence: automatic-route-r280-report.json, raw
probe log and diagnostic-and-routing-r280-summary.json.

This proves routing behavior for an automatic/default-following stream, unlike
the earlier fixed-endpoint fixture. It does not prove signal delivery, process
capture during routing, muting for fixed-endpoint clients, or an implemented
production mute-original flow. Next use exclusively owned virtual-feed markers
to test signal/capture without sending test tones to physical hardware.

### R281 — reject channel-local FIR tiling on measured performance

R279's measured worker CPU cost motivates a controlled cache-layout experiment
in the wide non-unity cached FIR path. The candidate computes the identical
coefficients/normalization and unchanged SSE2 dot products, but visits eight
frames per channel before moving to the next channel. Portable parity tests pass,
including the newly added 52-channel / 201- and 240-frame packet geometry against
independent 32-frame FIR reference reads, with partial coverage, nonfinite and
extreme samples, interleaving, and callback heap guards.

Two isolated source workspaces are compiled with the same native MSVC /O2 /W4 /WX
settings on the Dell. The installed app, driver, broker, priority and audio buffers
are unchanged. An opt-in prepared-window benchmark runs baseline/candidate/
candidate/baseline across 32 cases (16/32/52/64 channels, 128/201/240/256 frames,
44.1↔48 kHz). All output checksums match. Nevertheless median candidate/baseline
ratios by channel width are 1.0472, 1.0374, 1.0449 and 1.0431: a regression.
The critical 52-channel 201/240-frame cases also regress roughly 2.9–5.0%.

The algorithm candidate is rejected. native/core/cached_fir.hpp is restored
byte-for-byte from the preserved baseline, and the expanded resampling suite
passes afterward. Only the manual benchmark and meaningful parity coverage are
retained. Raw source snapshots, ABBA reports, hash verification and comparison
are in artifacts/windows/fir-r281-*; these are computation microbenchmarks, not
new installed CPU/continuity acceptance. The intermittent FIFO underrun remains
open. No audio job, build or temporary experiment remains active on the Dell.

### R282 / R283 — long continuity and refreshed latency verified

R282 passes a sustained one-minute 44.1 kHz / 52-channel float recording on exact
R278 diagnostic app/probe with both scheduling experiments ON. Buffers, priority,
driver .17 and broker are unchanged. All-channel-zero frames, capture gaps,
discontinuities, timestamp errors and driver underruns are zero. In 99 snapshots
inside recording, driver missing frames stay at startup 600 and publication
missing frames stay zero. The independent profile audit passes. This is real
longer-duration evidence, not resolution of the earlier R274 intermittent failure
or full 192-profile acceptance.

R283's separate 20-marker latency recording passes on the same controlled app
and .17 driver. The exact path, public PCM16/private float32 distinction, buffers,
settings and refreshed latency statistics are recorded in the latency section
above. All raw QPC receipt/submission intervals and the conventional median were
independently recomputed. Both tests clean their adapters/mixes, and their
terminal scheduled tasks are removed by native verifiers. Exact R246 app/control
hashes, both options OFF, non-elevated processes, original microphone, saved mix
and six defaults are verified after restoration. No audio acceptance job remains
active. Evidence: R282 report/verification/post-verification, R283 latency files,
desktop-r283-restored-installed-verification.json and long-and-latency-r283-summary.json.

The retained 52-channel / 201/240-frame resampling parity tests also pass the
macOS AddressSanitizer/UndefinedBehaviorSanitizer configuration (11.36 seconds).
The rejected R281 tiling algorithm remains removed; no unmeasured optimization
was retained or deployed.

### R284–R287 — captured signal survives virtual default-stream rerouting

The test-only automatic renderer now emits a 440 Hz marker only when its active
endpoint is one of two explicitly supplied LoopZ Feed roots. It starts with a
silent buffer, sends silence on every other endpoint, refuses hardware monitors,
and uses two independent engine-managed temporary mixes. Native endpoint
inventory includes the verified hardware mixKey, so the wrapper selects roots
by identity instead of display labels. Owned marker processes are contained in
kill-on-close jobs. A completion event is signaled only after render clients
stop/release; the child remains alive while its original per-app preferences
are restored, then exits with its actual render result.

R285's first attempt fails reading a routing preference (E_INVALIDARG) while the
marker is suspended. No routing change or signal measurement succeeds. Both
temporary roots, marker image/process and saved state are cleaned and its task
is removed. R286 corrects initialization ordering: start in silence, wait for an
active audio session, then read/apply the temporary preference. The immutable
R284 and R286 source snapshots/build logs preserve both versions.

R287 passes the corrected native fixture on Windows 10 Home 10.0.19045. It routes
the default-following stream to the first virtual Feed and then the second while
process loopback capture remains open. The two 8192-frame measured windows show
440 Hz amplitudes 0.0299665 and 0.0300273, zero missing frames, and 880 Hz leakage
0.0000114142 and 0.0000654307. Cumulative capture discontinuities increase from
zero to one at the handoff: this proves capture before/after the change, not
seamless continuity through it. The public microphone recording path is not
part of this fixture. Production mute-original remains unimplemented.

Rendering stops before original preferences are restored; native default checks
and wrapper checks pass. Both temporary PnP roots and their document mixes are
removed, no marker/Python process or image directory remains, and the scheduled
task is removed. Exact saved document bytes/hash, original microphone identity,
all six defaults, baseline R246 app and non-elevated processes are independently
verified afterward. App, driver and broker binaries are unchanged. Evidence:
automatic-capture-r287 report/verification/post-verification/summary,
source-r286 verification/build log and desktop-r287-unchanged-installed-verification
in artifacts/windows.

The expanded native Windows FIR reference/heap-guard suite also passes on R284
in 8.76 seconds, including the retained 52-channel 201/240-frame cases. The R281
algorithm candidate remains rejected and absent. The next product work is durable
owned per-app routing/restore state, user-override and crash recovery handling,
and production selected-app → mixer → public-microphone acceptance. The separate
intermittent recording FIFO underrun and full-port gates remain unresolved.

### R288 — portable SQLite app-routing ownership journal

AppRoutingJournal is a new optional control-thread component using the existing
vendored SQLite library. It is not linked to the production Windows runtime yet
and creates no real user-state file or OS routing change. It records an
authenticated application identity supplied by the platform, all three original
opaque preferences and attempted virtual targets before any OS mutation.

Every prepare commits with synchronous=EXTRA/DELETE journal semantics and advances
a global persistent revision, even for an earlier/repeated target. Originals
cannot be rebased while a receipt is owned. Unknown/partial setter outcomes keep
both old and attempted targets recoverable. After all OS role values are verified,
confirm discards older targets so a later user selection of an older Feed is
recognized as an override. Restoration planning distinguishes unchanged values,
owned values to restore and different user choices to preserve. Revision-checked
forget/confirm reject acknowledgments from older acquisitions, including A→B→A
and release/reacquire. The platform still must read/check current OS values before
restoring and verify outcomes before forgetting the receipt.

The store refuses foreign application IDs/schema versions, malformed sequences,
integrity/foreign-key failures, NUL/oversized fields and unowned confirmations. It
never resets an unreadable file. Tests cover reopening, abrupt process exit after
commit without SQLite close/destructors, failed baseline replacement, retarget
ambiguity, user overrides, stale acknowledgments and Unicode database paths.
Foreign SQLite files stay byte-identical after refusal. SQLite's no-follow flag
is requested; the POSIX symlink test passes, while Windows VFS reparse-point
safeguards remain an explicit platform-adapter responsibility. The adapter must
resolve/protect the parent directory rather than canonicalizing a database
symlink into an accepted target.

All 55 portable core CTest cases pass (13.50 s). The focused journal suite passes
ASan/UBSan locally and MSVC x64 Release on the Dell (0.34 s), including real abrupt
process exit on both operating systems. Exact source/binary hashes, build/test
logs and scoped summary are in artifacts/windows/source-r288-* and
routing-journal-r288-*. No Windows routing preference, installed app, broker or
driver was modified by these temporary-file tests.

Production integration remains substantial: authenticate live process image/owner
and creation identity, avoid PID reuse, compare and restore per-role preferences,
respect user overrides, recover after restart/app exit/uninstall, verify actual
stream movement and roll back unsupported fixed-endpoint apps. A muted app must
not simply be redirected into an arbitrary existing mix Feed: when that mix also
has pass-through enabled, its audio could be included twice. A dedicated routing
sink or equivalent design that avoids this duplication is required before
enabling the product control. No production mute-original support is claimed.

### R289–R291 — render-only sink configuration and setup type preservation

The pending 0.4.0.18 kernel candidate adds an explicit render-only adapter
configuration. Legacy AudioFormatRecord remains 16 bytes and its valid() stays
unchanged. All 192 legacy rate/channel pairs still decode as duplex microphone
mixes. A separate version-2/tag-0x53494e4b record selects a fixed 48 kHz stereo
app-routing sink. Unsupported versions/tags/formats fail without modifying the
decoded output. A missing registry record keeps the original stereo/duplex
default. The PortCls startup path skips capture-filter registration only for the
validated sink configuration; render/format/transport processing is unchanged.

The WDK x64 Release build succeeds with zero warnings/errors; generated INF
DriverVer is 0.4.0.18. The SYS candidate hash and exact source hashes are in
driver-r289-verification.json. It is unsigned and NOT installed. Endpoint absence
and normal-microphone behavior on the loaded new driver are not yet verified.
The Dell remains on installed .17 driver and its existing baseline app/broker.

Normal microphone format setup now refuses newer/non-microphone binary records
and query failures before touching format or pending state. It must not turn a
sink into a visible microphone during ordinary repair. The native R291 registry
tests pass for preserved sink records, oversized future records, query-denied
handles, and retained legacy malformed-type/pending repair. R290's first test
incorrectly expected invalid_argument rather than runtime_error for operational
refusal; expectations and failure diagnostics were corrected. Its exact synthetic
sink-record test key was identified by GUID/data, backed up and removed; no
private test-registry keys remain. No actual PnP device was changed by those
registry tests.

All 56 portable core tests pass (3.36 s), the configuration contract passes
C++17 ASan/UBSan, and native Windows adapter/configuration and setup-safety tests
pass. Evidence: adapter-r289-portable-tests.log, source-r289-tests.log, R290
failed log/fixture cleanup record, source-r291 build/tests/verification, and
render-only-r291-summary.json under artifacts/windows. Adapter.cpp CRLF line
endings are preserved.

This is not a provisioned or usable product sink yet. Broker intent/store, setup
role authorization/identity, endpoint inventory and runtime leases still need
integration. Routine microphone repair must never overwrite a sink; existing
installed helpers do not yet support provisioning one. Do not deploy/create the
new sink configuration until the matching broker/setup/controller is ready, then
sign using the existing workbench identity and verify a real render endpoint with
no microphone in all endpoint states, plus normal-mix regression and cleanup.
Production mute-original, intermittent FIFO underruns and the full-port gates
remain incomplete.

### R292 — separate owner-scoped app-routing sink identity

The driver identity library now derives one stable sink key per authenticated
owner from `owner + NUL + NUL + LoopZ.AppRoutingSink.v1`. Normal mix identities
retain their original `owner + NUL + mixId` SHA-256 serialization; mix IDs reject
embedded NUL, so a valid mix ID cannot serialize into the sink domain. Identity
hashing does not authenticate a SID; the broker remains responsible for that.
Independent hash vectors verify legacy compatibility, sink separation and owner
isolation. Empty, oversized and embedded-NUL identities are rejected.

Identity code is a small shared static library rather than a source inside the
capture library. On the Dell, the exact immutable source-r292 snapshot builds
LoopZ.exe, the control tool, setup helper and device-operations test. All three
focused native tests (driver identity, device operations and runtime) pass in
0.61 seconds. Source hashes match the snapshot manifest; executable hashes and
build/test output are retained in source-r292-verification.json and
source-r292-consumers-retry.log under artifacts/windows. An earlier consumer
build lacked the MSVC environment and failed; the corrected recipe initializes
vcvars64 before building. No product code change was needed for that failure.

These binaries are not deployed. The installed baseline remains unchanged.
Sink identity alone does not provision a sink: typed broker/store keys, explicit
sink setup/readiness, lease recovery and runtime routing remain incomplete.

### R293 — typed portable sink intent and durable state

DeviceIntentRequest now distinguishes MicrophoneMix from AppRoutingSink. Mixes
continue to encode as the strict version-1 schema. The strict version-2 sink
schema carries only operation ID, revision, action and kind: clients cannot
choose its owner, mix ID, name or format. There is one fixed 48 kHz stereo sink
per authenticated owner. Existing source callers default to MicrophoneMix.

The portable SQLite store preserves existing mix keys and schema-1 records.
Its internal sink key starts with NUL, which public mix IDs reject. SQLite
bindings and reads use explicit byte lengths. The sink does not consume any of
the 32 live microphone slots; the existing total backlog bound still applies.
Removal acknowledgement compares kind, key and revision, so stale completion
cannot remove a newer sink record. Saved-document planning validates sink
records and leaves their lifetime to routing leases, independently of mixes.

Portable tests cover strict sink fields, fixed configuration, disjoint keys,
idempotent acceptance, reopen/recovery, stale removal, owner separation, all 32
mix slots alongside the sink, and preservation during saved-document cleanup.
All 56 portable tests pass (6.86 seconds); the three focused tests also pass
ASan/UBSan (2.61 seconds). Exact source snapshot and hashes are source-r293.

This is not yet an enabled Windows sink API. DeviceIntentStore::accept and the
Windows setup adapter explicitly reject sink requests pending typed progress,
reconciler and provisioning integration. This prevents the legacy setup path
from treating a sink as a microphone. No installed service/database/device has
been upgraded for this change.

Native Dell x64 Release verification also passes: setup helper and embedded
broker build successfully, and all three typed intent/database/planner tests pass
(8.26 seconds). All ten changed source hashes match the immutable snapshot;
five executable hashes are recorded in source-r293-verification.json. The build
and test log and typed-intent-r293-summary.json are retained in artifacts/windows.

### R294 — typed broker reconciliation and progress

Broker reconciliation now selects current desired records by kind plus public
identity. Its completed-work cache and runtime progress cache use the disjoint
storage key domain. Progress carries DeviceIntentKind, including superseded-work
notifications. Removal acknowledgements forward the kind to SQLite. A sink
removal therefore cannot acknowledge removal of a normal microphone record.

Status accepts a strict version-2 appRoutingSink query with no caller-chosen ID.
Version-1 microphone queries and replies retain their existing shape. Sink
readiness requires matching authenticated owner, kind, identity and desired
revision; a callback reporting another kind or revision remains queued.

On the Dell, the exact source-r294 x64 Release snapshot builds setup/embedded
broker and three isolated administrative fixtures. Protected store/status passes
(0.509 seconds), reconciliation passes (0.418 seconds), and broker runtime passes
(0.295 seconds). New tests seed typed state only through a trusted fixture SQLite
connection into a unique protected test directory; public Windows sink acceptance
remains disabled. They cover sink/mix cache separation, undo during sink removal,
correct final sink acknowledgement preserving the microphone, strict sink status,
and wrong-kind/wrong-revision readiness rejection. Existing runtime recovery,
lost-wake/retry and shutdown regressions also pass. Device operations in these
fixtures are simulated. No new fixture directories remain.

All nine changed source hashes match the immutable snapshot; exact binaries,
logs and outcomes are recorded in source-r294-verification.json,
source-r294-tests.json and typed-broker-r294-summary.json under artifacts/windows.
No deployed service, driver or user database was changed. Actual render-only
provisioning/readiness, maintenance handling and runtime routing leases remain
required before enabling sink requests or claiming production mute-original.

### R295–R297 — render-only setup path and endpoint readiness

Setup now has dedicated upsertAppRoutingSink/finalizeAppRoutingSink functions.
They use the separate authenticated owner sink identity and fixed stereo 48 kHz
configuration. The common adapter upsert retains existing package/owner checks,
per-node binding and created-node cleanup. Sink setup inspects the protected
published INF before registering a node and currently accepts only the supported
0.4.0.18 package. The Dell's installed 0.4.0.17 package (oem47.inf) is explicitly
rejected before device registration; no sink was created on the old driver.

recordAppRoutingSinkConfiguration initializes a missing record only for a newly
created node. Existing nodes must already carry the exact supported sink record.
It preserves pending state until finalization and rejects wrong roles, unknown
configurations, query errors and malformed pending values before writing. Normal
microphone setup continues to reject sink records. Neither path converts an
existing adapter's role.

Endpoint finalization shares the existing format-reset/selection path but expects
one active render endpoint and no active microphone for a sink. It also enumerates
all Windows endpoint states and rejects any identified capture endpoint with the
sink's hardware-derived identity. Full-state enumeration errors fail readiness;
ancestry lookup can include phantom nodes. Three stable matching format polls are
required before clearing pending state. This code is compiled but has NOT yet
been exercised against a live render-only adapter; absence of every possible
inactive endpoint is not established by these registry tests.

The final immutable source-r297 x64 Release snapshot builds setup and embedded
broker. Its native device-operations test passes (0.05 seconds), including new
record initialization/retry/idempotence, malformed pending refusal, role-change
refusal in both directions and request preflight. No private registry fixtures
remain. R295 caught a shadowed test variable under /WX. R296 caught an incorrect
test expectation: a missing normal format record already means 48 kHz stereo and
need not request a restart. Only the test source changed after R295; setup behavior
was unchanged. Failed logs are retained alongside the corrected result.

Seven changed source hashes match the snapshot; binary hashes, old-package
preflight, cleanup and build/test logs are in source-r297-verification.json,
source-r297-registry-cleanup.json and sink-setup-r297-summary.json under
artifacts/windows. No app/service/driver was deployed and no PnP node changed.
Sink-specific removal, maintenance integration, broker enablement and live .18
sink/microphone regression testing remain before runtime lease integration.

### R298 — sink removal, maintenance and adapter dispatch

removeAppRoutingSink derives only the authenticated owner's separate sink key.
It requires LoopZ hardware, the matching protected owner and the exact supported
render-only record before removing an existing node. A missing node is idempotent
success. Normal mix removal keeps its original identity and authorization path.
The sink upsert now supports explicit maintenance package migration, while
ordinary broker setup still refuses package changes. New sink device descriptions
identify app routing rather than a microphone.

The Windows broker adapter validates typed payloads before its setup lock and
then dispatches sink create/finalize/remove through the dedicated functions.
Maintenance selects sink roots by sink identity and uses sink finalization;
uninstall's desired-reference check uses the same typed root identity. It no
longer attempts to derive a normal mix key from a sink's empty public ID.
Public DeviceIntentStore acceptance still rejects sink creation/removal pending
live-driver acceptance and runtime lease integration.

Exact source-r298 x64 Release builds setup, embedded broker and regression
fixtures. The native device-operations test passes (0.06 seconds). Protected
store/status, typed reconciliation and broker runtime fixtures pass in 0.493,
0.385 and 0.276 seconds respectively; no new test directories remain. Added
production-adapter preflight cases refuse malformed sink payloads and package
paths before registration, and sink removal rejects noncanonical owners. Sink
reconciliation's positive device operations are simulated. No actual sink,
package migration or sink removal has yet been exercised on the loaded driver.

Seven changed source hashes match the immutable snapshot; six executable hashes,
logs and outcomes are retained in source-r298-verification.json,
source-r298-tests.json and sink-lifecycle-r298-summary.json under artifacts/windows.
No installed app, broker, driver or saved mix was changed. The next acceptance
step requires the .18 render-only driver candidate and live sink creation,
render-only endpoint verification, cleanup and normal-microphone regression.

### R299–R305 — installed .18 driver, live sink failure and normal-mic regression

The exact unsigned R289 SYS hash was verified, then a new .18 package copy was
signed with the existing non-exportable workbench certificate. SYS and catalog
Authenticode checks are Valid with the unchanged certificate thumbprint. This
remains development signing, not Microsoft production signing. Test-signing and
autologon settings were not changed. R299's opt-in live fixture first proved that
old oem47.inf is rejected before registration, with no sink root remaining.

R300 backed up the existing stage and broker, recorded saved state/defaults,
gracefully closed the app, and used the exact R298 native helper to install the
new package and matching broker. Maintenance migrated one desired root and
returned 0 with no reboot. The loaded .18 SYS is verified through PSAPI module
path enumeration and SHA-256:
9EDC0D5E8E6B09E14A25F53C6782E8A4AC0C89E7B45F7F8A9D323BBA38F0224A.
The installed broker hash is
1C2CFC35A759E8D7A19B0F6B213EEC32A18B484F918553AF69881E2F5B05F9AB.
The original R246 desktop/supervisor/engine relaunch non-elevated and reconnect
the saved logical mix with float transport v3 and both experiment flags OFF.
The saved document hash and all six audio defaults are preserved. Windows did
recreate the microphone endpoint: its current UID is
{0.0.1.00000000}.{6e0df037-f955-423e-bbc4-0624364952c3}.
External recording-app selection persistence across upgrades remains unresolved.

The live sink fixture refuses a pre-existing sink, creates only the authenticated
owner's sink, finalizes it, opens/stops a silent 48 kHz stereo shared render
client, checks idempotent setup preserves its endpoint, and removes only that
new sink. It also checks repeated removal, prior LoopZ endpoint identities and
all defaults. R301 timed out in format readiness; cleanup succeeded. R303 passed
all phases with exactly one identified render endpoint and no identified capture
endpoint across all states. R304 repeated the same code and timed out again;
cleanup again succeeded. This is intermittent behavior, not a fixed issue.

R303's diagnostic fixture captures raw Windows endpoint inventories on failure.
R304 shows the root started and Windows created a new ACTIVE render endpoint,
{0.0.0.00000000}.{40c0806c-5af0-435b-9184-d90b7b138d2a}, which the hardware-identity
inventory omitted. The next investigation belongs in topology/interface ancestry
lookup, not a speculative kernel change or larger readiness timeout. The first
R302 diagnostic build caught an implicit wchar_t-to-char conversion under /WX;
R303 uses checked UTF-8 conversion and builds successfully. Exact source/binary
hashes and raw reports are retained. Public sink acceptance is still disabled.

R305 then ran the isolated legacy private-v2/PCM16 client fixture against .18.
All five phases pass: signal, stop silence, restarted signal, disconnect silence
and reconnected signal. Independent report inspection verifies signal amplitudes
near 0.025, no signal zero runs longer than one frame, and exact silence in both
silence phases. The temporary normal-mix root was removed; original saved mix
microphone IDs, defaults and document bytes match before/after. This proves
backward client compatibility, not new-driver full format/float precision or
selected-app end-to-end acceptance. Final PnP inventory contains only the original
mix root. The three completed R300/R305 scheduled test tasks were removed.

Evidence under artifacts/windows: driver0418-sign-r299.*, driver0418-loaded-r300,
maintenance-r300-*, desktop-r305-current-installed-verification,
source-r303-verification/build, sink-r301/r303/r304-live, private-r305-report and
verification, sink-r305-final-roots and render-only-r305-integration-summary.
Full port completion remains false; sink identity reliability, production routing
leases, intermittent FIFO underruns and the remaining platform/release gates
remain open.

### R306–R312 — reproduced missing topology identity with working render format

Extended opt-in sink diagnostics now record raw endpoint state, typed instance
and container properties, every connector's type/connection state, interface
property queries and parent ancestry, registered KSCATEGORY_AUDIO interfaces for
the exact sink root, and read-only IAudioClient activation/GetMixFormat results.
No stream is started on unidentified endpoints. The existing live fixture still
removes only its initially absent, newly created sink and checks preservation.

R306 passes; R307 and R308 fail readiness. R310/R311 pass; R312 fails readiness.
All six runs clean up successfully. R307 captures S_OK topology activation and
connector acquisition, followed by GetDeviceIdConnectedTo returning 0x80070490.
The endpoint's PKEY_Device_InstanceId is VT_EMPTY. R308 confirms there is exactly
one connector, type 2 (Physical_External), with IsConnected=false. Both endpoint
and root container IDs are {00000000-0000-0000-FFFF-FFFFFFFFFFFF}, the same
computer-wide container also used by the Dell physical audio adapter. Container
matching therefore cannot uniquely identify this sink and is not implemented.

R312 narrows the failure further: both root-specific TopologySpeaker and
WaveSpeaker KSCATEGORY_AUDIO interfaces are present, and the newly ACTIVE render
endpoint activates IAudioClient and returns S_OK for a 48 kHz stereo mix format.
The root is started. Thus the captured failure is not explained by a missing
registered filter or unreadable format; the topology connection used by the
inventory is missing. The native kernel endpoint-setup code also has error
propagation worth auditing, but no evidence establishes that as this failure's
cause, and no speculative kernel fix is deployed.

Microsoft documents that GetDeviceIdConnectedTo can return E_NOTFOUND for an
unconnected connector or when the other side is not another topology. Device
properties also define container IDs as physical-device grouping, not a unique
endpoint key. References:
https://learn.microsoft.com/en-us/windows/win32/api/devicetopology/nf-devicetopology-iconnector-getdeviceidconnectedto
https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-properties
https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-containerid

No production identification fallback, label matching, opaque endpoint-ID parsing,
container reassignment or timeout expansion was added. A candidate next direction
is a driver-published, versioned endpoint identity, checked against the actual
LoopZ root/hardware and endpoint flow. Before relying on such metadata, verify
that Windows publishes it consistently and that an unprivileged process cannot
forge it through the property store. This remains proposed, not implemented or
verified; public sink acceptance remains disabled.

R309 introduces a dedicated task-owned acceptance-workspace to reuse compilation
outputs between future diagnostic revisions. Immutable source archives/manifests
remain separate. All 975 source files were checked against source-r309-hashes
before copying the exact executable to acceptance-binaries-r309; its hash,
architecture/configuration and archive/manifest hashes are recorded in
source-r309-verification.json. No later source edit can replace that retained
executable. R306/R308 source and executable hashes are recorded separately.

Final installed-runtime verification passes after all attempts: R246 non-elevated
app, R298 broker, loaded .18 driver, saved document/defaults intact and the saved
mix connected with experiment flags OFF. No product binary was changed this
round. Evidence: source-r306/r308/r309 verification/build records,
sink-r306/r307/r308/r310/r311/r312-live, desktop-r312-current-installed-verification
and sink-identity-r312-diagnostics-summary under artifacts/windows.

### R313–R323 — metadata prototype and independent PnP-parent candidate

The .19 prototype publishes a private, versioned instance/version/flow tuple in
its own topology interface EP store before PortCls enables the interface. R313
caught a const-pointer mismatch with ZwSetValueKey; R314 corrects the local
variable and passes WDK x64 Release with zero warnings/errors (8.78 seconds).
Source hashes were verified before signing with the unchanged workbench
certificate. R316 native maintenance migrated the original root, returned 3010,
and required a reboot. The staged payload was updated and a temporary limited
user logon task relaunched the original app after the configured autologon.
Loaded-module path/hash verification proves .19 is loaded:
50628DFF9427135A0BF1B099C75BEEEC7AC8E6C9DD51C4409EFD244BCE1BAD5F.
The broker remains exact R298 and the app remains exact R246, non-elevated.

Saved document bytes and all six defaults are preserved. Windows recreated the
original mix microphone again; current UID is
{0.0.1.00000000}.{bba000b1-5617-4841-889c-473fd0636254}.
R322's normal legacy private-v2/PCM16 client regression passes all five phases
against .19 and removes its isolated root. Before/after microphone identities,
defaults and document bytes match within that run. This does not establish
new-driver float/full-format or third-party-client acceptance.

R317's sink lifecycle succeeds, but the read-only metadata probe sees VT_EMPTY
for all private fields on current MMDevice endpoints. Inspection of the two own
dynamic topology stores confirms the kernel wrote the correct instance string,
version 1 and flow values. Typed iteration also confirms the existing EP
association, event-driven flag, OEM format and OEM period are present and match
the static templates. An earlier one-off hashtable inspection reported a null
association; that incomplete observation is superseded by the typed value dump.
No INF path fix or selector rewrite was made. The metadata did not yield a
usable endpoint identity; write-protection testing was not reached and the
properties are not used to authorize routing. Do not claim this approach fixed
identification. Retire the unused prototype before stable delivery.

The independent alternative uses Windows' AudioEndpoint software-devnode
namespace. PnP exposes the correct protected parent for both original mix
endpoints, even when the MMDevice instance property is empty. R319 enumerates
render/capture device interfaces through CM APIs and obtains canonical software
instance IDs and real parents. Passing those interface paths directly to
IMMDeviceEnumerator::GetDevice returns E_INVALIDARG on this Windows 10 build;
that conversion is not used.

The R323 candidate treats the complete Core Audio endpoint ID as an opaque
component in the Windows-specific SWD\\MMDEVAPI namespace. It does not parse
UUID/flow fields, match labels, read undocumented MMDevice registry paths, or
match the ambiguous computer container. It asks PnP to verify the canonical node
(case-insensitive device-instance comparison), AudioEndpoint setup-class GUID,
the matching render/capture interface, and actual Root\\LoopZAudio hardware
ancestry. Conflicting positive LoopZ identities are refused. Legacy topology
lookup and classification of legacy LoopZ roots with no durable mix key remain.
The bounded namespace mapping is a Windows-specific compatibility assumption;
unknown forms fail the parent path rather than authorizing a different device.
Windows 11/clean-system acceptance is still required.

R320 initially encountered the installed SDK's missing AudioEndpoint class GUID
symbol. R321 uses the explicit stable class GUID and builds. Its live fixture
passes eleven consecutive create/format/silent-render/retry/remove cycles. The
fixture now requires exactly one ACTIVE render endpoint and no capture endpoint
in any identified state; inactive render history is allowed because Windows may
retain it after removal. Readiness timeout and audio buffering were not expanded.
Three old-lookup R315 controls on the same .19 driver also pass, so these runs
alone do not establish causal ablation of the old intermittent failure.

R323 adds control-only identity provenance and a live assertion that the PnP
path itself independently recognizes the sink. That assertion and the complete
lifecycle pass, with parentIdentityVerified=true and topologyIdentityVerified=true.
The namespace/class/flow/hardware path therefore actually works rather than
being an unused fallback masked by legacy success. The installed app does not yet
contain this change. Desktop, control tool, setup/embedded broker and two focused
test executables build; device operations, driver identity and isolated runtime
CTest regressions all pass (0.54 seconds total). Six consumer executables are
retained separately with hashes; none is deployed.

R315/R319/R321/R323 acceptance snapshots verify all source hashes before copying
immutable executables from the task-owned reusable checkout. All live sinks and
the R322 temporary normal root were removed. The completed R316 close/logon and
R322 test task registrations were removed after final runtime verification.
Evidence includes driver-r314 and driver0419-sign/loaded-r316 records,
maintenance-r316 records, metadata-r318 typed interface/template snapshots,
pnp-r319 inventories, sink-r321-live/repeats/old-lookup controls, sink-r323-live,
private-r322-report, source-r323-consumers build/verification and
pnp-parent-r323-integration-summary under artifacts/windows. Full completion
remains false: prototype cleanup, deployed parent lookup, production routing
leases and the broader acceptance/release gates remain open.

### Current workbench integration: R392 / R394 (2026-10-23)

R392 enables ordinary Windows application playback edits behind the native
`appOutputRouting` capability. The interface distinguishes requested routing
from confirmed routing and hides the control with older backends. Requested
playback status remains visible at compact widths. Muting a selected app in a
mix retains its capture and original-output routing; unmuting resumes the mix
without releasing or reapplying the recovery receipt.

The exact snapshot passes 22 native Windows tests and 72 interface tests.
Browser fixtures check the real edit command, pending/confirmed labels, legacy
capability gating, and dialog bounds at 1100×720. These fixtures do not prove
installed audio. Separately, the Dell normal-token native fixture passes
successful two-instance routing, virtual-only public-microphone recording,
mute/unmute, and restoration. Each recorded state covers 24,000 frames after
warm-up: mute is exact zero, unmute restores both selected tones near 0.03
amplitude, with no recorded discontinuities or timestamp errors. The receipt
revision remains unchanged through both edits. The automatic output movement
uses silent streams, followed by explicit virtual-only tone streams; this does
not establish uninterrupted audio through an automatic stream handoff.

R394 deploys the verified R392 desktop/control binaries and freshly built UI to
the Dell preview. The normal-user desktop and engine reconnect to the existing
public microphone. Saved document bytes, default audio roles, the installed
R391 broker/helper, and microphone identity are preserved. The actual Russian
Windows interface is captured and inspected. Five idle GPU samples of the
owned WebView processes report zero for every observed engine counter. This is
an idle-counter result, not active-audio performance or a power measurement.
The previous stage is backed up, temporary tasks are removed, and test signing
remains enabled as requested.

Exact source archives, executable hashes, native/browser results, cleanup and
installed verification are retained in `artifacts/windows/app-controls-r392`
and `artifacts/windows/app-deploy-r394`. Full completion remains false. The
installed controller's complete first-use store → broker sink → capture →
playback routing/recovery path still needs end-to-end acceptance. Current
float32 format/continuity, process lifecycle, hardware/third-party clients,
clean Windows 10/11 installation/update/repair, and production signing also
remain open.

### Installed playback routing and lifetime: R395–R399 (2026-10-23)

R396 fixes a lifetime issue exposed by the installed integration test. Windows
retains an inactive session owned by the engine after its clients close. The
engine now supplies a complete native inventory of its actual WASAPI render
endpoints. Only its own exclusive inactive session may be discounted when this
inventory proves there is no local render client for that endpoint. Active,
shared, unknown and foreign live sessions still retain the sink. Callers without
this native inventory preserve the previous conservative behavior. The 22
native checks pass, including live foreign inactive-client and own/shared/active
regressions. R397 deploys these exact desktop/control binaries while preserving
the R391 service/helper, existing UI, microphone identity and saved bytes.

R395 and R398 failed in fixture cleanup: the fixture called the live process
identity accessor after deliberately terminating that process. The validation
correctly rejected the call. This does not establish an application source crash.
The exact retained marker was relaunched in silence on an identified virtual
Feed so the installed engine could finish its original receipt restoration.
The failed results, recovery and cleanup records remain preserved. R398 also
verifies removal of the released internal PnP root with the same running engine
and preservation of the recovery database.

R399 caches the authenticated application key before terminating the process.
Its complete normal-user installed acceptance passes: ordinary source edits
request playback routing; the installed owner loop and R391 broker provision the
typed account sink; both silent automatic source streams move to it; the engine
confirms capture and routing; explicit virtual-only tones are recorded from the
public microphone; ordinary mute/unmute edits produce zero and restored audio;
original output roles are restored; the receipt clears; the source is removed;
and the internal PnP root disappears automatically. Acquisition and recovery
use the same engine PID. The outer harness removes all three temporary mixes
and marker directories, then verifies original microphone selections, all six
default roles and byte-identical saved routing. The task is removed afterward.

Each R399 waveform covers 24,000 stereo float32 frames at 48 kHz after 9,600
warm-up frames, with zero recorded discontinuities and timestamp errors. Both
selected tones measure approximately 0.03 in each channel; the excluded tone is
below 5e-10. Muted output is exact zero. Automatic stream movement is silent,
followed by explicit virtual-only tone streams: continuous nonzero audio across
an automatic stream handoff is not proven. History already existed in this
passing run; the historical `durableHistoryCreated` report field checks database
presence, not fresh creation. A complete passing fresh-history scenario remains
open, as do process lifecycle/override/restart acceptance, the current full
float32 format and continuity matrix, long stress, hardware/third-party clients,
clean Windows 10/11 installation/update/repair and production signing. Full
Windows completion remains false.

Exact snapshots, binaries, hashes, reports and restoration evidence are in
`artifacts/windows/installed-routing-r395`, `sink-observer-r396`,
`app-deploy-r397`, `installed-routing-r398` and `installed-routing-r399`.

### High-channel continuity remains open: R401–R404 (2026-10-23)

R401 repeats the 60-channel, 44.1 kHz float32 exclusive virtual roundtrip for
60 seconds using the installed R396 app. Saved mixes remain active and all
experimental timing flags are off. It fails with 7,090 all-channel zero frames,
three driver underruns, one private-worker restart and a 30.2211 ms missed
deadline. The publication missing-frame counter does not increase between the
within-recording observations in this run. The temporary mix/root is removed;
saved document bytes, microphone identity and defaults are preserved.

R402 prepares an optional AVX2 dot-pair kernel outside the render callback.
Only its separate implementation file is compiled for AVX2, with strict floating
point. Selection uses the Windows CPU/OS instruction-availability check; the
existing SSE2 implementation remains the fallback. Microsoft's
[feature-detection contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-isprocessorfeaturepresent)
describes the Windows 10 feature query. The prototype passes 22 Windows native
checks and the local portable and sanitizer resampling/monitor checks. Three
Dell offline trials each compare 32 shapes: optimized and SSE2 output hashes
are identical for every sample, and the mean of the trials' median time ratios
is 0.8325 versus the old implementation. One shape in the first trial is slower;
this is not a claim of uniform speedup or live-audio continuity.

R403 deploys the verified R402 desktop/control binaries with rollback backup.
The existing R391 service/helper, UI and saved-state baseline are preserved.
R404 repeats the unchanged live recording and still fails: 7,987 all-channel
zero frames, two driver underruns, a 34.6051 ms deadline miss, one worker restart,
and a publication missing-frame delta of 3,157. Capture packet position,
discontinuity and timestamp-error counters remain zero. The offline CPU saving
therefore does not establish a successful continuity fix. All temporary roots
and tasks are cleaned up, and installed state is verified afterward.

The next investigation is independent rate domains for disconnected virtual
mix graphs, rather than forcing every mix through the highest global rate.
Connected virtual dependencies must retain a shared rate and full bandwidth;
saved mixes must continue running. This has not been implemented or accepted.
A local header follow-up removes Windows dispatch overhead from other platforms
and keeps the SSE2 fallback inline; its portable/sanitizer checks pass, but its
Windows build remains pending. The Dell still runs the immutable R402 snapshot.
Evidence is retained in `artifacts/windows/format-r401`, `fir-avx2-r402`,
`app-deploy-r403` and `format-r404`. Full Windows completion remains false.

### Independent render rates: R405–R408 (2026-10-23)

R405 partitions the Windows document by connected virtual-device dependencies.
Disabled bindings remain part of the connectivity graph. Each component uses
its maximum nominal rate to preserve bandwidth; disconnected components with
the same rate share a domain. One serial render thread steps retained schedules
for all domains. Original document indices still map to the correct scene,
sources and meters. Bus sinks, private transports and hardware monitors use the
corresponding device domain rate. Normal publication cadence, frame buffers and
time budgets are retained; disconnected saved mixes continue running.

The prepared-domain tests verify independent rates, disabled/transitive edges,
full nominal bandwidth, original index mapping, retained clocks and actual
multi-domain/cross-device waveforms without render allocations. Five focused
portable tests and their sanitizer counterparts pass; 27 Windows native checks
also pass. R406 deploys the verified R405 desktop/control build while preserving
the R391 service/helper, interface, microphone identity and saved-state baseline.

R407 repeats the previously failing 60-channel float32 44.1 kHz exclusive
virtual roundtrip for 60 seconds. It passes with zero all-channel zero frames,
capture discontinuities, position gaps, silent frames and timestamp errors.
Within-recording publication missing-frame, driver-underrun, lease-silence,
worker-restart and deadline-miss deltas are all zero. The fixture confirms a
44.1 kHz render domain for the test mix while the disconnected saved mix stays
at 48 kHz. The temporary root/task is removed and document bytes, defaults and
saved microphone identity are verified afterward. This proves that particular
rate/channel/duration case, not general long-stress or hardware compatibility.
R408 is the separate 192-profile advertised-format sweep using 1.5-second
recordings; its final result is not established here.

A subsequent full portable run identifies the prototype's Windows feature probe
inside `native/core/cached_fir_kernels.cpp` as a genuine core-boundary violation.
The boundary rule is unchanged. The current follow-up moves CPU/OS detection
and the separately compiled AVX2 implementation into the Windows adapter; the
core retains only an SDK-free optional kernel setter/getter. Runtime prepares
the adapter before opening audio. Separate baseline and Windows-initialized
resampling suites are configured so both paths can be checked. All 58 portable
checks pass after the repair; Windows compilation of this follow-up remains
pending while R408 runs on the immutable R405 build. The passing R407 result
must not be attributed to the unbuilt follow-up.

Evidence is retained in `artifacts/windows/independent-rates-r405`,
`app-deploy-r406`, `format-r407` and `formats-r408`. Full Windows completion
remains false: final-source format/stress, active multi-mix and lifecycle,
hardware/third-party, clean Windows 10/11 installer and production-signing gates
remain open.

### Installed driver .21 and renewed continuity acceptance (2026-10-23)

The current Dell baseline is the normal Release R440 app, control, maintenance
helper and broker, with the exact development-signed 0.4.0.21 driver loaded.
R442 installed the matching set through the native maintenance helper; Windows
loaded the new image without a reboot. All 41 selected native checks passed.
The existing development certificate and test-signing configuration were retained.
The saved document bytes and all six default audio roles are unchanged, and the
desktop, background process and Engine run with normal user tokens.

The driver adds a validated, fixed-width integer-only float32 passthrough path.
R436/R437 compared 40 offline packet shapes, including unaligned and in-place
buffers, finite and invalid IEEE values, and independently checked output bits.
All output hashes matched in three trials; the median candidate/baseline time
ratios were approximately 0.186–0.190. This is a codec benchmark, not a whole-app
CPU claim. Inspection of the actual R438 WDK driver confirms the fixed byte loop
and absence of floating-point/vector instructions in the codec. Existing kernel
compiler mitigations remain enabled. R439 signed that driver with the existing
workbench certificate. R440 explicitly admits .21 for the render-only routing
endpoint and tests exact-version rejection as well as acceptance.

R445 repeats the previously failing exclusive float32 virtual roundtrip at
88.2 kHz / 54 channels for 60 seconds, with the saved 48 kHz mix still active.
It passes: no all-channel zero frames, capture discontinuities, position gaps,
silent frames or timestamp errors. All 99 within-recording observations report
zero publication missing frames, driver underruns, lease-silent frames, worker
restarts and deadline misses. Cleanup and normal installed state are verified.
Startup/teardown counters remain in the raw report and are not represented as
recording failures or silently discarded. R443/R444 stopped during preflight,
before audio started: normal-user inspection of the protected broker binary was
denied. R445 keeps protected-binary verification in privileged maintenance and
performs the recording under the normal desktop token without weakening ACLs.

R446 passes installed-service render-only endpoint creation, idempotent replay,
stale/forged request rejection and removal under the normal user token. R448
passes ordinary installed app routing: two independent selected tones reach the
public microphone, the excluded tone remains absent, mute produces exact zero,
unmute restores both tones, preferences are restored, pending receipts return
to zero, and the internal sink is removed. Its history existed before the test;
automatic movement uses silent streams before explicit virtual-only tones.
This does not prove fresh-history first use or uninterrupted nonzero handoff.

The driver upgrade regenerated the public microphone's Windows endpoint UID.
The logical saved mix reconnects, but external applications may need to select
the microphone again. Endpoint identity and upgrade UX remain open work.
R441 also rechecked idle GPU use: all five samples of LoopZ and its WebView
descendants report zero utilization after the earlier pulsing-dot fix. That is
an idle observation, not a claim about interaction or power consumption.

Evidence is retained in `artifacts/windows/codec-baseline-r436`,
`codec-candidate-r437`, `driver-build-r438`, `driver-sign-r439`, `consumers-r440`,
`maintenance-r442`, `format-r445`, `broker-sink-r446` and `installed-routing-r448`.
R447 is the separate 192-profile float-format sweep; its completed result is
not established here. Full Windows completion remains false. The complete
current-source format/stress matrix, fresh-history and continuous-handoff
acceptance, lifecycle/recovery, hardware and third-party clients, clean Windows
10/11 installation/update/repair and production signing/distribution still
require evidence. The earlier R329 latency measurement is not a measurement
of this newly installed driver/app pair.


### R449 startup-error candidate (not installed)

While R447 continues on the unchanged installed .21 build, the local .22
candidate stops endpoint setup immediately when topology or wave installation
or caching fails. The original failure now reaches rollback instead of being
overwritten by a later successful lookup/connection. Render setup returns the
required installation result independently of optional ETW availability. The
sample-only power-control/resource registration demonstrations are removed.

A new test compiles the actual production installation method bodies against
PortCls doubles. It injects topology/wave creation, cache and connection failures,
including partial outputs, and checks rollback, cached-object ownership, COM
references, output interfaces and optional ETW behavior. The old common method
fails this test with "Installation failure was masked". Candidate Release and
ASan/UBSan runs pass; the exact-version adapter check also passes. These are
method-level fault-injection checks, not live kernel/PnP failure injection.

The candidate is frozen as `source-r449.tar.gz` with a full hash manifest.
Its WDK build and fresh R450 app/helper/broker build are prepared but have not
run, and .22 is not signed or installed. Builds and maintenance must wait for
R447's actual task to become terminal. The loaded Dell baseline and R445/R446/
R448 acceptance evidence still belong to .21/R440, not to this source candidate.
Evidence is retained in `artifacts/windows/startup-r449`, `driver-build-r449`
and `consumers-r450`.


### R451 first-use routing fixture (prepared, not run)

A normal-desktop lifecycle fixture is prepared for the R449/.22 candidate after
R447 finishes and the matching native maintenance is accepted. It requires no
pending receipts/targets, no account sink root, unchanged saved mix/defaults/
microphone, no other test/build, and an ordinary SQLite recovery database.
It preserves the original recovery folder by rename, including its byte hash
and ACLs, and checks that ordinary startup leaves the recovery root absent.
The installed routing request must then create history with the native report
explicitly recording `historyExistedBefore=false`, restore playback preferences,
release receipts and remove the internal sink. Only after those checks does it
retain the newly created store separately and restore the original folder.
Failure preserves both histories for recovery rather than replacing live receipts.

The prepared PowerShell parsed on the Dell without executing lifecycle actions.
The read-only SQLite validator passes eight local acceptance/rejection checks,
including pending leases/targets, wrong metadata, sidecars and linked paths;
all database bytes remain unchanged. These checks establish fixture guards,
not first-use runtime acceptance. R451 is not started while R447 runs. Evidence:
`artifacts/windows/fresh-history-r451`, including phase receipts once executed.


### R455 bounded stress recorder (not Windows-built or run)

The existing stored-sample recorder retains its allocation ceiling; it cannot
cover long 192 kHz / 64-channel recordings. An explicit diagnostic-only
`--format-stress-exclusive-float` mode now uses a preallocated 32-slot SPSC queue
and a background spectral/continuity analyzer. Sample storage is bounded at
32 MiB for 64 channels, independently of the requested 1.5–300 second duration.
The capture producer allocates neither memory nor locks and never waits for
analysis. Overflow explicitly fails the measurement. Completion drains every
accepted frame; accepted/analyzed counts and pending packets are reported.
Queue contents, phase, step and all-channel zero-run state survive packet and
ring boundaries. Existing stored/precision/PCM16/latency modes are retained.
This is a test-tool change, not an audio-engine or driver optimization.

Portable checks compare 54 rate/width/packet shapes against independent
trigonometric projections, exercise partial/multiple slots, invalid samples,
overflow, producer/consumer ordering and queue wrap, and process a synthetic
five-minute 192 kHz stereo sequence without retaining it. Four Release checks
pass (1.73 s); their ASan/UBSan counterparts pass (33.07 s). The concurrent
primitive also passes ThreadSanitizer with no reports. These do not validate
Windows WASAPI integration or actual five-minute audio continuity.

The long-recording auditor additionally verifies exact recorder counts, no lost
packets/invalid samples, bounded storage, full-window QPC/frame coverage, dense
counter observations, unchanged audio threads, no publication/driver/scheduler
failure deltas, unchanged render domains and normal worker settings. Synthetic
validator controls pass and reject forged healthy results, missing observations,
worker replacement, short recordings, overflow and changed settings. A single
stress run is explicitly not a substitute for the complete format matrix.

Source R455 freezes this separate probe candidate. Its R456 native build and
installed stress recordings remain pending until the original R447 sweep is
terminal and cleaned. R449/.22 driver and R450 consumers remain separately
frozen inputs; this diagnostic changes neither of those installed candidates.
Evidence is retained in `artifacts/windows/streaming-probe-r455`.


### R447 full matrix accepted; R450 fixture repair / R461 candidate

The terminal R447 run independently passes all 192 1.5-second exclusive-float
profiles on driver .21/R440, including 192 kHz / 64 channels. All aggregate
all-channel zero frames, capture gaps, discontinuities and timestamp errors are
zero. The saved 48 kHz mix stays active, saved bytes/defaults/microphone are
preserved, and the temporary adapter, mix and scheduled task are removed.
Evidence: `artifacts/windows/format-r447/summary.json` and raw reports.

R449's .22 WDK build passes in 8.61 s with zero warnings/errors. Its complete
553-byte integer codec is byte-identical to the accepted .21 build; full static
inspection finds no floating-point/vector instructions and the existing hardening
flags remain enabled. R452 signs the SYS/catalog using the unchanged FEC5 workbench
certificate, without altering boot configuration. No .22 driver is loaded yet.

R450's fresh app/helper/broker build stops because its actual-method test
fixture redefines `_In_`, `_In_opt_` and `_Out_opt_`, already supplied by the MSVC
runtime's SAL header. The failure is retained. Three conditional macro guards
repair only the fixture; warning strictness and fault-injection coverage remain.
Local Release and ASan/UBSan installation-method checks pass after the repair.

Source R461 combines that guarded fixture with R455's bounded recorder and
report audits. Its entire kernel source subtree is identical to R449's.
R462 is the new matching app/control/helper/broker/probe build, selecting the
previous 42 checks plus four recorder/limit/report checks. The unstarted R456
separate probe build is superseded. Prepared R451/R453/R454/R457–R459 fixtures
must pin the R462 binaries before execution; none establishes acceptance yet.
Full Windows completion remains false, including final-pair format/stress,
first use/handoff/lifecycle, hardware/third-party, clean Windows 10/11 installation
and production signing/distribution gates.

### R453/R451 installed .22, fresh history, and R454 latency

R462's fresh Windows consumer build passes all 46 selected checks. R453 installs
that matching app/control/helper/broker pair and the signed R452 driver .22
through the native maintenance helper. The loaded DriverStore image matches the
exact signed SYS; maintenance reports no reboot required. Normal desktop and
engine processes, service identity, float transport, saved bytes/defaults and
normal worker flags pass post-verification. Test-signing remains enabled on the
user's Dell workbench. The public microphone endpoint UID changes across this
upgrade; external-app reselection and upgrade UX remain an open requirement.

R451 passes ordinary installed application routing with the recovery history
initially absent. It creates durable history, routes the independent test
processes, checks mute/unmute and excluded markers, restores original endpoint
preferences, releases the receipt and removes the internal sink. The newly
created empty database is retained separately; the original empty database and
ACLs are restored. Final lifecycle is complete with cleanup true and no recovery
required. The saved document remains byte-identical. This does not prove
continuous nonzero handoff, crash recovery, sleep or hardware replug behavior.

R454 measures the installed R462/.22 virtual feed → half-gain Engine → public
microphone path at 48 kHz stereo exclusive PCM16 over 20 markers. Median is
76.52275 ms, minimum 66.5901 ms and maximum 77.66645 ms; capture position gaps,
discontinuities and timestamp errors are zero. This includes WASAPI client,
Engine and driver buffering plus receiver scheduling; it excludes selected-app
capture, physical hardware, acoustics and network paths. It is a measurement of
this path, not general end-to-end application latency. Terminal task result is
zero, temporary resources are cleaned, and saved bytes/defaults/microphone
identity pass post-verification. Raw evidence is in
`artifacts/windows/latency-r454` and `artifacts/windows/fresh-history-r451`.

R463's 15 fresh idle GPU samples report zero percent on all identified desktop
and WebView GPU engines. This does not exclude interaction-dependent spikes or
establish macOS GPU behavior. Full Windows completion remains unproven.

R457's live bounded-recorder pilot passes 15 seconds at 48 kHz stereo on the
installed R462/.22 pair. Independent streaming audit accepts 24 counter
observations and 1 MiB sample storage; all accepted frames are analyzed with no
pending packets, drops or invalid samples. Terminal result is zero; cleanup and
post-verification preserve saved bytes/defaults and microphone identity.
Evidence: `artifacts/windows/stress-pilot-r457`. R458's separate five-minute
192 kHz / 64-channel test has started; no stress acceptance is claimed yet.

### R458 sustained maximum-format recording fails

The terminal five-minute 192 kHz / 64-channel exclusive-float recording fails.
All 57,600,000 accepted frames are analyzed using 32 MiB sample storage, with zero
recorder drops, pending packets or invalid samples. WASAPI capture position gaps,
discontinuities and timestamp errors are zero. The waveform nevertheless contains
476,610 all-channel zero frames, longest run 158,848 frames (about 827 ms), and
failed gain/step measurements. This is a real continuity failure, not an accepted
stress result.

Across 499 in-recording observations, publication missing frames increase from
0 to 422,964; driver underruns increase from 0 to 3 and lease silence from 0 to
32,512 frames. One worker deadline miss/restart occurs about 50 seconds after the
first observation. Publication misses begin around 13 seconds, before that restart.
Render maximum lateness reaches 83.05 ms, while the maximum render call reaches
5.98 ms. Those observations identify late publication and recovery effects but
do not establish a root cause. No buffer/deadline setting is relaxed.

The task is terminal with result 1 and removed; no probe/test process remains.
Temporary mix/root cleanup succeeds, and post-verification preserves the exact
saved document hash, all defaults and the saved microphone identity. The R459
float sweep and R464 PCM16 sweep remain unstarted. R464's prepared scripts parse
on Windows and pin the R462 binary/fixture hashes, but preparation is not audio
acceptance. Evidence: `artifacts/windows/stress-max-r458/failure-summary.json`,
`counter-events.json`, raw reports and post-verification.

### R465 diagnostic CPU attribution; R466 symbols build

R465 records a separate 30-second 192 kHz / 64-channel bounded-float diagnostic
with owned xperf Profile stacks on the unchanged installed R462/.22 pair. The
recorder itself loses no frames; this profiled waveform also fails continuity.
Profiling can disturb deadlines, so its dropouts are not normal acceptance.
The trace is stopped and merged; analysis reports zero lost events/buffers and
pins engine PID 10844, render thread 10056 and worker 11068. The temporary task
and audio devices are removed, and normal installed state, saved bytes/defaults
and microphone identity pass post-verification.

The exact .22 PDB resolves driver code: convertPackedSamples accounts for 385
render-thread exclusive hits (1.43%) and 3,374 worker hits (15.85%). Most render
hits (76.29%) are unresolved in the symbol-free user-mode build; do not infer a
specific DSP function or system CPU percentage from those samples. Whole captured
thread lifetimes include setup/cleanup and profiler effects. This trace does not
justify another driver codec change without resolving user-mode work first.

R466 is an isolated diagnostic build from unchanged frozen source R461, retaining
Release /O2 /Ob2 and adding /Zi plus full optimized link symbols. Its build and
same 46 checks have started; it is not deployed or accepted. Production .22/R462
remains installed. Evidence: `artifacts/windows/cpu-r465` and
`artifacts/windows/symbols-r466`.

### R466/R468 resolved FFT hot path; normal app restored by R469

R466 completes with all 46 selected Windows checks passing. Its complete frozen
R461 source verifies before and after the optimized symbol build. The diagnostic
LoopZ executable SHA256 is 2D6EC5A55EEF8C557A43CFACA86BE995ECC95FE3A21602B857CA563F79E0ED75;
its matching PDB is retained. R467 temporarily deploys only the app/control with
the existing driver, service and interface retained. Normal runtime and saved
state pass before the separate R468 30-second diagnostic trace.

R468 has zero lost trace events/buffers and resolves the user-mode work. Render
thread exclusive samples include 4,405 hits in UnityConvolutionPlan::transform<1>,
4,334 in transform<0>, and 3,750 in its read function, together about 46% of that
thread's selected hits. The complete driver module accounts for 2.00% of
render-thread exclusive hits; its codec function is a smaller subset. These are captured-lifetime sample shares, including profiler
and setup/cleanup effects, not whole-system CPU or latency guarantees. The actual
MSVC transform disassembly uses scalar double adds/multiplies despite the paired
real-channel convolution. Benchmarking packed-vector complex arithmetic is the
next focused candidate; no numerical or production optimization is claimed yet.

The trace is stopped/merged, the diagnostic audio task is removed, and R469
restores the exact original R462 app/control. Post-verification confirms normal
three-process runtime, service, saved bytes/defaults and microphone identity;
close/launch tasks are removed. The .22 driver and test-signing remain unchanged.
R458's unprofiled sustained failure is still unresolved. Evidence:
`artifacts/windows/cpu-r468`, `symbols-r466` and `app-restore-r469`.

### R470/R471 FFT arithmetic candidate; R473 correctness / R475 build

R470's first SSE2 implementation preserves all 72 offline sample hashes but is
slower: three trial median candidate/baseline time ratios are 1.2922, 1.2591 and
1.2650. It is rejected and retained as diagnostic evidence, never installed.
R471 keeps complex pairs packed through each FFT butterfly rather than converting
scalar structs between operations. Its 72 channel/frame/phase cases preserve every
sample hash in all three interleaved-order Dell trials. Median time ratios are
0.9474, 0.9489 and 0.9468 (about 5% shorter reads); individual ratios span about
0.879–1.017, so this is not a uniform or whole-engine speedup. SSE2 is baseline
x64; other architectures keep the scalar arithmetic. No filter support, buffering,
thread priority or deadline policy is changed.

The existing FIR-reference resampling and near-unity tests pass in local Release
(3.01 seconds total) and ASan/UBSan (117.45 seconds). R473 separately compiles the
candidate resampler on Windows against the unchanged frozen core dependencies;
both existing assertion-enabled numerical suites pass, including spectral quality,
gaps/tails/strides, extreme magnitudes, wide channels and no callback heap activity.
This isolated executable is not installed-application acceptance.

The new opt-in unity-convolution benchmark prepares its buffers outside timed
reads and emits per-case timing and output hashes. Frozen source R474 changes
only the FFT header, benchmark source and its CMake target relative to R461; the
entire driver subtree is unchanged. R475's fresh normal Release app/tool build
and all 46 selected checks have started. It is not deployed. The original R462/.22
app remains installed, and R458's sustained continuity failure remains unresolved.
Evidence: `artifacts/windows/fft-r470`, `fft-r471`, `fft-r473`, source manifests and
`consumers-r475`.

### R475/R476 installed FFT candidate; R477 screen fails

R475's normal Release build verifies all frozen R474 source inputs before and
after compilation and passes all 46 selected Windows checks. R476 preserves the
previous stage and deploys only its exact app/control binaries. Driver .22,
R462 helper/broker/interface, audio settings and normal worker flags are retained.
Post-verification confirms saved bytes/defaults/microphone identity and ordinary
non-elevated app/engine processes; update tasks are removed.

R477's unprofiled 60-second 192 kHz / 64-channel screen fails: 76,770 all-channel
zero frames, longest run 18,487 frames (about 96 ms). Every accepted frame is
analyzed; recorder packet drops, pending packets, invalid samples and WASAPI
position/discontinuity/timestamp errors are zero. Within-recording observations
show 32,178 publication missing frames and one worker deadline miss/restart.
This does not resolve maximum-format continuity. A one-minute result is not
comparable evidence of improvement over R458's five-minute result. R478's prepared
five-minute run is not started after this failed screen.

The terminal screen task is removed and temporary resources cleaned. Saved
bytes/defaults/microphone identity and normal runtime again pass post-verification.
Separate analysis of the exact runtime DLL export RVAs maps the R468 hot
classification addresses to _fdclass/_fdtest; the trace's callers are principally
gain/limiter processing. That is the next independent optimization target, not
an adopted change or established sole cause. Evidence: `artifacts/windows/consumers-r475`,
`app-deploy-r476`, `stress-screen-r477` and
`cpu-r468/runtime-classification-attribution.json`. Full Windows completion is false.

### R479 gain/limiter classification candidate; R481 build

The pinned SingZ overlay now classifies IEEE float encodings in finiteSample
instead of making a per-sample CRT call. Finite encodings, including negative
zero and subnormals, are returned intact; infinities/NaNs still become positive
zero and increment the same saturating diagnostic counter. Vendor sources remain
unchanged. Both input and generated output remain hash-pinned; generated builtin
source SHA256 is b32d465b277ad4544856373510456143f5f9e5d1e39cd0b6573a06b28b5ab7ea.

The common helper needs an independent oracle because both processing paths use
it. Expanded tests compare 263,168 sampled encodings across every exponent and
both signs with the CRT classification, at three initial counter values. Existing
exact gain/limiter traversal/automation/counter comparisons remain. The planar
overlay now also has reproducibility, changed-SDK rejection and tampered-output
rejection tests. Local Release containment/planar/overlay checks pass (0.63 s);
ASan/UBSan counterparts pass (1.09 s). Actual assertion-enabled Windows candidate
numerical checks pass separately.

R479's offline Dell benchmark compares 36 gain/limiter width/frame/invalid-input
cases in three interleaved-order trials. Output hashes and non-finite counters
match in every case. Median candidate/baseline time ratios are 0.2550, 0.2540 and
0.2590, about four times faster for these isolated processing functions. One
baseline timing outlier widens an individual ratio to 0.0679; it is not a claimed
uniform speedup. These measurements do not prove whole-app CPU, latency or live
continuity improvement.

Frozen R480 changes only the overlay, its tests, the opt-in benchmark and CMake
target relative to R474; the FFT candidate and driver subtree remain unchanged.
R481's fresh normal Release app/tool build and existing 46 selected checks have
started. The installed app remains R475 with driver .22 and R462 helper/service;
R477's sustained screen failure remains unresolved. Evidence:
`artifacts/windows/classification-r479`, source manifests and `consumers-r481`.

### R481/R482 installed classification candidate; R483 driver underrun

R481 completes its frozen R480 source verification and all 46 selected Windows
checks. R482 preserves the previous stage and updates only the exact app/control
binaries; driver .22 and R462 helper/broker/interface remain. Normal non-elevated
runtime, saved document bytes, all defaults and microphone identity pass.

R483's unprofiled 60-second 192 kHz / 64-channel recording still fails. It contains
3,229 all-channel zero frames in a single run (about 16.8 ms); all 11,520,000
accepted frames are analyzed with zero recorder drops, pending packets or invalid
samples. WASAPI position gaps/discontinuities/timestamp errors are zero. Within
the observed recording publication missing frames, worker deadline misses, worker
restarts and render skipped frames all remain unchanged at zero. This is not an
accepted stress result or a claim of an overall dropout fix.

One observed driver underrun adds exactly 3,229 missing frames, matching the
recorded silent run. It occurs around 11.4 seconds after the first observation.
At the later snapshot, render maximum lateness is 5.18 ms and worker maximum
lateness 12.15 ms; the worker's maximum render interval is 12.22 ms. Those
maxima do not identify the exact blocking call. Ingress/source missing counters
and outgoing-zero counters do not change during the observed window. The next
remaining investigation is private-worker/driver-queue scheduling, without
relaxing buffering or deadline policies.

The terminal task is removed, temporary resources are cleaned and saved
bytes/defaults/microphone identity again pass post-verification. The prepared
five-minute R484 and full float/PCM16 R485/R486 sweeps remain unstarted. R487's
separate read-only auditor now explicitly distinguishes public float32/PCM16
while requiring the existing private float transport. Ten synthetic controls
pass, as do repeat read-only audits of accepted historical R447/R457 reports;
this is no new live PCM16 acceptance. Evidence: `artifacts/windows/consumers-r481`,
`app-deploy-r482`, `stress-screen-r483` and `format-audit-r487`.

### R488 scheduling trace; R490/R493 paired-flow investigation

R488 captures CSwitch/ReadyThread/DPC/Interrupt and loader events on the unchanged
R481/.22 baseline, without sampled CPU or stack walking. Trace events/buffers lost
are zero and independent switch-pair validation reports no invalid pairs. The
render thread's largest off-CPU interval is 2.194 ms and ready delay 1.907 ms;
worker values are 3.293 ms and 1.284 ms. Neither has an observed off-CPU gap over
10 ms. Scheduled residency includes DPC/interrupt time and is not CPU execution.
The largest individual observed DPC is 3.684 ms in nvlddmkm.sys. These results do
not establish scheduler starvation or that graphics activity caused the underrun.
The traced waveform fails and is diagnostic only. The owned trace and task stop;
saved state and normal runtime pass afterward.

The xperf export uses the Dell's measured ANSI code page 1251. Its original CSV
is preserved; strict reversible conversion produces a separate hashed UTF-8 copy
for parsing. No capture is restarted to repair an export-encoding failure.

R489 temporarily enables only process-local stream chunks, without changing the
10 ms batch grid, 20 ms prefill, buffers, priority or deadlines. R490's unprofiled
60-second maximum-format run still fails: 3,558 all-channel zero frames match
3,558 publication-read missing frames. No driver underruns/missing frames, worker
misses/restarts or recorder drops are observed. Chunk submission alone is not
adopted.

R492 then temporarily enables the existing bounded publication wait alongside
chunk submission. R493's single 60-second 192 kHz / 64-channel run passes with
zero all-channel zero frames, capture gaps/discontinuities/timestamp errors,
recorder drops, publication-read missing frames, driver underruns/lease silence,
and worker deadline misses/restarts. Its independent profile/spectral check
passes; 99 within-recording observations verify both options and the unchanged
1920-frame batch. The window records 1,681 bounded waits and zero wait timeouts.
This is one experimental run, not normal-mode, full-matrix, five-minute or latency
acceptance. Long stress, error paths and current measured latency are required
before a production flow policy can be adopted.

R491 restores the original normal app launch with all experiment options off.
Saved bytes/defaults/microphone identity and normal runtime pass; temporary audio
and close/launch tasks are removed. No binary, driver, global environment or boot
setting changes in this investigation. The installed app remains R481/.22 with
normal defaults, whose R483 failure is still open. Evidence:
`artifacts/windows/scheduling-r488`, `chunks-test-r490`, `flow-test-r493` and
`chunks-restore-r491`.

### R495 five-minute paired-flow pass; R497/R498 matched latency

On the unchanged R481/.22 app/driver, R494 temporarily enables the paired options
for a five-minute 192 kHz / 64-channel run. R495 finishes with task result zero,
cleanup true and all 57,600,000 frames accepted/analyzed using 32 MiB sample
storage. Whole-recording all-channel zero frames, capture gaps/discontinuities/
timestamp errors, recorder drops/invalid samples/pending packets are zero.
Independent audit accepts 499 dense within-recording observations with unchanged
threads/rates, no publication/driver/worker/scheduler failure deltas, unchanged
1920-frame batch, and both paired options explicitly enabled. The final state
records 8,253 waits, zero wait timeouts and maximum wait 3.3226 ms. Saved bytes,
defaults and microphone identity pass post-verification; temporary resources/task
are removed. This is experimental paired-flow acceptance for one format, not
normal-default or full-matrix acceptance.

R499 adds an explicit diagnostic-only paired-flow auditor mode. The normal mode
continues to reject experimental flags. The diagnostic mode additionally requires
both flags together, unchanged batch size, non-resetting exercised wait counters,
zero observed/final timeouts and a maximum wait within the existing 30 ms worker
limit. Eleven synthetic controls pass; historical default R447/R457 reports and
the earlier experimental R493 report pass their correctly scoped audits. No
report is rewritten to pretend its experiment flags were off.

R497 and R498 measure the same R481/.22 48 kHz stereo exclusive-PCM16 virtual
Feed → half-gain Engine → public microphone path, 20 markers per mode. Normal
median is 77.317775 ms (67.3188–78.55505); paired experimental median is
68.020075 ms (67.3695–78.0648). Both runs have zero capture position gaps,
discontinuities and timestamp errors. This single comparison shows no latency
increase in the measured path; overlapping ranges and receiver scheduling prevent
a general speedup guarantee. The path excludes selected-app capture, physical
hardware, acoustics and network latency.

R496 restores the original normal launch, all experiment options off. Saved
bytes/defaults/microphone identity and normal runtime pass; all close/launch and
audio tasks are removed. The production default policy has not yet changed.
Remaining work includes a tested native adoption with failure/capability behavior,
current full float/PCM16 matrices, lifecycle/hardware/third-party/clean-OS and
production signing/distribution gates. Evidence: `artifacts/windows/flow-long-r495`,
`latency-normal-r497`, `latency-flow-r498/comparison.json`, `flow-audit-r499` and
`flow-restore-r496`.

### R500 native paired-flow candidate; R502 build

The worker now selects the paired publication policy natively for transport-v3
float publication histories at the normal batch cadence. Legacy transports,
non-history sources and the opt-in small-batch diagnostic retain buffered mode.
The old independent publication-wait/stream-chunk environment controls are no
longer read. The paired path uses the same tested chunk/wait algorithms, 10 ms
batch grid, 20 ms prefill, queue reserves and 30 ms worker failure limit.

Paired startup requires a successful queue-status query before initialization is
acknowledged. Loss of queue status during periodic observation or a publication
wait stops the worker with an error rather than continuing without a verified
reserve. Cancellation/failure is checked before further ingress work and within
wait loops. The publication clock origin is established after capability setup,
so setup time does not consume the first audio deadline. Existing runtime
recovery remains responsible for reconnecting the desired microphone.

Diagnostics expose publicationFlowVersion 1 for the native paired contract and
0 for buffered legacy delivery. Portable policy/queue-budget tests pass in
Release (0.54 s) and ASan/UBSan (5.80 s). Thirteen report controls distinguish
native mode from experimental launches; the actual passing R495 experimental
report is correctly rejected as native acceptance. Native matrix/streaming audits
require the version marker, both paired features, normal batch and valid wait
budgets. These checks do not prove live lost-queue recovery or installation.

Frozen R501 changes only the user-mode worker/policy/diagnostics, associated
policy test and report auditor/controls relative to R480; driver sources remain
identical. R502's complete normal Windows app/tool build and 46 selected checks
have started. Nothing is deployed yet; the Dell remains normal R481/.22 with
experiments off. Native-mode stress, latency and the full format matrices remain
required before adoption is accepted. Evidence: `artifacts/windows/native-flow-r500`,
source manifests and `consumers-r502`.
