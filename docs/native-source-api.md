# Native song and training source integration

The first integration seam is an **in-process C++ source**, not cross-process
PCM transport. Header: `native/include/loopz/runtime/audio_runtime.hpp`;
`AudioSource` is defined in `native/include/loopz/audio_scene.hpp`. This is a
source-level API tied to the pinned LoopZ revision, not a stable binary ABI.
Rebuild all C++ consumers when updating this revision: the resampler now wraps
`zCore::routing` directly, its private layout changed, and `ResampleFrame` aliases
the shared provider result. Do not mix old object files with updated headers.
The source guard verifies both DSP and routing target sources, including sources
added by an embedding parent after configuration. The SDK does not select CPU
affinity; the standalone Linux Engine owns its startup policy. The shared
resampler uses bounded per-call stack scratch (under 160 KiB for its convolution
path), with no callback heap allocation or additional PCM queue. Embedding hosts
must provide sufficient audio-thread stack space.

The standalone service protocol explicitly reports `pcm:"native-only"`.

```cpp
loopz::runtime::AudioRuntime runtime;
std::shared_ptr<const loopz::AudioSource> source = makePreparedSongSource();
runtime.bindNativeSource("native:song", source);
// Add an application binding with selector native:song to the RouteDocument.
// Validate/prepare, durably store desired intent, then commit on control.
auto prepared = runtime.prepare(document);
storeDesiredAtomically(document); // host owns durable acknowledgement semantics
runtime.commit(std::move(prepared));
```

`makePreparedSongSource`, `document` and `storeDesiredAtomically` above are host
application responsibilities, not shipped API functions. The standalone Control
owner already implements persistence and acknowledgements, but does not expose
a socket operation to register a C++ source. A Box socket client alone cannot
attach song PCM to the current service.

## Ownership and rendering

One serialized control owner calls registration, preparation, commit, polling
and destruction. Use one persistent runtime/device owner shared across song and
microphone composition; do not instantiate a second ALSA host for Box playback.
`bindNativeSource(selector,shared_ptr<const AudioSource>)` requires a native:
prefix, a maximum 4096-byte selector, a non-null source and 1–64 channels. The portable
application binding currently requires stereo, so bind a source with at least
two channels and declare the application route stereo. Register before prepare
for immediate resolution. A missing `native:` registration
resolves offline rather than rejecting the full document. Later registration
requires the host to prepare/publish a scene; control polling cannot attach a
producer by itself.
Replacing a registration does not mutate an already-prepared graph; prepare and
commit a new document scene to publish it. `unbindNativeSource(selector)` removes
future resolution and returns whether a registration existed; published scenes retain their shared ownership until
replaced. Registration is bounded to64 distinct selectors; replacing an existing
selector does not consume another slot. Retained ownership can outlive playback.

`prepare()` returns owned prepared storage or throws, opening new required
endpoints while retaining working ones. Resolve or destroy that preparation
before the next prepare. Persist desired state before `commit()`, which is a
noexcept publication; foreign/already-committed/null preparations are ignored.
On first startup without an active scene, unavailable endpoints remain offline
in the published full graph while healthy branches run. Once a scene is active,
a failed replacement keeps it. Call `poll()` from control to collect retired
resources and retry failed physical branches independently.
Destruction drains callbacks. Never destroy the runtime merely because the TV
window closes or a song stops.

`AudioSource` implements:

- `channels() const noexcept`: fixed logical layout;
- `read(firstHostNs,rate,RoutingOutput,frames,AudioReadCursor&) const noexcept`:
  fill every requested planar float output, zero missing samples, return
  `{missingFrames,online}`. Timestamps are the runtime monotonic host domain;
  data must be mapped to that timeline rather than relabeling unknown samples;
- optional `readMeter(snapshot)`: true means supported; frames0 means unavailable;
- optional `readChannels(...)`: unique increasing channel selection; return
  nullopt without touching output when unsupported;
- optional `readWithFilter(...)`: defaults to read; never prepare coefficients
  or alter shared producer state on the render thread.

All read/meter methods run under realtime constraints: no allocation, locks,
logging, OS/file/network calls or resource destruction. Each serial render owner
gets an independent `AudioReadCursor`. Do not consume a shared destructive queue
when multiple buses/monitors need the same samples. Allocate buffers, decoding,
training/stem composition and bounded history on control/producer threads.
`AudioHistory` is an existing fixed-capacity serial-producer/non-destructive-reader
adapter; append accurate host timestamps and authoritative sequence metadata
when available. The current pinned selected capture buses support 64 channels
and deliver optional callback provenance. ALSA sourceFrame counts received PCM,
not hardware sample positions; `sourceCounterHardware` is false. `observedXruns` is
the last delivered callback's observed overrun count, and `captureBoundaries` counts
history resets. None measures exact lost frames/duration. A missing source
counter or invalid timestamp stays unknown, not a fabricated zero.
Prepared song/training algorithms remain Box/SingZ product responsibilities.

