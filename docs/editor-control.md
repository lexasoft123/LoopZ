# Editor commands and configuration ownership

## Desktop visibility and polling

The macOS desktop publishes its actual window visibility to the interface.
Hidden, minimized and fully occluded windows suspend state polling, release-update
status polling, meter expiry/publishing and CSS animation. An inactive or partly
covered window remains live. This is a view-only scheduling signal: it never edits
the document, pauses audio or stops the Engine/control connection.

Showing the window starts a fresh state read immediately. A response requested
before hiding is discarded; if that read is still in flight, the fresh read starts
as soon as it drains. The scheduler keeps at most one store poll in flight and
does not let a stopped page restart its timers. Navigation resynchronizes the
native visibility signal after each load.

`npm run test:webkit:visibility --prefix interface` exercises real AppKit hide,
minimize, full/partial occlusion and reload against a synthetic, nonpersistent
WebKit fixture. It requires an interactive macOS session and opens only its own
test windows; no production Engine, audio endpoint or saved mix is accessed.

## Editor transport

The portable editor commands, authenticated local transport and EngineRuntime
dispatch are implemented. The installed full-routing Engine exposes them through an
explicit user-domain Mach-service mode. The repository's `interface/` and native
desktop application use this transport and actual engine state. Canonical
desktop/Engine file deployment and capture-free activation are verified; actual
login behavior and the newer workspace changes remain acceptance work.

The installed desktop supports `--routing-status` for diagnostics. It validates
its interface seal, connects through the same production server identity/UID
checks as the editor, and prints one bounded snapshot without opening a window
or sending an edit, initialization or retry command. The recovery harness uses
this to compare the entire canonical document and desired/active generations;
retained public devices alone cannot acknowledge Engine restoration. This
command uses the same installed Engine and does not alter saved routing.

Each runtime device also exposes decimal-string `graphFailures` and
`sinkFailures` counters. They are the existing scene counters, read on control;
snapshotting them adds no render-side instrumentation. Their lifetime follows
that prepared scene. Discovery uses a metadata-only macOS adapter and excludes
LoopZ virtual devices before expensive metadata reads. Picker fields and fresh
hardware-buffer validation retain the same contract; channel labels are not part
of this picker projection.

The repository/private diagnostic also supports an input-only capture handshake:
`--virtual-input-check --capture-protocol stdio-v1 --read-device <virtual-uid>
--buffer-frames 64 --seconds 28 --signal marker`. After `prepared`, write
`begin-capture` followed by a newline. After every participating reader emits
`captured`, write `release-input` to each. The final report includes strict-window
signal checks and bounded timestamped zero spans/callback gaps. No audio samples
are written. This opt-in diagnostic was tested from a signed private Engine; the
installed `ca8b00…ccf6ba` pair is unchanged and does not include the new flags.

## Ownership and commits

`native/include/loopz/route_edits.hpp` defines portable commands for creating,
duplicating, renaming, deleting and resizing devices; changing device controls
and rate; adding, removing and selecting sources and monitors; changing their
controls and channel wiring; and reordering rows. The core reducer has no
platform headers, filesystem access, audio IO or permission requests.

A batch contains 1–128 commands and produces one validated candidate. Every
command must leave a valid graph. If any command fails, the original document
is untouched. To delete a referenced device, remove its dependent sources
first in the same batch. Disabled sources also count as dependencies. Feedback
cycles, duplicate wires, invalid channels and gains remain errors.

The macOS `EditorController` uses the existing `ConfigurationController` as the
single authority. It saves a changed candidate atomically before replacing
desired intent. It does not submit directly to the driver, create a second
configuration owner, or wait for a native acknowledgement. Normal controller
polling performs native reconciliation and retries after disconnection. The
snapshot reports desired and active generations separately.

Call these methods only on the engine's existing serialized control owner.
`EngineRuntime` now creates its editor controller with an OS-generated random
UUID epoch. Its editor entry point observes pending external document changes
before comparing revisions, saves through the same configuration owner, and
rejects edits during installer-only validation. The transport queues work for
the control owner; it never runs these methods from an XPC callback or renderer.

## Revision rules

Each snapshot supplies `{epoch, generation}`. Each edit must echo both values.
An edit based on an older generation or another engine process is rejected
before mutation. This also protects against another editor, external document
reloads and native client sample-rate requests, which advance the same desired
generation. After a conflict, refresh and let the user resolve the changed
intent; do not automatically replay a stale batch.

Commands cannot replace an entire document, choose a storage path, or set a
backend format-request receipt. Edits retain existing receipts so old native
requests cannot overwrite later user choices. Duplicating a device clears the
copy's receipt because it belongs to a new stable identity. A missing or corrupt
document must be explicitly created or repaired before editing; opening an
editor never implicitly publishes an empty graph. A snapshot advertises
`canCreateDocument` only when both the configured primary and recovery file are
absent. The explicit `initialize` operation validates a nonempty candidate,
checks the revision and atomically creates the primary without overwriting an
existing file. Concurrent creators cannot replace one another. Corrupt or newer
documents and existing recovery files require repair rather than initialization.

The optional native `loopz-live-editor-client` diagnostic now accepts `initialize`
with the same bounded standard-input JSON request as `edit`. It calls the existing
authenticated API and does not bypass revision, file-creation or trust checks.
Installed cold-start checks verify refusal for corrupt/newer saved files, explicit
first creation when both files are absent, rejection of a second initialization,
and restoration of that first device after Engine restart with its public UID.
The signed diagnostic stays in the workspace; installed app bytes are unchanged.
Evidence: `artifacts/cold-start-delivery-proof.json`.

