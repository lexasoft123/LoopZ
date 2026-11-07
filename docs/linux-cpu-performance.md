# Linux CPU audit and Pi measurements

Development-preview evidence from 2026-11-07, on an Orange Pi 5 Plus (RK3588,
ARM64). Percentages below are Engine process CPU, where 100% means one occupied
core. They exclude Box, graphics and shared kernel/service work.

## Enabled paths and settings

The final Release build uses Clang 21.1.8, `-O3 -DNDEBUG`, C++20 and generic
ARMv8-A SIMD. Disassembly of the actual shared resampler object contains NEON
`fmla` instructions. There are no sanitizer, debug or unoptimized overrides in
that target. Unsafe fast-math is not enabled: finite-value handling remains part
of the audio contract.

LoopZ now forwards to the pinned `zCore::routing::AudioResampler` instead of
maintaining a local reader that bypassed its optimizations. Cached FIR handles
eligible near-unity reads; the prepared convolution plan handles eligible unity
reads of 128–256 frames. Other ratios, layouts and request sizes retain the
shared fallback. These paths preserve filter support and existing queue/history
budgets. Callback tests cover spectrum, drift, partitioning, absent support,
interleaved output and allocation freedom. A scalar reference checks 128, 256
and 257 frames, including large sample anchors and missing input.

The optional private cached-FIR native-kernel hook has no ARM-specific provider
in this pinned dependency; its portable implementation uses compiler SIMD.
Enabling a build flag cannot supply a missing provider. The reusable source SDK
links the same pinned routing/DSP targets and rejects substituted or omitted
canonical sources.

The standalone Engine now selects the highest kernel-reported CPU capacity
within its inherited allowed mask before creating workers. On this Pi that
selects A76 cores 4–7 (capacity 1024) rather than allowing migration to A55 cores
0–3 (capacity 397). `LOOPZ_AUDIO_CPU_POLICY=inherit` opts out. Unknown capacity,
homogeneous CPUs or affinity errors preserve the inherited policy. The SDK does
not change its embedding application's affinity.

The host's `ondemand` governors, priorities, kernel and device settings were
preserved. Capture/output requests for FIFO priority 20 were denied with EPERM
by the existing unprivileged limits, and inherited scheduling remained active.
Thus this is not a claim that every possible performance setting is enabled.

## Compiler comparison

Final generic and ThinLTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`,
`-flto=thin`) builds were compared with the frozen pre-change implementation on
CPU 5, after warm-up, with three alternating repetitions and no concurrent task
builds. The baseline driver used its complete old core archive and old header;
old and new private object layouts were never mixed. Each comparison covers
2/8/64 channels, four rate pairs and 256-frame reads. Raw JSON rows are retained
under `artifacts/cpu-optimization/final-uncontended` on the Pi.

For fractional 48→48 kHz stereo, medians were 723.326 µs before, 46.958 µs
shared, and 46.083 µs shared with ThinLTO. The twelve fractional-clock rows ranged
from 0.065× to 1.018× the old processing time. ThinLTO ranged from 0.887× to
1.010× the generic shared time: useful for some unequal-rate cases, about 1.9%
for this stereo unity case, and no universal improvement. The default remains
the portable generic Release build; an embedding parent may opt into IPO with
its matching compiler/toolchain. CPU-specific `-mcpu`/`-mtune` flags are not
forced into a library targeting heterogeneous machines.

Microbenchmarks are synthetic processing measurements, separate from hardware
continuity. Extreme wide-channel/high-ratio cases still exceed their output
period; these results do not qualify the maximum admitted graph.

At 200 ppm measured drift, the shared implementation took 0.552×–1.026× the
old processing time across the twelve rows; ThinLTO took 0.882×–1.012× the
shared generic time. Both builds passed the resampling correctness tests.

## Physical transport

Two final-build runs each passed all 71 assertions over 60 seconds after
warm-up, without an attached profiler. Engine CPU was **18.204%** and **18.777%**
of one core. Both had zero warm deltas for missing source/monitor frames,
capture/output xruns, discontinuities, graph/render failures, deadlines, skipped
frames and reanchors. Failed replacement retained the prior route; undo and
restart restored the saved graph. Startup/warm-up counters are retained in the
reports and excluded from these warm deltas.

The graph was FLOW 8 ten-channel capture, selected channels 9/10 to a stereo bus,
and connected HDMI output, with monitor gain zero from creation. Rates were
48 kHz, callbacks 256 frames, reported output queue 1024 frames, source history
1024 frames and monitor history 1792 frames; renderer lead stayed 256 frames.
The container retained its two-CPU quota. The default Engine policy selected
cores 4–7 from the unrestricted inherited mask. No additional buffering was
introduced. These budgets are not measured end-to-end latency.

The tested Engine SHA256 is
`31396e42859118d6934de60ce370354ce74b3171ab00f6a85b1c9099ebda3745`.
Frozen executables, harness, raw snapshots, per-thread affinity/ticks,
cgroup/frequency/thermal context and restoration records are preserved under
`artifacts/cpu-optimization/pi-shared-final`. Run IDs are
`linux-hardware-lkc0hlne` and `linux-hardware-r_qia8ru`.

A separate matched baseline/final/baseline/final sequence used the same explicit
4–7 affinity mask for both executables, the same devices, graph, rates, buffers,
quota and 60-second duration. All four runs passed 71 assertions with zero warm
continuity errors. Engine CPU was:

| Run | Before | Shared resampler |
| --- | ---: | ---: |
| 1 | 38.593% | 20.011% |
| 2 | 38.558% | 17.860% |

The two-run means are 38.575% and 18.935%, about **50.9% less Engine CPU** under
this controlled configuration. Governors stayed dynamic and unrelated clients
were left running; these short repeated observations are not a worst-case
headroom or endurance guarantee. Reports and frozen binaries are under
`artifacts/cpu-optimization/pi-shared-matched`; run IDs are
`linux-hardware-q17aqu66`, `linux-hardware-8p2p8szo`,
`linux-hardware-3rprsve8` and `linux-hardware-ocg9on48`. The baseline Engine SHA256
is `30b4ae9dc5827097a904ca6a57e76bf8cd2f9bac5c8243e5a58c0604feee17a0`;
the final binary has the SHA256 recorded above. Box and VNC were restored again.

The earlier 93.7% result allowed heterogeneous-core migration. It cannot be
used as a matched estimate of the resampler's CPU saving. Earlier experimental
runs with unrestricted affinity also exhibited continuity failures; those
reports remain preserved and are not counted as successful qualification.
Maximum graph load, long-duration operation, audible/analog continuity and
end-to-end latency remain unqualified. Box's original image and display/VNC
supervision were restored after the exclusive window, with unchanged display
service configuration.
