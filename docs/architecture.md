# LoopZ feasibility architecture

The first milestone proves one stereo virtual duplex device, one application
source, one microphone, one silent hardware monitor and two independent
recording clients. Completing these gates is required before expanding to
multiple devices or building the complete interface.

## Module boundaries

The first portability refactor separates the reusable C++20 core from the
macOS product backend. The baseline before this refactor is commit `92ce955`.

| Layer | Location | Responsibility |
| --- | --- | --- |
| Portable core | `native/core`, `native/include/loopz` | zdsp graph, source history, timestamp interpolation, atomic timeline, marker analysis |
| macOS backend | `native/platform/macos` | Core Audio capture/driver, Mach clock and scheduling, XPC transport, consent, inventory and process lifecycle |
| Dependency composition | `cmake/singz` | Portable zdsp runtime; optional macOS zcore device host in `macos.cmake` |
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

This is the initial module split. The helper still owns the feasibility
lifecycle and command-line orchestration; the complete route model and
Windows/Linux audio backends remain future work.

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

The feasibility interpolator is linear. Release-quality asynchronous
bandlimited conversion, measured spectral quality and high-rate or
multichannel support are subsequent gates. A rate or route discontinuity must
produce silence and explicit restart in this version; it must not silently
join unrelated source generations. The helper reports a virtual rate change
and exits so its graph can be prepared again. Runtime device/rate/channel
transactions belong to the expansion phase.

## Sources and safety

Application capture selects a bundle identity. A single eligible stereo
float32 output stream uses device-stream capture; other eligible routes retain
stereo mixdown. Unknown/failed output queries never become an empty route. On macOS 26, Core Audio can restore processes by bundle ID. The older
OS fallback needs explicit process watching/rebinding and remains a gate for
claiming full macOS 14.2 support. Browser/helper-process coverage must be
verified; a single process is not assumed to represent an entire application.
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