Equal edits are no-ops: no file replacement and no generation increment.
An accepted changed edit means saved desired intent. It does not mean the
driver has applied that intent. Display the restoration phase and compare the
desired and active generations before showing that routing is current.

## Wire contract

### Virtual-device sample rates

The workspace routing backend accepts 44.1, 48, 88.2, 96, 176.4 and 192 kHz.
`native/include/loopz/sample_rates.hpp` is the portable shared policy used by
transport mapping, configuration validation, broker decoding, endpoint rendering
and HAL properties. Both nominal-rate ranges and physical/virtual stream-format
lists enumerate all six rates, including correctly bounded partial reads. Native
client requests still pass through the configuration owner's durable desired
document and format-request receipt before HAL changes the active format.

The Engine snapshot advertises this list as `runtime.sampleRates`. The interface
uses it for existing devices and an explicit sample-rate choice during creation.
Missing or malformed metadata falls back to the older 44.1/48 kHz list. A new
device initially follows the single, fresh default hardware output's supported
rate, otherwise 48 kHz (or the first advertised rate if 48 kHz is unavailable).
An open draft retains a rate that becomes unavailable visibly and blocks its
submission; it never silently replaces that choice. Saved unsupported formats
also remain visible in existing-device controls.

The current workspace negotiates this list with the authenticated broker and
driver. A broker advertises `rateCapabilitiesVersion: 1` in its hello reply. Only
then does a new peer include `sampleRateMask` in its attachment. Mask bits follow
the portable rate array in order and must include the two baseline rates with
no unknown bits. The broker intersects both attached peers' masks with its own
policy. Pairing epochs bind the result; unpaired connections fall back to
44.1/48 kHz. Older servers receive the unchanged three-field attachment, and
older peers still receive exactly four fields in pair notices.

The Engine advertises the negotiated list and rejects newly saved unsupported
formats, new devices at those formats and re-enabling a disabled unsupported
device. Previously saved rates remain intact across downgrade or disconnection;
unrelated controls can still be saved, and restoration waits for compatible
components. Direct publication is checked again in the helper, broker and
driver. The driver narrows its available-format lists without changing an active
device's nominal rate, IO client references or clock. That active format remains
visible even if temporarily outside the negotiated choices. A pending native
rate request outside the current mask is retained but withheld until capable
peers return.

High-rate deployment requires a matching routing driver, root broker and Engine.
The frozen pending driver update preserves the older broker and does not deliver
this feature. The legacy feasibility driver also remains limited to two rates.

`artifacts/high-rate-proof.json` records private transport, driver clock/IO,
format-request persistence, conversion and native WebKit acceptance. These tests
do not measure real hardware or application-capture latency at higher rates.
Full installed high-rate audio and a coordinated upgrade remain acceptance work.
`artifacts/rate-negotiation-proof.json` adds current/pre-negotiation wire
compatibility, malformed attachment refusal, backend snapshot, durable downgrade
and recovery checks. Frozen prior connection/broker sources are compiled under
separate class names through real private authenticated XPC; these are wire
compatibility fixtures, not old installed HAL binaries.

### Removing an individual output pair

The workspace editor supports `removeOutputPair` with `device` and zero-based,
even `firstChannel`. The portable reducer rejects unknown devices, missing/odd
pairs and removing the final stereo pair. It removes only wires attached to
the deleted channels and renumbers surviving channels in order. Output wires
from external sources shift only their destination; physical input channels
remain unchanged. Own pass-through wires shift both axes. Monitor wires shift
their virtual source axis, and direct virtual-device consumers shift their
source axis, including disabled consumers. Surviving fractional wire gains,
selectors, controls and stable IDs remain intact. The complete edit is one
validated, durable, undoable document change; existing native format approval
and recovery still govern activation.

The Engine advertises `runtime.editCapabilities.removeOutputPair: true`. Older
helpers expose no individual-pair action. Each actual SingZ output-pair card
offers removal when more than one pair remains. The separate confirmation
component previews removed/renumbered wires and dependent devices, caches its
review against document layout, and invalidates it when relevant wiring changes.
The action builder checks the same review basis before transport. Capability
loss disables an open confirmation. Control-only changes do not unnecessarily
invalidate that review. Socket selection clears when layout changes; deleting
the invoking pair returns focus to the surviving channel configuration control.
Rejected saves retain the dialog and existing graph and are not replayed after
reload.

`artifacts/output-pair-removal-proof.json` records every pair position in 4–64
channels, actual zdsp samples, dependent virtual-route audio, strict decoding,
native 8→6→8→6 transitions with active private clients, durable whole-graph
undo/redo, native Tab/Enter, rejection/downgrade and both ordinary/instrumented
WebKit runs. The native factory checks keep the unchanged dependent mapping
and clock while approving the selected device's format transition. They use
private hosts and do not prove installed Core Audio continuity or physical
keyboard/VoiceOver operation. This feature is outside the frozen signed update.

### Saved source and monitor selections

The workspace interface now exposes application, physical-input and virtual-source
replacement in source options, and output-device replacement in monitor options.
Physical input and monitor layouts can be set from 1 through 64 channels. App
capture remains stereo; virtual sources follow their selected device's layout.
Unavailable saved selections remain visible. Virtual choices reject direct and
indirect feedback, including disabled graph dependencies.

