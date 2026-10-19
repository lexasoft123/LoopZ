# LoopZ feasibility validation gates

Status is recorded separately for automated tests, installed tests and hardware
tests. A compiled executable or synthetic signal is not proof of app capture,
microphone authorization, clock stability or installed operation.

| Gate | Pass condition |
| --- | --- |
| Independent dependency build | Build and verify pinned source hashes without the SingZ or singz-ui sibling directories |
| DSP | Actual zdsp graph mixes three stereo buses, contains nonfinite input and limits peaks at both rates |
| Buffer transport | Two readers receive the same frames; overwritten frames become zero; concurrent and cross-process operations preserve channel pairing |
| Driver publication | Installed stable UID is visible to Core Audio; both streams open at the negotiated format |
| Installation and updates | Fresh install, helper update and driver update work without a Core Audio restart or reboot; default device selections and an unrelated hardware audio stream remain intact |
| Installed bridge | Sandboxed coreaudiod and the per-user helper exchange protocol-checked memory through launchd |
| Two consumers | Two independent probe processes each observe 440 Hz left and 880 Hz right at >0.07 amplitude and cross-channel amplitude <0.01 |
| Failure silence | With the helper stopped or killed, both channels have RMS <1e-6 after the 250 ms heartbeat bound |
| Real sources | Selected application and authorized microphone each produce independently identifiable captured signals; no synthetic substitution |
| Pass-through | Audio written to the virtual output appears once at virtual input; it is not recursively recaptured |
| Background | Helper launched in its installed identity works without a UI; restart and consent denial/revocation fail clearly |
| Independent monitor | Hardware callback reads the virtual timeline using hardware timestamps; automated runs write zeros to the physical output |
| Sustained run | At least two physical hours with independent source/monitor clocks; no cumulative delay growth, missed driver frames after warmup, or unbounded memory growth |
| Latency | Measure end-to-end latency at both rates; initial target <150 ms excluding client processing; record buffer settings and hardware |
| Compatibility | Oldest promised OS and current OS; each promised CPU architecture; recording and calling clients |

The feasibility device is one stereo device, one app capture, one microphone,
one monitor and at least two recording clients. More devices, native
multichannel app capture, arbitrary channel mapping and hot structural edits
are outside this milestone. Supported maximum concurrency for the complete
release must be chosen from measured results.

Core tests simulate rate mismatch; they do not constitute a physical drift or
two-hour test. Installed tests require a driver installation and may require
Core Audio to reload drivers. Permission prompts require the user's response.

## Measurement boundaries

The installed timestamped fixture combines 1100/2200 Hz stereo carriers with
channel-specific, host-time-coded markers. The output callback generates the
marker from its Core Audio host timestamp. Each recording client correlates
every complete one-second window against the input callback host timestamps
with a 1 ms delay search grid. A pass requires both carriers, channel identity,
marker correlation above 0.75, delay below 150 ms, and a delay range no greater
than 2 ms. Hardware sample-counter discontinuities and an overrun of the fixed
analysis queue fail the run. This measures the digital route's timestamp
delay. It excludes acoustic microphone latency and external client buffering.

The sustained controller separately requires zero post-warmup increments in
app/microphone history misses, driver/monitor misses, source discontinuities,
monitor timestamp errors and scheduler late frames. It requires a successful
helper exit, nonzero app/mic source samples and callbacks, and no fatal output.
Observed resident-memory range after the first minute must stay within 16 MiB
for each process. A short rehearsal cannot establish a long-term memory bound.
Microphone/monitor clock diagnostics record measured rates and anchor ages;
the app marker alone does not validate the independent microphone clock.

The controller requests a control-thread telemetry checkpoint after both recording
clients finish and before stopping the helper. Error deltas run from warmup to
that checkpoint, covering the complete recording interval and client exit churn.
Later increments are reported separately as `postCheckpointCounterIncrements`;
they are not automatically attributed to shutdown. The final helper exit and
stopped event must still succeed. Optional app-only comparisons disable both the
microphone and monitor and do not certify those paths. The churn rehearsal opens
and closes an additional one-second recording client every ten seconds while
the two continuous clients keep recording.

The optional 64/128-frame render experiment is restricted to app-only
device-stream capture. Its controller also requires positive minimum source
availability margin and publication slack, zero missed publication deadlines,
and an exact match between requested and started block sizes. Source lookback
must be at least one render block and a multiple of that block. Driver build 3
accepts source metadata in 64-frame steps while keeping the independent driver
read queue on its existing 256-frame grid. The driver metadata and helper's
actual lookback are configured together; neither is substituted for measured
marker delay. Smaller-block tests do not certify microphone or monitor latency.

Core tests compare the actual zdsp graph rendered in 64/128-frame calls with
the same samples rendered in 256-frame calls, at both sample rates. They cover
limiting, nonfinite-input containment, output buffer boundaries, and a full
4096-frame capture append followed by a smaller history read. Synthetic probe
tests detect a 4 ms marker delay using the same 1 ms search resolution as the
installed measurements. These tests cannot replace installed timing evidence.

Failure-to-silence observations contain callback receipt timestamps and block
peaks. They support the 250 ms silence bound, with callback-block uncertainty;
they do not identify the exact time of the last nonzero sample.

## Installer checks

The updater serializes privileged transactions with a root-owned lock, validates
the complete sealed driver bundle, snapshots the package into a private
directory and prepares sibling replacements with rollback copies. Active LoopZ
IO or its capture engine prevents an update. It restarts only its own broker.
A signed helper control query verifies a real driver attachment without mapping
audio memory or starting capture.

The independent hardware guard writes zeros, checks timestamp/sample continuity
and total frame coverage, and must remain alive through final update checks.
Only the installer's explicit stop can pass it; expiry is a failed check.
Service PID/start time, all three default device UIDs, the resident driver
directory inode and its complete bundle hashes must remain unchanged.

Failure-injection tests cover promotion failure after a backup rename,
restoration failure with backup retention, failed fresh installation, missing
targets with symlinked parents, concurrent installer locks, manifest traversal,
extra files, incompatible protocol, resource-level driver differences, changed
defaults/service identity, and broker-stop failure during rollback. These tests
use temporary destinations and do not mutate the installed system.
