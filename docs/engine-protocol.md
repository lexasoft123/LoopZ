# Engine control protocol 1.0

Development contract for the standalone native Engine. This describes current
source, not installed hardware qualification. See [Linux setup](linux.md) and
[the native source API](native-source-api.md). Product version, control major
version 1 and routing-document version 2 are separate versions.

## Transport and discovery

`loopz-engine [--socket PATH] [--state PATH]` serves AF_UNIX SOCK_STREAM. Default
socket: `$XDG_RUNTIME_DIR/loopz/control.sock`; XDG_RUNTIME_DIR is required without
an override. Default state: `$XDG_CONFIG_HOME/loopz/engine.json`, falling back to
`$HOME/.config/loopz/engine.json`. The socket parent must be a private directory
owned by the effective user; socket mode is 0600. Both peers check the effective
UID with SO_PEERCRED on Linux. Socket and state lock files prevent duplicate
owners of those paths. This does not coordinate a competing non-LoopZ ALSA owner.

Each connection carries exactly one UTF-8 JSON request plus newline and one JSON
response plus newline, then closes. No subscriptions or pipelining. Keep the
request frame including newline within 1 MiB (1048576 bytes); the provided client
requires request JSON bytes strictly below that bound. Responses have an
independent 16 MiB (16777216-byte) frame limit so a valid saved document plus
inventory and telemetry can still be reconciled. CLI and desktop parse responses
with that larger bound. Saved routing and request parsing retain the 1 MiB
limit; the JSON parser accepts an explicit byte bound with a hard maximum of
16 MiB. Parser nesting is limited to32.
Each complete frame receive and frame send has a two-second monotonic deadline,
in addition to socket read/write timeouts; trickling bytes does not renew it.
Connect, hardware open, enumeration and request execution are outside that frame
deadline. Audio callbacks run separately from the serial control owner.

```sh
loopzctl                              # canonical state
loopzctl --request '{"version":1,"operation":"hello"}'
loopzctl --stdin < request.json       # one compact JSON line
```

CLI success writes JSON only to stdout. Exit0 means a successful response,
exit2 a protocol response with `ok:false`, exit1 a CLI/transport failure printed
to stderr. A state response has no `ok` field and counts as CLI success; inspect
readiness/restoration rather than interpreting exit0 as working audio.

## Operations

Every request has `version:1` and string `operation`.

| Operation | Additional fields | Response |
| --- | --- | --- |
| `hello` | none | `{version:1,ok:true,protocol:"loopz.engine",major:1,minor:0,pcm:"native-only"}` |
| `state` | none | Canonical state below; no generic `ok` envelope |
| `retry`, `retry-session` | none | Reconciles latched storage from disk, then retries activation; success does not guarantee activation; read state |
| `initialize` | `request` below | Creates the first nonempty routing document; conflicts if one exists |
| `edit` | `request` below | Changes an existing document; conflicts if none exists |

Mutation shape (epoch is opaque, generation is an unsigned64 decimal string):

```json
{"version":1,"operation":"initialize","request":{"version":1,"expected":{"epoch":"COPY_FROM_STATE","generation":"0"},"commands":[{"type":"createDevice","id":"mix","name":"Mix","channels":2,"sampleRate":48000,"passThrough":false}]}}
```

Use `passThrough:false` on Linux: the generic editor default is true, but Linux
pass-through capture is unavailable. For undo/redo replace `commands` with
`"history":"undo"` or `"history":"redo"`. History is process-local and is not
restored after service restart. Exactly one of commands or history is required; ambiguous requests are rejected. Read state before every mutation, send its exact
revision and reconcile on conflict. Canonical generations prohibit leading
zeros except `"0"`; never coerce64-bit counters through JavaScript Number.

All command objects reject unknown fields. Required fields follow `type` below;
optional fields are in parentheses. Controls are any subset of
`{gain:number,enabled:boolean,muted:boolean}`; gain is finite linear amplitude
0..1. All channel indices are zero-based. A connection is
`{sourceChannel:uint32,outputChannel:uint32,gain:0..1}`.