A physical source change sends `setSourceSelector` and `setInputChannels` in one
atomic edit. Monitor changes use `setMonitorEndpoint`; app/virtual changes use
`setSourceSelector`. Stable row IDs, controls and surviving fractional wire gains
are retained by the native reducer. A smaller layout previews the number of
removed connections and requires an explicit confirmation. The preview is tied
to the saved selector/layout/wiring and the proposed replacement; changed wiring
invalidates an open confirmation. The action builder also rejects a stale layout
before transport. Choosing to keep the current selection resets the draft without
sending an edit. Disconnected or uncertain transport disables these controls.

`artifacts/endpoint-editor-proof.json` records unit tests and an isolated browser
run of the production bundle, using the real native command decoder and reducer
via `loopz-editor-reducer-fixture`. The bridge, inventory, revisions and runtime
telemetry are test fixtures: these checks do not prove actual authenticated XPC,
saved-file durability, native WebKit or live audio routing. Both actual SingZ
palettes, two window sizes, modal keyboard focus and monitor connection direction
labels are checked. This interface change is outside the frozen pending installer.

After configuring the existing macOS build, the offline browser check is:

```sh
cmake --build build/editor-release --target loopz-editor-reducer-fixture
npm --prefix interface test
npm --prefix interface run build -- ../artifacts/endpoint-editor-interface
npm --prefix interface run test:browser:endpoints -- ../artifacts/endpoint-editor-interface ../build/editor-release/loopz-editor-reducer-fixture ../artifacts/endpoint-editor-browser
```

The browser harness launches an isolated headless Chrome profile and never
connects to installed LoopZ. `LOOPZ_CHROME` can select its executable. The native
test adapter processes bounded stdin JSON and does not open files or audio IO.

### Gain commit semantics

The shared device/source/monitor gain control now listens to the native committed
`change` event as well as pointer release, adjustment-key release and blur.
React's range `onChange` handles drafts from `input` events; it can suppress the
subsequent committed event after tracking the same value. Previously a semantic
input/change sequence displayed -6 dB while the actual saved gain stayed at
unity. The real private bridge/runtime rehearsal retained that failing case.

Held adjustment-key events remain one pending gesture until key release or
focus loss. Pointer drafts also remain uncommitted until their commit/release.
Repeated committed/release/blur events do not add routing history. Comparison
uses the slider's bounded integer-dB position, preserving fractional saved gains
when focus/blur occurs without an edit. Rejected saves restore the latest saved
display/accessibility value; uncertain outcomes still require explicit review.

The native Engine profile checks semantic and change-only/unfocused commits,
trusted AppKit arrow keys, repeated-key and pointer grouping, fractional focus/
blur, undo/redo and reconstruction. Its test-only handler reads the fixed private
primary routing file after each gain save. The state-only `gain` profile checks
rejected device/source/monitor edits, inherited disabled state, correct command
paths and no replay after reload. Normal and address/undefined runs are bound in
`artifacts/gain-commit-proof.json`. WebKit's
[slider implementation](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/accessibility/AccessibilitySlider.cpp)
dispatches input/change when changing an accessibility slider's value; these
event-contract tests do not certify actual VoiceOver actions. Public direct inspection of the
owned offscreen WebKit view exposed only its root during this rehearsal. No
Accessibility privacy prompt, VoiceOver activation, system input injection or
installed audio test is performed. The pending signed update remains unchanged.

### Native WebKit interface checks

On macOS, `npm --prefix interface run test:webkit` builds a fresh production
interface bundle and runs it in a real `WKWebView` through the production
`LZResources` handler at `loopz://app/index.html`. The test requires access to
the window server; a restricted command sandbox cannot run WebKit's processes.
Its hidden window uses a separate, nonpersistent website data store. It does
not construct the production bridge, start the Engine, request capture access
or open audio devices.

`artifacts/webkit-interface/result.json` records rendering, bundled SingZ styles,
named settings dialogs, focus wrapping and restoration, new-device dismissal,
and appearance/device selection surviving a document reload. The accompanying
`interface.png` captures WebKit's rendering. Keyboard checks dispatch DOM events;
they do not establish native key delivery or VoiceOver behavior. The state-only
native fixture also does not prove authenticated production transport, edit
durability or preferences surviving an application restart. Those remain
separate acceptance checks.

The additional `test:webkit:engine` runner uses the actual `LZBridge`, authenticated
anonymous XPC service, document runtime and atomic document storage. Its internal
constructor injects a private session host; production desktop/login entry points
retain their fixed endpoints and production host, with no runtime override.
The test creates, renames, mutes, duplicates and deletes devices through visible
UI controls, closes the editor while rendering continues, reconstructs the runtime
from its private saved file, and reopens the editor. It verifies stable device
identity, unchanged canonical intent, a new runtime epoch and restored controls.

```sh
cmake -S . -B build/editor-release
cmake --build build/editor-release --target loopz-webkit-engine-tests loopz-interface
npm --prefix interface run test:webkit:engine
```

Each run retains its private routing document, process output and report under
`artifacts/webkit-engine/run-*/`; the latest passing report is also copied to
`artifacts/webkit-engine/result.json`. The fake driver publishes no HAL devices.
This verifies the real bridge/service/storage path, not installed audio IO or
an Engine process crash. Native keyboard delivery, VoiceOver, durable appearance
preferences and deployed recovery still need separate acceptance.

### System appearance

The editor now defaults to **Match system**, choosing the actual SingZ Atelier
or Night Studio palette from WebKit's `prefers-color-scheme` media query. Native
appearance changes update both the palette and native form-control color scheme.
Existing explicit palette preferences remain explicit. Settings uses the real
SingZ segmented control for all three choices; the titlebar's quick toggle
chooses the opposite resolved palette and leaves automatic mode. Only the
choice is persisted, so reload does not freeze the currently resolved palette.
Appearance subscriptions and edits do not send routing or permission commands.