Stopping or changing a song should make that source publish silence/offline or
replace only its prepared source scene while enabled microphone routes remain.
Removing a route from the next document does not remove the retained registration;
call unbindNativeSource afterward on control when it is no longer needed.
Explicit input-off and monitor/device reconfiguration still govern physical
lifetime. Reuse unchanged device owners; failed replacements must retain the
working route. Runtime restrictions and protocol status are documented in
[the protocol](engine-protocol.md).

## Boundary with Box and zCore

A future Box integration must either place its native source producer inside
the persistent Engine process through a reviewed reusable library boundary, or
first implement a bounded versioned shared-memory adapter. The latter is absent:
no SHM ABI, producer daemon, attach command or external playback package is
claimed ready. Design and test format/rate/channel bounds, capacity, sequence
and generation publication, timestamp quality, reader fencing, lifetime,
underrun/disconnect silence and restart compatibility before using that route.
Control messages may carry settings/status/meters, never audio samples.

The physical audio provider is the explicit zCore gitlink, not copied ALSA code.
LoopZ exports this seam through the pinned source SDK below. Box must pin that
source contract and implement its producer integration before using it. Preserve one device owner and avoid a fork of
SingZ SharedAudioService or LoopZ DSP. Current native tests establish bounded
software behavior; real Pi/installed audio qualification and external Box
integration remain separate work.

## Pinned source SDK consumption

The preview now provides a concrete **source-level** embedding boundary:
`LoopZ::Runtime`. Pin the LoopZ repository revision in the host project and
initialize or provide the exact zCore revision recorded by LoopZ's gitlink.
This is not an installed binary ABI or Box client integration. It permits the
persistent native host to compose its song/training producers and microphone
routes inside one runtime without duplicating zCore or opening competing ALSA
owners.

A parent CMake project that already builds zCore uses:

```cmake
# Both paths refer to explicit, pinned Git checkouts owned by this project.
set(ZCORE_BUILD_MEDIA ON CACHE BOOL "" FORCE) # host policy, if needed
add_subdirectory(third_party/zcore)
set(LOOPZ_SHARED_ZCORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zcore")
set(LOOPZ_BUILD_MACOS_BACKEND OFF CACHE BOOL "" FORCE)
set(LOOPZ_BUILD_ENGINE OFF CACHE BOOL "" FORCE)
set(LOOPZ_BUILD_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
set(LOOPZ_BUILD_LINUX_DESKTOP OFF CACHE BOOL "" FORCE)
set(LOOPZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/loopz)
target_link_libraries(my_persistent_audio_host PRIVATE LoopZ::Runtime)
```

If zCore targets already exist, `LOOPZ_SHARED_ZCORE_ROOT` is mandatory. LoopZ
requires the device and DSP targets' actual source directory to match that
path, verifies its clean Git HEAD against LoopZ's **index gitlink**, and reuses
those targets. It neither adds zCore again nor changes the parent's media,
analysis or testing settings. Configuration and build-time verification reject
dirty or mismatched dependency checkouts. The DSP target's actual source list
must include the canonical graph runner and built-in nodes and may contain only
tracked sources within that checkout. This checks constructed targets even when
an embedding parent resets overlay cache variables afterward. A generated,
configuration-specific final source manifest is rechecked at build time, so
later target source changes cannot bypass the pin. This source guarantee does
not freeze legitimate parent compiler features or build options. An uninitialized
LoopZ-local zCore directory is allowed only when a valid external shared source was supplied.
Without preexisting zCore targets, omit the shared-root setting: LoopZ builds
its own verified gitlink normally.

`LOOPZ_BUILD_RUNTIME_LIBRARY=ON` creates the runtime library independently of
`LOOPZ_BUILD_ENGINE`. The latter additionally enables the POSIX daemon, socket
control owner, CLI and service installation. Library-only builds create none of
those executables or service install rules. `LOOPZ_BUILD_TESTS` disables only
LoopZ's tests; it leaves the parent's `BUILD_TESTING` policy intact. Existing
parent `format` and `format-check` targets are preserved. The public target
propagates C++20 and includes the existing native source API above. The host
remains responsible for one serialized control owner and durable desired-state
acknowledgement before runtime commit.

Run the reusable silent consumer acceptance from a LoopZ checkout:

```sh
python3 tests/sdk_consumer_test.py
python3 tests/sdk_consumer_verifier_tests.py
```

The consumer creates a real parent project, adds zCore once, links only
`LoopZ::Runtime`, registers a native song source, prepares/publishes a scene
with hardware-opening factories forbidden, and verifies that parent tests stay
enabled. It also rejects a target/source-directory mismatch. Disposable Git
fixtures cover external source revision and dirty-tree rejection. Build outputs
stay in `artifacts/sdk-consumer`; supply `--build PATH` and `--zcore PATH` for an
isolated build or an already-owned external dependency.

`RuntimeLimits` in the public runtime header defines aggregate admission budgets.
They are checked before allocation/opening; failed demanded endpoints and wider
retained capture profiles count. Keep replacements within those budgets. Tests
exercise all aggregate limits together and rapid edits without unbounded retained
physical owners. See the protocol for the current values and hardware evidence.
