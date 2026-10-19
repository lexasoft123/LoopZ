# LoopZ feasibility architecture

The first milestone proves one stereo virtual duplex device, one application
source, one microphone, one silent hardware monitor and two independent
recording clients. Completing these gates is required before expanding to
multiple devices or building the complete interface.

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
stream before committing. Fresh driver activation and driver-binary replacement
still need an independently verified design. No automatic global-reset fallback belongs
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

Mach absolute time is the common reference; the virtual device has its own
sample clock at 44.1 or 48 kHz. Each capture endpoint retains its source sample
counter and hardware host timestamp. A bounded source ring and copied affine
anchor map a requested host time into source samples. Measured source rate
accounts for long-term drift. The original measured graph aligned sources 4096 virtual frames
behind its publication timestamp. Driver input adds a fixed 1024-frame queue
margin. Expected software delay is therefore about 107 ms at 48 kHz, before
application and hardware buffering; this is a measurement target, not a result.

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

Application capture selects a bundle identity and currently captures a stereo
mixdown. On macOS 26, Core Audio can restore processes by bundle ID. The older
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

The revised virtual graph runs on a dedicated time-constrained thread with an
independent Audio Work Interval, rather than sharing HAL health queries and
telemetry with the render deadline. Its default source lookback is 2048 frames;
`--latency-frames` permits 256-frame increments through 4096 for controlled
rehearsals. A private tap aggregate requests 128 frames. After warmup, the worker
records the minimum margin between the required block end (including
interpolation lookahead) and the latest capture sample represented by its clock
snapshot. The original driver queue and its advertised 5120-frame latency are
unchanged; reduced signal latency does not make that metadata accurate.

A render reader owns its last coherent source-clock snapshot. It makes only two
publication attempts and can reuse that snapshot during a normal capture
append; the history remains indexed in the source sample clock. A generation
change invalidates the cached snapshot before discontinuity samples are
published, and is checked again after reading the block. There are no render
locks, logging calls, HAL queries or heap allocations.