Run the isolated acceptance profile on macOS:

```sh
node interface/scripts/test-webkit.mjs artifacts/webkit-system-appearance system-appearance
```

Its test-only handler changes the private AppKit window between Aqua and Dark
Aqua and verifies actual WebKit media-query updates, explicit overrides, native controls, Settings
layout, unchanged canonical routing and document reload. It never changes the
system appearance preference or constructs the production Engine. Normal and
address/undefined runs, native keyboard regression, authenticated private
bridge/storage regression and source hashes are bound in
`artifacts/system-appearance-proof.json`. Complete-process preference persistence
has the additional private-bundle evidence below; physical keyboard/VoiceOver,
reboot/logout and deployed UI behavior remain gates.
This workspace increment is outside the frozen pending signed package.

### Preferences across app restarts and updates

`npm --prefix interface run test:webkit:persistence` builds two temporary,
ad-hoc-signed hardened apps with different freshly generated bundle identities.
They use the desktop's default persistent WebKit store, production resource
origin/handler and production interface bundle. A document-start probe reads
stored appearance/selection before the interface can write preferences. A
synthetic bridge serves state and refuses all edits or system actions; no Engine,
HAL device, capture, privacy request or production routing document is opened.

Seven separate application processes check explicit Night Studio/Atelier choices,
automatic mode under opposite native window appearances, stable selection and
isolation between identities. A version-2 private bundle is re-signed under the
same identity; its changed signed executable retains the earlier preferences.
Each process must exit successfully before the next starts. A final read verifies
that using the second identity has not replaced the original identity's settings.
Public WebKit removal clears both private data stores' records after the run;
empty per-identity cache metadata directories may remain.

`artifacts/preference-persistence-proof.json` binds actual process results,
signature/image changes, source hashes, fixture namespace isolation and cleanup.
This verifies default-store behavior in private app identities. It does not
certify the installed canonical desktop, Developer ID update delivery, logout,
reboot or power-loss durability. Existing nonpersistent WebKit profiles continue
to cover different UI/keyboard/recovery behavior, without claiming this gate.

### Source and permission recovery guidance

The interface derives recovery conditions in `interface/src/connection-state.js`.
Actions use explicit condition kinds instead of parsing native error text.
Current denied/restricted/unrequested microphone metadata takes precedence over
an older successful capture status. Idle inputs show readiness without claiming
active capture. Partial inputs offer source review while retaining available
channels and saved wiring; a disabled upstream virtual device offers navigation
to that device. Stale/disconnected telemetry cannot advertise healthy endpoints
or offer an ineffective retry. Settings distinguishes microphone denial,
restriction, initial setup and unknown status.

`node interface/scripts/test-webkit.mjs artifacts/webkit-recovery recovery`
exercises the visible guidance and actions in native WebKit using synthetic
snapshots. Its separate test-only handler records all non-state commands and
refuses edits and system actions. These checks do not alter or prove real TCC
grants, denial/revocation delivery or the first-time permission-request flow.
They preserve canonical saved intent and confirm that reload does not replay
recovery. The screenshot shows simulated denied-access guidance; actual macOS
permission acceptance and installed-driver recovery remain required.

### Explicit microphone setup

When Engine metadata reports first-time microphone access, Settings offers
**Enable microphone access**. Only that click sends `request-microphone` through
the origin-checked desktop bridge. A separate permission queue validates the
fixed installed Engine's existing Developer ID requirement/capability check and
runs its foreground `--request-microphone` command. No routing/capture mode is
started by this request. Normal state polling and graph rendering continue.

The bridge rejects duplicates and caller-supplied fields, exposes pending state
across Settings reopens, and cancels/reaps its owned request child on desktop
shutdown. The Engine's consent loop is limited to 60 seconds; its launcher has
a 65-second deadline and bounded output. Existing session commands retain their
five-second deadline. Only an owned, unreaped child is signaled on timeout or
cancellation. Denied/restricted states retain the explicit Privacy & Security
action; reconnects and reloads never send a consent request.
Foreground launch diagnostics on stderr are excluded from the structured JSON
response; existing session commands retain their combined-output behavior.

`loopz-microphone-setup-tests` checks grant/denial/pending response validation,
malformed or inconsistent replies, bounded child output, timeout, cancellation
and reaping using self-spawned test children. The real-bridge WebKit runner uses
an injected permission provider to check pending UI, concurrent state traffic,
duplicates, parameter rejection, panel reopen, shutdown cancellation and no
request replay after runtime reconstruction. It does not open a real consent
dialog or establish actual TCC grant/revocation behavior. The workspace changes
remain outside the immutable pending installation package.

### Saved list ordering

Device, source and monitor options now expose keyboard-accessible Move up/down
controls and the current position. Moves send a complete identity permutation
using `reorderDevices`, `reorderSources` or `reorderMonitors`, keeping controls,
wiring and source/binding alignment. The action builder rejects stale list
orders. Boundary controls and disconnected edits are disabled. Device selection
is pinned by ID before submitting a move, so an earlier state refresh cannot
change the editor's selection while its reply is delayed; the selection persists
across interface reload. Creating a device also persists its selected ID.
Async moves restore keyboard focus to the move button, or the available opposite
direction at a boundary, while respecting a user focus change during the save.

