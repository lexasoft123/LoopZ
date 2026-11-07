# Linux application and standalone Engine design

Status: implementation specification, 7 November 2026. Acceptance gates below
are requirements, not claims of completed qualification. Targets: Linux x86-64
and ARM64; the physical board is Orange Pi 5 Plus, not Raspberry Pi.

## Scope and ownership

LoopZ owns the persistent routing service, validated document/control contract,
CLI, existing React/SingZ UIKit editor host, packages and migration. zCore owns
reusable native hosting, ALSA, capture leases, histories and DSP. The parent
SingZ Box session owns its client, song/training composition and TV adaptation
once LoopZ has a stable contract and artifacts. Application source stays out of
the singz-box infrastructure repository.

Service lifetime is independent of songs and editor/TV windows. Enabled input
monitoring retains device leases when a song stops or a client restarts.
Explicit input-off and device reconfiguration still control hardware lifetime;
a persistent service need not keep all hardware open while idle. One service
owns each physical ALSA direction. PCM never crosses JavaScript.

## Audited source and provenance

Start from committed LoopZ origin/main in the isolated linux-engine worktree.
The heavily dirty Windows migration checkout is reference only, not a released
dependency. macOS embeds React in AppKit/WebKit and already has a per-user
Engine lifecycle; Windows work uses WebView2 and separate platform adapters.

Audited Box source: SingZ `fb090a9c54db235470599506c0b90c634055eb43`, clean
zCore child `b3364438e641f928c48a8935706216989d2114cc`. The latter was verified
published at `refs/heads/codex/flow8-hdmi-mix` with `git ls-remote` on 7 November.
Advance LoopZ's explicit gitlink after integration checks, never a floating
branch. The prior LoopZ pin `b8905f6ca6da354997db3e6f9c5aa730bd3c64f1` has
older APIs; verify portable and macOS compatibility when updating.

The current reviewed integration pin is
`fce0eeb3dceffd032246cdbebcf5d6709a75dff4`, advancing the audited Box baseline
with bounded 64-channel selected capture and callback provenance. Publication
and parent integration checks must be verified separately; a local reviewed
commit does not by itself establish remote availability or hardware acceptance.

Reusable paths at the current pin, relative to the zCore repository:

- `zcore/platform/linux/alsa_pcm.*`, `alsa_capture.cpp`, `audio_host_linux.cpp`,
  `audio_input_linux.cpp`, `audio_input_wake_linux.cpp`: direct hardware
  inventory, bounded stop/wake, coherent timing, capture leases and recovery.
- `zcore/include/zcore/device/audio_host.h`: `AudioHost`, opaque inventory UIDs,
  zero-based physical maps, negotiated format, route/stream generations,
  terminal state, XRUN/deadline/discontinuity diagnostics.
- `zcore/include/zcore/device/audio_input_backend.h`: selected planar
  `openBus()` does not start callbacks; Unsupported permits mono fallback,
  Error forbids fallback; `takeFailure()` belongs on the control thread. The
  selected bus limit is now64. Optional capture provenance carries generation,
  sequence, valid timestamps/source counter and last-observed ALSA XRUN count;
  source frames count received PCM, never hardware sample positions/lost time.
- `zcore/include/zcore/routing/audio_history.h`: preallocated `AudioHistory`,
  serial `append()`, non-destructive `read()` and per-reader `AudioReadCursor`,
  missing reasons and `readWithCoverage()`. Missing frames reset generations;
  unknown timing must not be relabeled as valid nominal PCM.
- `zcore/include/zcore/routing/sample_clock.h`: bounded lock-free historical
  anchors retain the clock interval corresponding to each requested sample.
  Only authoritative continuous counters permit bounded correction.
- `zcore/include/zcore/routing/routing.h`: prepared validated `RoutingGraph`,
  maximum 64 channels/32 sources at this pin. Render performs no allocations,
  locks, logging, device queries or shared-resource destruction.

CMake targets: `zCore::device`, `zCore::routing`, `zCore::audio`,
`zCore::runtime`, with existing `SingZ::` aliases. Linux requires C++20,
Threads, ALSA development headers, pkg-config and libudev development headers.
Keep media/analysis disabled. Regression suites include
`audio_host_linux_tests`, `routing_tests` and `routing_clock_tests`. Named ALSA
plugins are a private test seam, not production UID parsing or silent fallback.

SingZ `native/playback/shared_audio_service.*` demonstrates persistent ownership
and drained song/training/mic renderer attachments; `microphone_mix.*` shows
selected-channel independent-clock relay and diagnostics. Reuse through zCore
or a reviewed pinned library; do not copy these into a diverging product fork.

Newer regular32 work is uncommitted: core base b336443, source snapshot hash
`62e577e60dd65bfc6a337254b7722bea4c05cd7d0c53e9daf70428c02122d911`, image
`localhost/singz-runtime:0.26.7-regular32-62e577e6`. Infrastructure
`docs/RECOVERY-2026-11-07.md` records archived patches/receipts in ignored
`artifacts/recovery-20261107/`. Its validated 32/64/128/256 override and newer
metering need upstream review before dependency consumption. Newer Box archives
also have renderer ownership fences/rollback, but are not a reviewed release;
the earlier native Box receipt predates final JS fixes.

## Linux UI host decision

