# Experimental Windows virtual cable

The v0.2.1 development-signed preview is distributed only with the explicit
[experimental setup and removal guide](../../../../packaging/windows/experimental/WINDOWS-SETUP.md).
See [current acceptance](../../../../docs/windows-preview-release.md); older
format and validation notes below describe the driver’s development history.

This is development source, separate from the desktop preview package. It is
not installed or loaded by the desktop build. Do not distribute it as a working,
signed LoopZ driver until the loaded-driver acceptance below passes.

The first endpoint pair is **LoopZ Feed** (playback) and **LoopZ Microphone**
(recording), both stereo PCM16 at 48 kHz. Each adapter has separate ingress and processed queues and its own endpoint pair. The setup helper provisions
one deterministic root instance per owning account and stable mix ID. The runtime
automatically publishes each mix to that instance and keeps hardware monitors as
independent consumers. The desktop ZIP alone still has no installed driver.
Exact-float shared WASAPI opening passed the earlier single-adapter Windows 10
checks; multi-adapter loaded validation is a separate acceptance requirement.

## Provenance

Adapted from Microsoft's MS-PL-licensed `audio/simpleaudiosample`, commit
`bb68ed5e770bce04dc5395ec9258dafab034184d` of
https://github.com/microsoft/Windows-driver-samples . Copyright notices and
LICENSE are retained. `upstream-sha256.json` records the original source hashes.
All source is formatted to the repository style. Behavioral changes are in common.h/common.cpp, minwavertstream.h/.cpp, capture format
and topology tables, resource identity, INF and build project. File recording and
tone generation are removed. Internal sample class names remain to keep the
adaptation reviewable.

Only event-notification WaveRT buffers are supported; polling allocation is
rejected rather than exposing unvalidated render memory.

The adapter owns nonpaged, spinlock-protected ingress packet storage and a
200 ms processed recording FIFO with 20 ms prefill.
The WaveRT clock advances at exact 48 kHz with rational sub-frame carry, rather
than rounding positions to whole milliseconds. Portable randomized clock tests
check that callback partitioning cannot accumulate clock drift.
Copy operations hold its lock for at most 480 frames. Capture clocks advance
while underflow produces silence. Pausing/stopping the producer or consumer
clears the corresponding queue. Ingress remains independent of capture, so
hardware monitors can consume playback with no recording client. Processed
audio does not accumulate while capture is closed. Render
publication checks committed WaveRT packet numbers, and long DMA catch-up is
bounded to the current DMA window. The FIFO tests exercise the same allocation-
free implementation in userspace; they do not validate kernel stream lifecycle.

## Build

Use Visual Studio 2022 Build Tools with **Windows Driver Kit Build Tools** and
**MSVC x64/x86 Spectre-mitigated libraries**, plus the NuGet CLI. Run
`scripts\build_windows_driver.cmd` from the repository root. The project pins
Microsoft.Windows.WDK.x64 10.0.26100.6584 and its SDK dependencies 10.0.26100.1.
KMDF is pinned to 1.31. The INF targets x64 Windows 10 build 19041 and newer;
Windows 10 22H2 is the required acceptance baseline.

The script produces an unsigned development build only. It does not enable test
signing, import certificates, modify Secure Boot, create a device, or install a
driver. A trusted signing/distribution path or an explicitly approved dedicated
test environment is required before loaded validation.

## Recorded development checks

A development-signed build loaded successfully on Windows 10 22H2. Separate
left/right markers reached two shared WASAPI recording clients; producer stop
produced zero samples and restart restored audio without closing recording.
The desktop app-capture/mix/Feed path also passed with no missing source/monitor
frames during its short checked interval. A direct feedback edit was rejected
before persistence. These checks do not replace the remaining acceptance below.

Windows 10 currently displays localized names such as Speakers (LoopZ Virtual
Audio) and Microphone (LoopZ Virtual Audio). Feed/Microphone describe their roles.
`loopz-windows-cable-probe.exe --endpoints` reports typed roles without opening
audio. Identification follows the topology connector to the adapter's exact
ROOT\LoopZAudio hardware ID and does not guess from the friendly label. Per-mix instance identity is now retained by discovery; friendly names and stable
endpoint identity across driver updates still need loaded acceptance.

## Loaded-driver acceptance

- Verify INF/catalog, imported APIs, identities and 48 kHz stereo formats.
- Feed distinct left/right markers into playback and record them through WASAPI;
  verify signal, channel separation, missing frames and measured latency.
- Keep capture open through feed pause, starvation, process crash and restart;
  require silence without stale replay and stable recording timestamps.
- Test both startup orders, capture close/reopen, buffer overflow and long timer
  gaps, followed by multiple shared recording clients.
- Test the desktop monitor provider's exact-float shared-mode format against
  Feed. Prevent selecting Microphone as an input to its own feed.
- Exercise unplug/device removal, sleep/resume, long recordings, low-memory
  failure, driver verifier on a dedicated test system, installation and removal.

Compilation and FIFO tests alone do not establish these properties.

## Private processed playback transport

`duplex_bridge.hpp` implements bounded transport storage for driver 0.3.0.1. Public playback packets and processed recording samples have separate
queues; playback cannot reach recording without an engine write. Incoming packets
retain their presentation time and sequence, and overflow reports a discontinuity.
The engine owns an exclusive generation with a 100 ms monotonic lease. Expiry
clears both queues, produces silence, and rejects late writes from that generation.
Playback and recording stream lifecycles remain independent, so an app-only mix
does not need a public playback client and a hardware monitor does not need an
open virtual microphone.

`loopz-windows-duplex-bridge` checks those invariants, including adapter isolation,
capture reopen, overflow, invalid packet lengths and clock regression. The
WaveRT render path writes only ingress packets; capture reads only processed
audio. Topology filter properties expose the fixed, versioned protocol in
`transport_protocol.hpp`. Requests validate verb, instance and value sizes,
version, reserved fields, frame limits and generation. Connect checks the
provisioned owner SID against the caller security context; subsequent calls
require the same referenced process object. No raw caller pointers enter queues.
Property data is staged outside the spinlock in nonpaged pool.

The Windows 10 workbench passed `--private-roundtrip`: distinct channel markers
arrived at half amplitude after private processing, playback stop produced
silence, disconnect produced silence while playback continued, and reconnect
restored the processed markers. The temporary root device was removed afterward.
`virtual_cable.cpp` connects the desktop mixer to this protocol. Full saved-mix
acceptance is tracked separately in `tests/windows_pass_through_acceptance.py`;
the older 0.2 archives and their direct-cable test results do not prove it.
Saved-mix acceptance passed again on driver 0.3.0.1 with separate channel
markers, 0.5 mix gain, producer stop and restart. Its measured peak was about
0.025, with the stricter 0.026 peak limit. Setup now skips healthy adapters
already bound to the staged package and installs other adapters individually.
Adding and removing the test mix preserved the existing microphone ID. The
earlier global-hardware-ID upgrade from 0.3.0.0 to 0.3.0.1 changed the ordinary
Windows endpoint ID; external-app selection preservation remains open. An
authorization check rejected a SYSTEM-account connect with access denied, while
a second owner-account process was rejected as busy rather than preempting the
active engine.