`artifacts/order-editor-proof.json` records native-reducer browser checks and
runtime fixtures. Eight saved device/source reorder cycles retain their capture
owners, monitor stream, transport regions and sampled mix in the optimized and
address-instrumented strict profile. A deterministic two-monitor fixture checks
immediate warm-bus reads after reordering at 48/44.1 kHz, including thread
instrumentation. These use injected endpoints rather than actual audio devices.

The first strict thread-instrumented runtime run missed the 1024-frame queue;
diagnostics showed 2457 frames of producer lag and render execution beyond its
deadline. An 8192-frame diagnostic also encountered clock reanchors. Those failed
timing reports are retained. The thread-instrumented runtime profile therefore
checks ownership, mapping/persistence retention and eventual audio after each
commit at the unchanged 1024-frame queue; optimized and address-instrumented
runs retain the strict per-poll sample check. This separates race/functional
acceptance from real-time performance. Installed-client continuity and live
latency remain required. The production queue settings are unchanged.

### Command encoding

`editor_protocol.hpp` provides protocol version 1 with a 1 MiB request bound,
strict required/optional fields, typed booleans and numbers, bounded UTF-8 text,
and the same source-kind names as the document schema. All generation counters
are canonical unsigned decimal **strings** so JavaScript can represent the full
64-bit range without rounding.

For example, after reading the authoritative snapshot:

```json
{
  "version": 1,
  "expected": {"epoch": "<epoch from snapshot>", "generation": "17"},
  "commands": [
    {"type": "renameDevice", "device": "<stable device id>", "name": "Stream Mix"},
    {
      "type": "setSourceControls",
      "device": "<stable device id>",
      "source": "<stable source id>",
      "controls": {"gain": 0.5, "muted": false}
    }
  ]
}
```

Gains use linear amplitudes from 0 through 1. Channel indices are zero-based.
Wiring commands replace the selected source or monitor's complete connection
list; each connection contains `sourceChannel`, `outputChannel` and `gain`.
IDs are supplied by the platform's identity generator, never derived from a
display name. Source selectors remain opaque backend identities. Physical
availability is separate from saved intent, so an unplugged input stays saved.

Output resizing preserves valid wires, removes deleted sockets, updates
dependent virtual-source channel counts and adds new pass-through identity
wires. It does not reconnect intentionally disconnected app or monitor wires.

## Authenticated local transport and reconnection

`EditorService` accepts only a pinned client code requirement and exact user ID.
The production requirement permits the Developer ID signed
`com.lexasoft.loopz.editor` application from team `USJ7H3X44X`. The client pins
the corresponding `com.lexasoft.loopz.engine` identity and its own user ID. Both
code requirements are enforced by XPC. A non-mutating handshake verifies the
server before any graph operation is sent. Production has no unsigned or
same-user-only fallback; isolated fixtures use their own exact executable hash.

The service bounds connections to eight, outstanding requests to sixteen and
each peer's outstanding work to four. An in-flight operation keeps its slot.
Unauthenticated connections and queued work expire after two seconds. Closing
a peer removes its unstarted work. The control owner polls at most one request
per iteration. Stopping the service cancels its peers and drains transport
callbacks without stopping the audio runtime.

`EditorServiceClient` bounds each request to three seconds. A lost reply to an
edit or recovery request has an uncertain outcome: the operation may already
have saved intent. The client disconnects and never resubmits that operation.

`EditorConnection` is the desktop's serial background connection owner. Its
`poll()` reconnects and refreshes snapshots with backoff from 100 ms to five
seconds. This handles launchd's observed ten-second replacement delay while
keeping each handshake bounded. Cached desired state remains available for
display, but editing is disabled until a fresh authenticated snapshot succeeds.
After an accepted, failed or uncertain edit, a fresh snapshot is required again.
Only reads and handshakes retry automatically; graph changes are never replayed.
These methods belong on a background transport thread because a connection or
read can wait for its bounded reply.

The runtime snapshot includes actual source/monitor connection states and
atomic device/source/monitor peak and RMS observations. Meter rows use stable active
device/source/monitor IDs and are tagged with the active generation, independently of
the desired document. Meters become invalid when rendering stops, errors or its
last completed block is older than 250 ms. They are never synthesized from saved
gain settings. Monitor observations additionally require a complete connection
and a physical output callback within 250 ms. Microphone authorization is read
without requesting access. Internal latency frames are timing metadata, not an acoustic
latency measurement.

The development Engine accepts `--editor-service true` only with routing-document
mode. Its user-domain launch job must declare
`com.lexasoft.loopz.editor.control` in `MachServices`. Installer-only validation
rejects this mode before starting any service. Listener failure does not stop
routing; the engine retries listener setup after five seconds. The desktop now prepares a separate supervised session job; its deployment and
actual login acceptance remain open.

## Desktop application

`native/platform/macos/desktop/` owns the AppKit window, sealed WebKit resources,
application/device discovery and the native message bridge. `interface/` owns
the React view and its projection of the engine document. It imports the actual
`@singz/ui` 1.9.5 archive and complete kit stylesheet. There is no demonstration
graph or synthesized meter fallback in this application.

The bridge accepts only the main frame at `loopz://app/index.html`. It serves
three allowlisted bundled assets, refuses remote navigation, checks the bundle's
resource seal before exposing controls and provides no path, command or Mach
endpoint override. Requests go to a serial background connection owner. Only
the signed editor and Engine identities can use the production endpoint.

The UI disables writes when disconnected, awaiting a fresh snapshot or handling
an uncertain mutation. Lost replies require explicit review of refreshed state;
no graph change is automatically replayed. Local preferences contain selection,
theme and friendly endpoint labels, never the desired routing graph. Source,
output and monitor meters use runtime observations and expire locally after
250 ms without a fresh response.