| Type | Fields |
| --- | --- |
| createDevice | id, name (channels=2, sampleRate=48000, passThrough=true) |
| duplicateDevice | device, id, name |
| renameDevice | device, name |
| deleteDevice | device |
| resizeDevice | device, channels |
| removeOutputPair | device, firstChannel (even zero-based pair start) |
| setDeviceRate | device, sampleRate |
| setDeviceControls | device, controls (safetyLimiter:boolean) |
| addSource | device, id, kind, selector (channels=2, muteOriginal=true, connectByDefault=true) |
| removeSource | device, source |
| setSourceControls | device, source, controls (muteOriginal:boolean) |
| setSourceWiring | device, source, connections |
| setInputChannels | device, source, channels |
| setSourceSelector | device, source, selector |
| addMonitor | device, id, selector (channels=2, connectByDefault=true) |
| removeMonitor | device, monitor |
| setMonitorControls | device, monitor, controls |
| setMonitorWiring | device, monitor, connections |
| setMonitorEndpoint | device, monitor, selector, channels |
| reorderDevices | ids |
| reorderSources | device, ids |
| reorderMonitors | device, ids |

Batch maximum128 commands. Apply is transactional and validates each resulting
step; order removals before deleting referenced buses. Reorder lists are stable
IDs, not friendly labels or device enumeration positions.

## Document and channel maps

Canonical document is `{version:2,devices:[...]}` (decoder also accepts version1).
A `device` is a logical bus, not a Linux virtual soundcard:

- `id`, `name`, `sampleRate`, optional `formatRequestReceipt`;
- `routing:{outputChannels,gain,enabled,muted,safetyLimiter,sources:[...]}`;
- each source: `{id,channels,gain,enabled,muted,connections}`;
- `bindings:[{id,kind,selector,muteOriginal}]`, one per source with matching ID;
- `monitors:[{id,deviceSelector,channels,gain,enabled,muted,connections}]`.

Source connections map source channel→bus output channel. Monitor connections
map bus output channel→physical output channel. Empty wiring means no wires;
`connectByDefault` requests the reducer's initial mapping only. An input source
with channels N opens physical channels0..N−1; selective capture is represented
by routing wires, not a separate physical-index list in this protocol. Likewise
monitor channels N opens physical outputs0..N−1 and wiring selects the target.
Opaque selectors come from inventory UIDs; never parse card numbers from them.
For input-device only, the selector `default` resolves inventory.defaultInputUid
when acquiring or reopening capture. A missing default fails explicitly. This
does not promise automatic migration of an already-running capture when the
system default changes.

Kinds: `input-device` selects hardware; `virtual-device` selects another document
bus ID; `application` is stereo and on Linux supports a native:
source through the in-process API; an unregistered native: selector resolves
offline until its host binds a source and prepares/publishes a replacement scene; `pass-through` is stored by the portable model
but unavailable for enabled Linux routes. General Linux application capture,
public virtual-device creation and mute-original application control are absent.
Cycles, including disabled dependencies, and mismatched channel maps are invalid.

Current document maxima:32 buses,32 sources/bus,32 monitors/bus,64 routing channels,
4096 wires/list,128 commands/request. Bus outputs are even counts2..64; source
channels1..64. Strings are at most4096 bytes and contain no NUL; model-specific
identity/receipt constraints also apply. Runtime is48kHz with256-frame mix blocks,
a 1024-frame source history budget and negotiated monitor history budgets
(not measured latency), capture buses
currently at most64 channels and one enabled monitor owner per physical output
UID. Channel expansion on an already-owned input or output requires explicit
off/reconfiguration; a failed replacement retains the prior route. Limits are
implementation bounds, not a promise that their maximum combined load is qualified.

## Acceptance, activation and failure

A mutation prepares resources, atomically writes/fsyncs desired routing, then
commits prepared publication. A temporary resource/preparation failure may still accept and persist
the new desired document for retry while retaining active audio. An invalid or
unsupported runtime configuration returns `unsupported-configuration` before
persistence and leaves desired state unchanged. Success reply:
`{version:1,ok:true,code:"ok",detail:"",revision:{epoch,generation},activeGeneration}`.
Compare generations and restoration; `ok:true` means accepted intent, not audible
or fully restored routing. A no-op returns the generic success without revision.
After a lost reply, read state before resubmitting. Epoch changes after restart;
saved desired generation survives. Active generation does not prove all devices
remain connected. On initial startup with no active scene, endpoint-open failures
retain the full desired graph and publish healthy branches with failed resources
offline. The generation may be active while restoration remains retry; failed
physical branches retry independently. When replacing an existing active scene,
a new endpoint-open failure retains that prior scene instead. Missing native:
registrations are offline source state, not unsupported configuration; registering
one requires the native host to prepare/publish a scene before it becomes active. `poll()` checks/retries failed endpoints independently on an
approximately two-second control cadence; no hard real-time recovery bound is
promised. Temporary pending intent remains pending until preparation succeeds or is edited.

Errors use `{version:1,ok:false,code,detail}`. No `retryable` field is implemented:

