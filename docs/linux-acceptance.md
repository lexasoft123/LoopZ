# Linux preview acceptance

This is development-preview evidence, not a stable release qualification.
The implementation targets Debian 13 x86-64 and ARM64 and keeps the existing
React/SingZ interface inside a native GTK3/WebKitGTK 4.1 host. The Engine and
native source SDK run independently of that editor.

## Automated and desktop checks

On 2026-11-07, native x86-64 and Orange Pi 5 Plus ARM64 builds each passed
50 CTest cases, including the actual WebKitGTK bridge and visible routing
workspace under isolated Xvfb. Screenshots, frozen executables, interface assets
and SHA256 receipts are preserved in the task's ignored `artifacts/linux-final`.
These checks did not use the host Wayland session and do not prove compositor
occlusion behavior. Host-session desktop acceptance remains unverified.

The real parent-project SDK consumer built and ran on both Linux architectures.
It reused one pinned zCore target, published a native song source, retained parent
build/testing policy, and rejected mismatched, dirty and late DSP source overlays.
The frontend suite passed 81 tests, its locked Prettier check and production build.
The CPU follow-up passed 51 ARM64 native cases, the parent SDK checks,
48 standalone macOS cases and all 132 full macOS cases. Native IPC/XPC checks
ran outside the filesystem sandbox after sandbox-only bind/identity failures.
The CPU implementation also passed native x64 Linux CI; the parent source SDK
checks now run on both Linux CI architectures.

Changed native/Python files pass clang-format 22.1.8 and Black 25.1.0 checks.
The repository-wide native formatting check still reports unrelated baseline
files; those files were preserved rather than reformatted in this feature.

## Packaging evidence and limits

The packaging script produces separate Engine/CLI/user-service and desktop
Debian/tar payloads with source snapshots and installed-file SHA256 receipts.
Both x86-64 and ARM64 payloads passed metadata, split-content, checksum, dynamic dependency
resolution and disposable dpkg-root install checks. Exact packaged executables
passed silent initialization, stale-generation rejection, persisted state and
stop/restart epoch checks. The fixture used the build container's runtime
libraries; clean apt dependency installation on a fresh distribution remains
unverified. No package script starts or enables a service. Debian dependencies are derived
per split from the actual target ELF symbols with `dpkg-shlibdeps`; the desktop
also requires the exact matching Engine package. Receipt dependencies and Debian
control metadata matched on both targets.

## Physical transport evidence

The first hardware attempt was invalidated when Box's display supervisor
recreated its runtime. A second test paused the supervising display service,
opened FLOW 8 capture channels 9/10 through a ten-channel native bus, and routed
to the connected HDMI output with monitor gain zero from the initial commit.
It measured 60 seconds after warm-up without an attached profiler.

That run kept generations and routes stable and reported zero capture/output
xruns, capture history discontinuities and output render failures. It failed
continuity: source missing-frame delta 36,332, monitor missing-frame delta 102,529,
and one Engine deadline miss. These results are preserved as a failed run and
do not establish hardware readiness. The failed report remains preserved.

After bounded estimated-clock projection, contiguous render catch-up and
queue-derived monitor budgeting, the same route passed all 71 assertions in a
repeat run: `linux-hardware-spf2dc3d`. Over the 60-second warm interval, capture
advanced 2,881,280 received frames and output rendered 2,880,768 frames. Capture
and output xruns, source and monitor missing frames, graph/render failures,
Engine deadlines, skipped frames and reanchors all had zero deltas. Failed
output replacement retained the advancing prior route; undo and Engine restart
restored the saved full route. Different capture/output frame counts reflect
independent observation boundaries and clocks, not a measured lost-frame count.

The tested profile was 48 kHz, 256-frame callbacks and a reported 1024-frame
output queue. The monitor history budget was explicitly increased from 1024 to
1792 frames (768 frames / 16 ms of configured history allowance), including the
reported queue, callback period, filter support and scheduling reserve. Source
history budget remained 1024 frames; the renderer leads by one 256-frame block.
These components do not constitute measured end-to-end latency. Raw provider
timestamps remain in the evidence; the projected timeline remains estimated.
Before the CPU follow-up, the Engine consumed 93.7% of one CPU core over that
interval (process tick accounting, two-CPU container quota, no attached profiler). This is informational
for this ten-channel capture / selected stereo route, not a CPU improvement claim
or qualification of the maximum admitted graph. The
[CPU follow-up](linux-cpu-performance.md) replaces the local resampler with
the pinned shared implementation and adds a conservative Engine startup CPU
policy. Two final ARM64 runs passed all 71 checks at 18.2% and 18.8% of one core,
with unchanged graph and buffering budgets. The old unrestricted-affinity result
is not a matched estimate of the resampler improvement. Production load/headroom
and long-duration continuity still need further measurement.

Box was restored using its original image, its display service configuration
was byte-identical, both display and VNC supervision were active, and the VNC
listener returned its RFB greeting. No mixer/default-device, kernel, firmware or
signing settings changed. Tests are silent transport checks; audibility,
analog continuity and end-to-end latency remain unverified. External Box song
producer integration and a shared-memory transport are separate work.