Application discovery lists normal running applications and explicitly chosen
application bundles. Hardware discovery enumerates SingZ AudioHost inventory
without opening a device. Selecting a source saves its backend identity through
the engine. Privacy buttons open fixed macOS settings panes on explicit use;
discovery itself requests no capture permission. Source and monitor wiring,
dependent deletion and channel resizing use the same validated command batches
as other clients. Connections are also available through a keyboard-accessible
matrix and paginated list for large graphs.

For development, run `npm ci --offline --ignore-scripts` in `interface/` after
populating the package cache, then build the `loopz-desktop` CMake target. Without
the cache, use normal `npm ci --ignore-scripts`. The macOS build includes bundled
UI resources and an ad-hoc development seal. Production transport requires a
Developer ID signed pair; `scripts/stage_desktop.py` stages the desktop and Engine
with the existing SingZ-derived signing configuration. It does not install them,
register a login job or replace the immutable initial routing package.

## Evidence and remaining integration

`loopz-route-edits` exercises command behavior, atomic failures, dependency and
feedback rules, 64-channel resizing, retained identities/receipts and actual
SingZ DSP output at 44.1/48 kHz.

`loopz-editor-controller` exercises two-editor and restart conflicts, atomic
persistence, no-op inode preservation, backend rate rejection, native-request
receipts, failed saves, and edits saved while disconnected then reconciled.

`loopz-editor-protocol` decodes every command variant into real document edits,
checks wire bounds and malformed fields, and verifies exact counters beyond
JavaScript's safe integer range through `UINT64_MAX`.

`loopz-editor-service` exercises authenticated runtime edits and real DSP meter
changes, persistence, unrelated mapping retention, stale external-file edits,
driver disconnect/restoration, identity rejection, request/connection bounds,
expiry, cancellation, shutdown during connection and lost-reply recovery without
replay. All audio providers are injected.

`loopz-editor-service-process` runs separate client and engine processes through
a uniquely named temporary user-domain Mach service. The first client exits,
the engine is terminated cleanly, and a new client reconnects through launchd's
actual replacement delay. Saved multi-device intent, 64-channel edits, meter
changes and old-epoch rejection are verified. The fixture removes its own job
and never touches the installed Engine, driver or real capture endpoints.

`loopz-editor-initialization` checks exclusive first creation, permissions,
simultaneous creators, unsupported formats and preservation of existing primary
and recovery files. Desktop resource and signature tests check URL restrictions,
rejected endpoint overrides and resource tampering. The Node model tests cover
actual meter projection, stale data, command preservation, uncertain replies,
first creation and UI-only preferences.

A separately signed desktop/Engine pair created a real first device through the
production identity pins and displayed actual input inventory. The broker was
absent, so this proves durable editing and discovery, not HAL activation or live
audio. The bounded review job had exited by the editor-close observation;
repeat the actual GUI background-survival check with a fresh job. The separate
process fixture above remains the evidence for engine independence.

Production integration still requires user-job registration, login choice and
broader live UI acceptance. Installed publication, hardware recovery and latency gates in
`product-roadmap.md` also remain open. The previously reviewed installation
package and administrator command are immutable and do not contain these
editor additions; source development does not replace that sealed payload.


## Background session and login choice

The macOS desktop owns a serial `SessionSupervisor`, separate from its editor
connection. On launch it inspects `com.lexasoft.loopz.session` in the current GUI
user domain. A matching loaded job is reused; an unknown definition or process
is preserved and reported. A connected separate editor service is not displaced.
The host verifies the installed `/Applications/LoopZ Engine.app` signature and
read-only `--runtime-capabilities` response before creating a missing session.
Older Engines report an update requirement without starting capture.

The session uses the OS account's home directory, not an environment override.
Its document is `~/Library/Application Support/LoopZ/routes.json`; its private
job lives under `LoopZ/Session/engine.plist`, outside `Library/LaunchAgents`.
Directory ownership, permissions, regular-file shape and symbolic links are
checked. A file lock serializes publishers; a fully synchronized temporary file
is published exclusively. Existing job definitions must exactly match. Child
commands use argument arrays, a five-second deadline and a 64 KiB response bound.

The job starts with `--routing-document`, `--editor-service true` and the tested
64-frame render quantum. `KeepAlive` lets launchd restart unexpected exits, with
a ten-second throttle. Source lookbacks and advertised timing remain unchanged.
Release builds are required for the measured low-latency workload; the earlier
Debug concurrency failure is preserved. Output is discarded rather than growing
an unbounded per-second status log. Editor shutdown disconnects only its client.
The supervisor never edits the document or stops an Engine. Existing engine
reconciliation restores desired devices, sources, wiring and controls.

Login startup is independent. The signed nested `LoopZLogin` helper is declared
by `Contents/Library/LaunchAgents/com.lexasoft.loopz.login.plist`, using a relative
`BundleProgram`, `RunAtLoad` and no `KeepAlive`. It starts/reuses the supervised
session and exits. The Settings action explicitly registers or unregisters this
one-shot helper through `SMAppService`. Turning login startup off therefore does
not unregister the running session Engine. Settings reads macOS's actual enabled,
not-registered and approval-required states; it never stores a substitute login
flag in browser preferences. An approval-required state offers the system Login
Items settings page. Registration is available from `/Applications/LoopZ.app`.