| Code | Meaning and client action |
| --- | --- |
| unsupported-version | Envelope/request version unsupported; negotiate a compatible client |
| unsupported-operation | Unknown operation |
| invalid-request | Parse/shape/range/graph/history failure; fix input; transport framing failures may also close without a reply |
| conflict | Epoch/generation or initialization state differs; reread canonical state |
| unsupported-configuration | Runtime rejects a configuration it cannot support; fix the edit; desired state is unchanged |
| storage-error | Failure before state rename; edit was not accepted; correct the storage failure and retry with current revision |
| storage-unavailable | Saved state is unreadable/invalid or durability failure is latched; preserve and repair state, then retry-session |
| uncertain | Rename succeeded but directory durability failed; desired state/generation now reflect the published replacement; reconcile from disk with retry-session |
| generation-exhausted | uint64 desired generation exhausted; administrative migration required |

Activation errors appear as restoration text, not dedicated hardware error codes.
A post-rename durability error latches storage unavailable. `retry` and
`retry-session` reread and validate the saved state and fsync its parent before
clearing the latch, replacing desired state/generation and resetting undo history.
A corrupt/unreadable saved file causes `invalid-request` during that retry and
keeps the latch; failed parent fsync returns `storage-unavailable`. Reread state
after recovery before editing. State file is a bounded regular file, opened without
following symlinks; replacements use0600 temporary files and atomic rename.

## State and telemetry

Top-level `version:1`, `connection:{connected:true,ready:boolean}`,
`inventory:{applications:[],devices:[{uid,name,inputChannels,outputChannels,
inputChannelNames,outputChannelNames}]}` and `snapshot`:

- `version:1`, `revision:{epoch,generation}`, desired `document` or null,
  `canCreateDocument` (true only when no desired document exists and storage is
  available);
- `history:{canUndo,canRedo}`;
- `restoration:{phase,desiredGeneration,activeGeneration,error}`; phase is
  `no-document`, `retry` or `restored`;
- `runtime:{platform:"linux",sampleRates:[48000],metersFresh,devices,monitors,
  sources,inputs,outputs,renderedFrames,graphFailures,missedDeadlines,
  editCapabilities:{removeOutputPair:true}}`.

`runtime.devices` contains active scene buses `{id,active,meter,sources}`; each
source is `{id,online,missingFrames,meter}`. `runtime.monitors` contains
`{device,monitor,meter,sourceOnline,missingFrames,measuredOutputRate}` and, when
matched to a desired monitor, `complete`, `connected`, `error` from its output
owner. These rows represent active monitor programs; a desired monitor whose
replacement has not activated may not yet have a runtime row. The desired
monitor remains in `snapshot.document`.

`runtime.sources` reports desired hardware bindings as
`{kind:"input-device",selector,demanded,connected,complete,error}`. Demand means
both source and bus are enabled. Connected/complete describe a matching active
capture owner, not whether its source samples currently have usable timestamps.

A meter is `{valid,channels,frames,peak:[...],rms:[...]}` with linear amplitudes;
frames0 is unavailable. `metersFresh` is false while desired and active
generations differ, and monitor meter.valid is forced false in that situation.
Bus/source meter rows may still describe the retained active graph; clients must
honor metersFresh and must not label them as the pending desired graph's signal.
Freshness is generation alignment, not a hardware continuity proof. Restoration
also requires connected capture with online history and usable timestamp quality;
connected alone cannot prove restored input.

Endpoints serialize `{id,connected,error,xruns,discontinuities,deadlineMisses,
sampleRate,periodFrames,streamGeneration}`. Capture endpoints additionally expose
`timestampQuality`, `sourceSequenceAvailable`, `sourceCounterHardware`,
`sourceCounterOrigin`, `observedXruns`, `captureBoundaries`, `capture`,
`measuredRate`, `callbacks`. Generic `xruns`, `deadlineMisses` and `periodFrames`
are null because the capture interface cannot report those terminal/period facts.
`observedXruns` is a decimal string when metadata is available, otherwise null:
it counts ALSA overrun errors last observed with a delivered callback, not exact
lost frames or a live terminal-status query. `captureBoundaries` counts runtime
history reset boundaries, including generation/sequence/quality changes; it is
not an XRUN count or lost-duration measurement.

`capture` is null without valid provider metadata, otherwise:
`{clockDomain,streamGeneration,sequence,sourceFrame,sampleHostTimeNs,
callbackHostTimeNs,flags,discontinuity}`. Identifiers/counters/times are decimal
strings; sourceFrame and either host timestamp are null when their validity bit
is absent. Endpoint streamGeneration reflects this metadata when available.
Flags are numeric bits: 1 source frame valid, 2 sample host time valid,
4 callback host time valid, 8 timestamp quality valid, 16 stale anchor,
32 discontinuous. Discontinuity is the pinned enum integer: 0 None,
1 StreamGenerationChanged, 2 SequenceGap, 3 SampleRateChanged,
4 TimestampQualityChanged, 5 ClockReanchored, 6 DeviceLost, 7 SourceFrameOverflow.
Clients should tolerate unknown enum values and flags.