Use native GTK/WebKitGTK with packaged local React/SingZ UIKit assets and an
asynchronous native bridge to the service. This follows existing WebKit hosting,
supports x86-64/ARM64 and Wayland, and does not add Node or another audio owner.
Validate the actual installed WebKitGTK host on the board. Reject external
navigation and subframe bridge calls, bound requests and validate again in the
service. Preserve macOS WebKit and Windows WebView2 behavior and platform build
switches. Retain channel selection/wiring, meters and English/Russian/Simplified
Chinese controls. Explicitly report unsupported Linux application/virtual capture
capabilities rather than inventing availability. Hidden/obscured UI throttles
polling/animation; closing it never stops enabled monitoring.

## Control and native source contract

Provide a documented versioned local control/telemetry endpoint and CLI,
independently buildable/installable without GTK or frontend dependencies.
Protocol compatibility is separate from product version. Require bounded framed
requests, same-user authentication and private runtime/state paths; no network
listener is needed. Structured failures distinguish unsupported version,
malformed/oversized input, stale edit, invalid map, unavailable hardware,
persistence failure and activation failure, with retryability/current epoch and
generation where relevant.

Expose device/channel inventory, source→bus→hardware maps, gain/mute, persisted
routing, desired/active generations and acknowledgements, meters, device/clock
status and reconnect/recovery. Preserve existing expected epoch/generation edit
semantics. Persist validated desired state atomically before acceptance; accepted
intent is separate from active audio. Prepare off realtime threads. Failed
replacement keeps the working route, including exclusive endpoint conflicts.
On initial startup, publish the full graph with unavailable physical/native
branches offline so healthy branches can run; restoration remains incomplete.
Retry failed physical branches independently while preserving desired intent.
An endpoint failure while replacing an existing active scene retains that scene.
An absent native: registration resolves offline until its host registers and
prepares a new scene; polling does not invent or register a song producer.
Reconnect reads canonical state; a lost reply is not proof an edit was rejected.

The first native song/training seam is reusable in-process
`bindNativeSource("native:<id>", AudioSource)`: a native producer binds a bounded,
realtime-safe source, and the persistent runtime retains physical ownership.
JavaScript controls settings only. Stopping/detaching the producer retires that
source while enabled microphones remain. This seam does not provide cross-process
PCM transport. A bounded shared-memory adapter is deferred and must be designed,
versioned and tested before a separate Box process can send song/training PCM.
Its future contract must specify format/capacity, sequence/timestamps, producer
generation, fencing/lifetime, timestamp quality and underrun/disconnect behavior;
never pass C++ objects across process boundaries or introduce a competing ALSA
owner. The current native control protocol carries no PCM.

## Phases and acceptance ownership

1. **LoopZ service/protocol:** standalone runnable Engine/CLI; negotiation,
   bounded parsing, stale edits, atomic persistence/restart, desired versus
   active state, same-user isolation, reconnect and failed-replacement tests.
   A control-only runnable slice must label audio inactive and cannot count as
   the complete Linux application milestone.
2. **LoopZ runtime/pinned zCore:** inventory, capture histories, channel maps,
   gain/mute, meters, prepared publication and independent-clock recovery.
   Silent fake-backend and ALSA loopback checks cover channel identity, invalid
   maps, bounded missing audio, device loss/reconnect and healthy-route survival.
   Song detach and editor death retain enabled monitoring. Verify no competing
   physical owner or PCM serialization over the control channel.
3. **LoopZ host/packages:** x86-64/ARM64 Engine/CLI and real editor packages,
   separate Engine-only install, service unit, dependency metadata, source/hash
   receipts, clean installation/update/rollback and saved-route restoration.
   Run frontend model/store tests, real WebKitGTK automation/screenshots,
   localization and visibility checks. Run portable/macOS regression builds and
   available Windows compatibility checks after dependency/build changes.
4. **Coordinated hardware window:** record installed hash/source snapshot,
   architecture/configuration, identities, formats/rates/periods, route and active
   clients. Begin with loopback, then agreed FLOW→HDMI independent-clock tests.
   Measure warmed unprofiled continuity counters and end-to-end latency with
   reference/exclusions. Cover startup, device loss/reconnect, service/editor
   restart and persisted recovery. Report build/fake/browser, installed,
   hardware and third-party-client evidence separately.

Every commit needs independent review of the exact staged tree and recorded
`git write-tree`; index changes invalidate review. Preserve binaries and source
receipts, keep scratch data in ignored artifacts. Package previews remain
previews until promised acceptance passes; release tags require authorization.

## Current hardware facts and unverified limits

Infrastructure `docs/AUDIO-QUALIFICATION-2026-11-07.md` supersedes older default
routing prose: selected direction is SingZ USB→FLOW DSP→analog MAIN→separate
amplifier, HDMI for video. Physical amplifier connection/audibility, analog
latency and pitch/reconnect integration are unverified. FLOW Recording exposes
10 capture/4 playback channels at 48 kHz S32_LE; 9/10 is pre-master return. Do
not enable a software mic return to FLOW without an explicit feedback-safe
route. HDMI relay remains an optional diagnostic path.

Regular32 evidence is a settled 20-second clean silent window at 32/128;
startup missing-frame counters increased. HDMI acoustic estimates of 260 ms at
256/1024 versus 240 ms at 32/128 are 5 ms-grid envelope estimates on a specific
path, not precise latency or new LoopZ qualification. Buffer duration, configured
history budget and end-to-end latency are distinct. UIDs may change across boots;
mode changes invalidate cached formats.

Preserve active Box, VNC/display, kernel and mixer settings. Read-only inspection
does not authorize competing streams or service replacement. Coordinate
exclusive hardware access before physical audio testing. No firmware flashing,
kernel replacement, mixer experiment or reboot is part of this port.
