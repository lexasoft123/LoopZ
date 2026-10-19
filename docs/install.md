# Install and exercise the local prototype

Build and pass the automated checks first. Stage ad-hoc signed local artifacts:

```sh
python3 scripts/prepare_install.py
```

The staging step does not install or run anything. Its default helper arguments
explicitly select a 120-second synthetic stereo test signal. This signal goes
only into the virtual input; no physical output is enabled.

Review `artifacts/feasibility/manifest.json`, then install the prepared artifacts
with administrator authorization:

```sh
sudo python3 scripts/install_local.py
```

The installer verifies signatures and complete package hashes, copies a private
verified snapshot, and prepares replacement files alongside their destinations.
An exclusive installer lock prevents concurrent replacement. It keeps originals
until activation and continuity checks pass; failure attempts file restoration
and broker recovery independently, then verifies restored hashes and runtime
state. Any incomplete recovery is explicitly marked as requiring repair. It never invokes a system audio reset.

Existing installations use a hot update:

```sh
python3 scripts/install_local.py --replace --check
sudo python3 scripts/install_local.py --replace
```

`--update-engine` is an alias for the hot update. `--update-tools` also requires
an identical helper and broker configuration. The complete installed driver
bundle must match the package: incompatible driver changes are rejected before
any file or service changes. The loaded driver is preserved, including its
inode. Updating the processing helper and DSP does not require driver reload.

Stop LoopZ's source/recording clients and engine before updating. Other audio
clients may remain active. During the update, a separate AUHAL client writes
zeros to the current hardware output and checks its sample-counter continuity.
`--guard-uid UID` selects another physical output for this check when needed.
No microphone permission or capture is required for installation validation.

Success additionally requires the virtual device to be alive, an authenticated
driver attachment to the matching broker protocol, unchanged default input,
output and system-output UIDs, and unchanged Core Audio service PID/start time.
The resulting evidence is saved to
`/Library/Application Support/LoopZ/install-result.json`.

Fresh installation only succeeds if the device actually appears and attaches
within 20 seconds without a reset. Otherwise it fails and rolls back its files. Filesystem rollback cannot
guarantee that an already-loaded plug-in has been unpublished; residual
activation and changes in runtime/default-device state are reported explicitly.
Fresh activation into an already-running audio server remains unverified.
The standard installer rejects live replacement of a changed driver. A separate,
guarded isolated-host experiment succeeded on the tested macOS release; see
[the activation evidence](latency-comparison.md). It is not a supported general
installer path. Copying a driver to disk does not establish activation.

Start the helper in the GUI login session using the generated launch agent:

```sh
launchctl bootstrap "gui/$(id -u)" artifacts/feasibility/com.lexasoft.loopz.engine.plist
/Library/Application\ Support/LoopZ/loopz-probe --seconds 5 --expect-tone
```

Run two probes at once to exercise independent clients. The synthetic test
expects 440 Hz in the left channel and 880 Hz in the right channel. After
stopping the helper, run the probe with `--expect-silence`.

For real sources, prepare the helper arguments explicitly using the application
bundle ID and microphone UID from the inventory command:

```sh
python3 scripts/prepare_install.py --engine-args-json '["--app","com.example.player","--mic","default","--seconds","60"]'
```

Restaging changes the pinned helper identity when the binary changes; reinstall
the matching broker configuration. Consent is requested by the installed
helper. `--mute-app` opts into muting the source's original playback.
`--monitor-muted UID` exercises a physical output callback silently.
`--no-pass-through` disables the public playback bus to isolate application
capture during validation. These flags are prototype diagnostics.

To stop and unregister the manually started helper:

```sh
launchctl bootout "gui/$(id -u)/com.lexasoft.loopz.engine"
```

Then remove the prototype with `sudo python3 scripts/uninstall_local.py`.
The prototype uses ad-hoc development signing. Developer ID signing,
notarization, a distributable installer and automatic login registration are
separate release gates.

## Timestamped stability check

After updating the installed helper and signing `build/LoopZ Test Source.app`,
run `python3 scripts/soak_test.py --seconds 7200` from the GUI user's session.
The selected fixture outputs only to LoopZ Audio. The helper captures that app
with original playback muted, explicitly disables pass-through, captures the
built-in microphone at 0.01 mix gain and exercises the built-in speaker callback with zeros.
Two independent clients validate each second of output with fixed memory.
Only measurements are saved; no microphone recording is written to disk.

The test stops its two manually registered agents on completion or handled
interruption. It does not register either agent for login. A shorter duration
is a rehearsal and cannot satisfy the two-hour gate. Results and tested binary
hashes are written under `artifacts/soak-*/result.json`.

For the current explicit low-latency configurations:

```sh
python3 scripts/soak_test.py --seconds 20 --latency-frames 768 --driver-period 1024 --driver-queue 0 --without-microphone --without-monitor
python3 scripts/soak_test.py --seconds 600 --latency-frames 1024 --driver-period 128 --driver-queue 0 --exercise-client-churn
```

The first matches the fresh Loopback app-only setup. The second retains the
microphone and muted monitor and opens/closes an extra recording client every
ten seconds. The controller configures the driver's advertised source delay
and the helper's actual lookback together. A post-recording telemetry checkpoint
must acknowledge completion before the helper is stopped. Neither short run
satisfies the two-hour gate. The helper default lookback remains 2048 frames;
general mixdown may require that larger margin.

The smaller-block app-only experiment requires driver build `loopz-timing-3`
and the matching helper:

```sh
python3 scripts/soak_test.py --seconds 20 --block-frames 64 --latency-frames 192 --driver-period 128 --driver-queue 0 --without-microphone --without-monitor
```

The 192-frame lookback is 4 ms at 48 kHz and 4.35 ms at 44.1 kHz, before
measurement uncertainty. This is a requested setting, not a measured result.
Repeat the short strict rehearsal at both rates before any longer regression.
Small blocks require one eligible device-stream app capture and explicitly
disable microphone, monitor, tone, mixdown and pass-through. The graph remains
prepared for a maximum of 256 frames; actual render calls use 64 frames. Capture
history still accepts callbacks up to 4096 frames. The default render block,
source lookback and driver read queue remain unchanged.