Timestamp quality is 0 Unknown, 1 Hardware, 2 CallbackEstimate. Unknown timing
is withheld from routing history rather than relabeled as valid samples.
The ALSA source frame counter counts PCM actually received within the physical
route, not hardware sample positions. sourceSequenceAvailable indicates usable
provider source-frame metadata; sourceCounterOrigin is received-pcm or unavailable
and sourceCounterHardware remains false. Sequence/generation boundaries can
identify discontinuity but cannot reconstruct how much hardware audio was lost.
Do not call received-PCM continuity a hardware-counter proof.

Counters/generations are decimal strings; meter frames/channels, sample rates,
periods and timestamp quality are numbers. Live observations are not an atomic
cross-device timeline. Null/missing metadata is unknown, never evidence of zero
errors or zero latency. Poll canonical state; do not synthesize meters. These
software counters and meters do not measure electrical or end-to-end latency.

State reads renew a 500 ms native meter-preview lease. It renders enabled mixes
for telemetry without adding physical output owners or editing routing. Unused
preview demand expires when readers stop polling; configured monitor routes
continue independently.

The `partial` restoration phase means the full desired graph is published while
some sources or monitors remain unavailable. Healthy branches remain available
in the editor. Shared source connection entries are deduplicated by kind and
selector with union demand; native sources publish application-kind status.
Output endpoints include callbacks, renderedFrames, renderFailures and
invalidCallbacks, plus inputChannels/outputChannels and reported output period
and buffer sizes. Monitor entries include callbacks, graphFailures and
invalidBlocks. Zero-valued provider latency components are represented as null
because this provider API cannot establish whether zero is known. These separate
reported components are not measured end-to-end latency.

Standalone runtime admission is bounded before endpoint/history/graph allocation:
8 unique demanded physical captures and 128 captured logical channels, 8 physical
outputs and 128 output channels, 128 summed bus output channels, 512 summed source
channels, and 8192 source-plus-monitor connections. Disabled source/monitor graph
storage counts; retained wider capture profiles also count. The portable document
schema still permits 32 buses and 64 channels per node. Over-budget edits are
rejected before device acquisition; saved unsupported documents remain readable
for repair with a valid batch. These are admission limits, not a claim of realtime
performance for every maximum configuration.

### Timing diagnostics

Capture `historyReads` reports enabled read diagnostics, decimal-string read and
requested-frame counts, missing-frame reasons (`invalidRequest`, `offline`,
`anchorUnavailable`, `clockMapping`, `resamplingSupport`, `generationChanged`),
and unpublished/expired/racing/absent source-sample lookup counts. Lookup counts
are filter taps and must not be added to missing output frames. It also reports
maximum verified clock-step nanoseconds and verified step count.

`historyClock.projected` identifies the estimated received-PCM history timeline;
`maximumRawPhaseStepNs` and `maximumProjectionErrorNs` describe its observations.
Raw `capture` provider timestamps and timestamp quality remain unchanged. This
policy adds no PCM queue and does not establish hardware clock accuracy.
Only callback-estimated timestamps with valid, continuous received-frame metadata
use projection. It follows a 32-block phase average with corrections bounded to
0.2% of each block duration. A backward timestamp, source gap or more than 2 ms
of raw-to-projected error resets the history rather than concealing a boundary.
Hardware-quality timestamps and streams without usable metadata remain raw.

Monitor `historyBudgetFrames` is a configured render-clock budget, with
`historyBudget` components `queuedOutput`, `outputPeriod`, `filterFuture`, and
`schedulerReserve`, all in render-clock frames. It accounts for the reported
playback queue and history publication/filter support. It is separate from the
source history budget and is not measured end-to-end latency. The scheduler
renders one 256-frame block ahead; this lead is separate from the tagged-clock
source lookback and monitor history budget. The total rounds
up to a 256-frame render block; its components remain unrounded. Unavailable
bootstrap profiles report `historyBudgetKnown:false` and null budgets until the
output opens with a negotiated profile.

`renderLateFrames`, `renderSkippedFrames` and `renderReanchors` are decimal
counters. A late scheduling wake can catch up without skipping the render grid;
skipped frames and reanchors remain separately visible. `missedDeadlines`
counts blocks published after their tagged interval end or skipped blocks.