The signed candidate passed real installed-driver recovery with four temporary
capture-free devices. After a launchd-directed forced Engine crash, all four
returned with unchanged saved intent; stereo, 8-channel virtual chaining and
64-channel audio passed strict checks. Temporary graph withdrawal and job removal
passed with unchanged defaults/Core Audio identity. Evidence is
`artifacts/background-session-proof.json`. The 50-check suite and two supervisor
address/undefined sanitizer checks passed. Actual SMAppService registration,
logout/login, deployed canonical paths, physical capture/monitor recovery and
production app-update handling remain separate acceptance gates. No login item
was registered by validation.

Repeat the opt-in check only with no existing LoopZ graph:

```sh
python3 scripts/check_background_routing.py --package /absolute/signed/desktop-review
```

It uses a unique temporary job, a capture-free graph and the installed routing
driver. Live identity is checked before asking launchd to signal the exact job.
Every normal/exceptional exit attempts graph withdrawal and job removal in a
`finally` block. Metadata records cleanup state; a previous unfinished check
blocks another run. Abrupt process termination can still require explicit cleanup.
The successful existing-slot installation does not establish fresh-slot support.

The original check opens new clients after the crash. That proves restored IO,
but does not prove survival of clients that were already connected. A newer
Engine candidate advertises `continuousRoutingProbe: 1` in its read-only runtime
capabilities and supports this stricter mode:

```sh
python3 scripts/check_background_routing.py \
  --package /absolute/signed/desktop-review --continuous-clients
```

It keeps the same stereo, 8-channel virtual-chain and 64-channel clients open
across the verified temporary Engine job's crash. The diagnostic and controller
use a bounded stdio handshake. A clean baseline must pass before the controller
is permitted to trigger that crash. Device and stream IDs, liveness, sample rate,
HAL clock origin and client stream generation are checked throughout. Audio
timestamps, nonfinite samples, buffer bounds and render failures remain checked
during the interruption. Silence is counted and reported separately there.
Following the restoration acknowledgement, one explicitly reported second of
priming remains in the interruption interval; a separate three-second interval
must then pass strict per-sample and per-channel marker checks without reopening
clients. This diagnostic currently requires 48 kHz and existing microphone
authorization; it never requests permission or opens a physical endpoint.

The recorder is portable C++ with preallocated callback storage and an atomic
phase handoff. Native identity/clock queries and child-process control stay in
the macOS adapter and opt-in script. Invalid commands, deadlines, a child exiting
early and incomplete cleanup fail the check. Withdrawal and job removal are
attempted independently, and missing-job verification distinguishes actual
absence from an inspection failure.

`artifacts/continuous-client-proof.json` records offline validation of this
diagnostic, its controller and cleanup. It is not an installed recovery pass.
The driver rebinding candidate in `artifacts/driver-handoff-proof.json` is also
not installed. The signed desktop package awaiting administrator authorization
predates both changes and remains unchanged.

Real Chrome output can also be checked independently of application capture:

```sh
python3 scripts/check_chrome_routing.py \
  --package /absolute/signed/desktop-review --output-only
```

This opt-in check requires an Engine advertising `virtualInputSineProbe: 1` and
an existing microphone grant; it never requests access. It launches an owned
headless instance of installed Chrome with a temporary profile, explicitly
selects one private virtual output, and observes stereo 48 kHz tones through
512- and 64-frame clients. Browser-only discovery permission exposes output
labels without opening media input. No raw audio is saved. The sine profile
checks signal and timing health but returns null marker correlation/delay.

Chrome 155.0.8059.85 passed these profiles, including owned-process shutdown,
device withdrawal and unchanged defaults/Core Audio. The browser fixture uses
public Playwright server/process ownership APIs; cleanup closes the remote
connection, server, local HTTP listener and stdin. An earlier retained controller
handle failure and the verified external-state repair remain in the evidence.
`artifacts/chrome-output-compatibility-proof.json` binds the final check and its
diagnostic/scope regressions. This is not a latency comparison or full browser
application-capture pass.

Without `--output-only`, the harness additionally attempts application capture
and shared fan-out. It first rejects any selected process outside its owned
Chrome instance or output outside the private sink. macOS's app-family selection
can include helpers from another Chrome profile. In that case the test stops;
it must wait for an isolated session or receive explicit human authorization
before `--allow-existing-chrome-helpers` is used. That option broadens only the
permitted process set; unrelated output devices remain forbidden.

Device lifecycle and unrelated-route continuity have a separate opt-in check:

```sh
python3 scripts/check_device_lifecycle.py \
  --package /absolute/signed/desktop-review
```

It creates two private stereo devices with the existing guarded session host.
One 64-frame, 48 kHz client stays open for 30 seconds while the other device is
renamed, has playback hidden/restored, receives mute/gain controls, is disabled
and re-enabled, loses/regains pass-through, and is deleted/restored under the
same saved UID. Changes go through the native atomic document writer and the
Engine's actual reload/transaction path. A metadata-only native observer checks
real public names, liveness, rates, channel counts and object/stream identity.
The restored device must also pass five seconds of stereo IO. No physical
endpoint, permission prompt, production document or login setting is used.

`artifacts/device-lifecycle-live-proof.json` records the passing sequence and
one uninterrupted unrelated marker client. Hiding playback recreates the
edited endpoint's public input stream on the tested host; the test records this
without claiming that an existing client on that endpoint survived. Only
control-only edits require that edited stream's identity to remain unchanged.
The unrelated streaming device and streams must stay unchanged throughout.
Actual active edited-input recovery and desktop-driven lifecycle remain open.

The stricter active-input visibility check is:

```sh
python3 scripts/check_active_input_visibility.py \
  --package /absolute/signed/desktop-review
```

It holds one input-only client across four hide/show cycles. An independent
owned virtual route supplies the signal; the edited pass-through has zero gain.
Five-second readers before and after the 28-second interval give separate
baseline/reopen observations. A fresh-reader pass cannot overwrite a failure
of the existing client. Process IDs are recorded for each client and visibility
observation. Temporary generator termination is explicit and limited to the
owned child; the normal private graph/job/default/Core Audio cleanup follows.

Installed continuity fails on the tested host, while baseline and fresh-reader
signal checks pass. The workspace driver correction preserves the unchanged
input clock at visibility-only approval and rollback; actual installed IO with
that correction remains unverified. The evidence and retained failures are
bound in `artifacts/active-input-visibility-proof.json`. No diagnostic changes
the user's routing document or requests permission.

Native keyboard acceptance uses the shared production WebKit preferences:

```sh
node interface/scripts/test-webkit.mjs artifacts/webkit-native-keyboard keyboard
node interface/scripts/test-webkit.mjs artifacts/webkit-native-keyboard-sanitized keyboard sanitized
```

The private fixture sends a bounded set of NSEvent key-down/up pairs through
its own NSWindow. It never posts system events, uses an event tap or registers
this command on the product bridge. Normal and address/undefined runs each
receive 18 trusted key events and pass 67 assertions, including key/modifier
identity and rejected fixture requests. Settings traversal, modal focus,
activation/shortcuts and routing socket selection/cancellation are exercised;
no edit or system action is sent. The original failure showed a trusted Tab
escaping the controls because `WKPreferences.tabFocusesLinks` was not enabled.
`loopzConfigureWebView` now supplies that setting in the desktop and fixtures.

`artifacts/native-keyboard-proof.json` records these checks, 17 appearance and
15 recovery-guidance checks, 18 real bridge checks in each normal/instrumented
build, and the resource/signature regressions. It includes a WebKit snapshot
using the actual SingZ 1.9.5 components. These results cover the fixture's native
window delivery; physical/system keyboard delivery, production menus, launch
focus and VoiceOver are not certified. The prepared combined recovery package
and installed apps remain unchanged; this desktop correction needs later
deployment.

## Routing Undo and Redo

The workspace editor now exposes Undo/Redo with SingZ buttons, Command-Z /
Command-Shift-Z outside text editors and dialogs, and native Edit menu actions.
The menu forwards text editing to WebKit's undo manager; routing actions use
the existing authenticated edit endpoint and its epoch/generation fence.
Requests contain exactly one `history: "undo" | "redo"` action, never a
replacement document or a mix of history and commands. Older Engine snapshots
without both history flags hide the controls.

`LoopZ::Core` owns a platform-independent journal of canonical intent snapshots,
bounded to 32 edits and 8 MiB of retained snapshot payload. Bookkeeping and
transient preparation allocations are outside that payload limit. An oversized
document remains editable but cannot retain history beyond the budget. History
is session-local and resets when the Engine restarts or observes changed
external intent. Backend-only format receipts are excluded from the intent
comparison and current receipts are preserved during traversal. This is not an
audit log of unobserved external edits that return to the same intent.

The controller prepares a history transition before saving and publishes its
cursor only after durable save succeeds. A failed save leaves the cursor and
desired graph unchanged; a no-op retains redo, while a new edit branches away
from redo. Undoing first creation leaves a saved empty document. Redoing creation
or undoing deletion restores the same saved device identities. A missing reply
uses the existing uncertain-save review flow and is never replayed automatically.
Saved intent and active graph generations remain distinct during restoration.

`artifacts/edit-history-proof.json` binds portable-core, controller, protocol,
authenticated XPC, frontend and real-WebKit tests, including address/undefined
sanitizer runs. The private WebKit checks exercise native menu routing actions
and ensure a menu action with text focus does not traverse routing history;
they do not certify typed-text undo, physical keyboard/menu delivery, VoiceOver,
live-audio history traversal or deployed apps. The prepared combined recovery
update and installed system payload remain unchanged; these editor changes
require a later signed deployment.

## Native Edit menu acceptance

The desktop and private WebKit fixture now construct Edit menus with the same
`loopzCreateEditingMenu` function. Undo and Redo retain explicit native targets;
Cut, Copy, Paste and Select All continue through the normal responder chain.
Redo uses the shifted `Z` key equivalent with Command/Shift. An isolated test
with correctly formed Shift-Z events rejected the unshifted `z` equivalent;
the shifted equivalent passes both normal and instrumented runs.

The fixture dispatches native key equivalents through this shared menu and
plain typing through its own NSWindow. Shift is retained in both NSEvent
character fields, as required by Apple's
[charactersIgnoringModifiers specification](https://developer.apple.com/documentation/appkit/nsevent/charactersignoringmodifiers).
Command-Z and Command-Shift-Z each advance the routing generation exactly once.
A trusted native text key changes the rename field; menu shortcuts undo, redo
and undo that text while leaving both routing revision fields unchanged. Saving
then preserves the intended name through runtime reconstruction.

Unsupported keys/modifiers, non-Boolean flags and extra fields are rejected by
the private handler. The production bridge rejects native-key injection. No
system events, event taps, clipboard access, real audio or OS consent prompts
are used. `artifacts/native-history-menu-proof.json` binds the runs, exact
sources and protected-payload checks. Physical/system keyboard delivery,
locale/layout coverage, full production menus and deployed acceptance remain
open. This extends the narrower text-focus isolation evidence in the initial
history proof; its archived sources and reports remain unchanged.
