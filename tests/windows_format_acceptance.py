"""Opt-in desktop/admin acceptance of actual public channel/rate formats.

Creates one isolated pass-through mix alongside saved mixes, exercises exact
public formats through the saved half-gain mixer, and removes its root adapter.
System defaults, session volumes and boot settings remain unchanged.
LOOPZ_FORMAT_AUTOMATIC_SETUP=1 uses the installed engine/service and needs no
privileged helper or pnputil. LOOPZ_FORMAT_PROBE_EXECUTABLE pins a separate probe. The opt-in
LOOPZ_FORMAT_PAUSE_SAVED_MIXES=1 diagnostic temporarily disables saved mixes and
restores their controls in cleanup; its results do not prove concurrent mixing.
Usage: SCRIPT APPDIR REPORT [smoke|full|single RATE CHANNELS|rates CHANNELS|transitions CHANNELS].
Full covers all 192 advertised formats; rates checks six rates at one width.
Transitions repeats 48 -> 44.1 kHz five times at one width; it is a separate
continuity test and does not substitute for the full format matrix.
"""

import hashlib
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from windows_acceptance_report import write_report

folder = Path(sys.argv[1]).resolve()
report_path = Path(sys.argv[2]).resolve()
mode = sys.argv[3] if len(sys.argv) > 3 else "smoke"
if mode not in ("smoke", "full", "single", "rates", "transitions"):
    raise ValueError(
        "Use smoke, full, single RATE CHANNELS, rates CHANNELS, or transitions CHANNELS"
    )
single_rate = int(sys.argv[4]) if mode == "single" else 48000
single_channels = (
    int(sys.argv[5])
    if mode == "single"
    else int(sys.argv[4]) if mode in ("rates", "transitions") else 2
)
if single_rate not in (44100, 48000, 88200, 96000, 176400, 192000) or not (
    2 <= single_channels <= 64 and single_channels % 2 == 0
):
    raise ValueError("Unsupported format")
if report_path.exists():
    raise RuntimeError("A fresh report path is required")
control = folder / "loopz-windows-control.exe"
probe = Path(
    os.environ.get("LOOPZ_FORMAT_PROBE_EXECUTABLE", folder / "loopz-windows-cable-probe.exe")
)
automatic_setup = os.environ.get("LOOPZ_FORMAT_AUTOMATIC_SETUP") == "1"
probe_mode = os.environ.get("LOOPZ_FORMAT_PROBE", "--format-roundtrip")
recording_ms = int(os.environ.get("LOOPZ_FORMAT_RECORD_MS", "1500"))
if not 1500 <= recording_ms <= 300000:
    raise ValueError("Recording duration must be 1500..300000 ms")
# Mirror the native probe's existing worst-case allocation ceiling. For full
# sweeps, validate the widest/highest profile before any graph mutation.
# Explicit stress mode uses a fixed queue instead of storing the entire recording.
budget_rate = single_rate if mode in ("single", "transitions") else 192000
budget_channels = 64 if mode in ("full", "smoke") else single_channels
if (
    probe_mode != "--format-stress-exclusive-float"
    and budget_rate * (recording_ms // 1000 + 1) * budget_channels > 31 * 192000 * 64
):
    raise ValueError("Recording exceeds the probe's fixed sample storage budget")
if probe_mode not in (
    "--format-roundtrip",
    "--format-roundtrip-raw",
    "--format-roundtrip-short-period",
    "--format-roundtrip-leased-period",
    "--format-roundtrip-exclusive",
    "--format-roundtrip-exclusive-float",
    "--format-stress-exclusive-float",
    "--format-precision-exclusive-float",
    "--latency-roundtrip-exclusive",
):
    raise ValueError("Unsupported format probe")
if probe_mode == "--latency-roundtrip-exclusive" and (
    mode != "single" or single_rate != 48000 or single_channels != 2 or recording_ms < 15000
):
    raise ValueError("Latency probe requires single 48000 2 and at least 15000 ms")
fault_mode = os.environ.get("LOOPZ_FORMAT_WORKER_FAULT", "")
if fault_mode:
    if (
        fault_mode != "deadline"
        or not automatic_setup
        or mode != "single"
        or single_rate != 48000
        or single_channels != 2
        or recording_ms < 10000
    ):
        raise ValueError("Worker fault fixture requires automatic single 48000 2 and >=10000 ms")
    from windows_worker_recovery_fault import inject_deadline_failure

identifier = "format-acceptance-" + str(uuid.uuid4())
owner = (
    subprocess.check_output(["whoami.exe", "/user", "/fo", "csv", "/nh"], text=True, timeout=10)
    .strip()
    .split(",")[-1]
    .strip('"')
)
key = hashlib.sha256((owner + "\0" + identifier).encode("utf-8")).hexdigest()
report = {
    "ok": False,
    "mode": mode,
    "mixId": identifier,
    "mixKey": key,
    "probeMode": probe_mode,
    "recordingMs": recording_ms,
    "profiles": [],
    "expectedProfiles": (
        192
        if mode == "full"
        else (
            1 if mode == "single" else 6 if mode == "rates" else 10 if mode == "transitions" else 18
        )
    ),
    "cleanup": False,
}
expect_native_domain = os.environ.get("LOOPZ_FORMAT_EXPECT_RENDER_DOMAIN") == "1"
pause_saved_mixes = os.environ.get("LOOPZ_FORMAT_PAUSE_SAVED_MIXES") == "1"
report["savedMixesTemporarilyPaused"] = pause_saved_mixes
report["automaticSetup"] = automatic_setup
report["workerFaultMode"] = fault_mode
if fault_mode:
    report["scope"] = "worker recovery fault injection; not audio continuity acceptance"
report["probeExecutable"] = str(probe)
report["probeHash"] = hashlib.sha256(probe.read_bytes()).hexdigest()
created = False
provision_attempted = False


def test_adapter_exists():
    # Setup rolls newly created roots back if endpoint format setup fails.
    # Check both present and phantom nodes so already-removed roots are a
    # successful cleanup without hiding real pnputil failures.
    locate = ctypes.WinDLL("cfgmgr32").CM_Locate_DevNodeW
    locate.argtypes = (ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD)
    locate.restype = wintypes.DWORD
    node = wintypes.DWORD()
    result = locate(ctypes.byref(node), "ROOT\\LOOPZAUDIO\\LZ_" + key, 1)
    if result == 0x0D:  # CR_NO_SUCH_DEVNODE
        return False
    if result != 0:
        raise RuntimeError(f"Cannot inspect test adapter: CONFIGRET 0x{result:x}")
    return True


def state():
    result = subprocess.run([str(control), "--state"], check=True, capture_output=True, timeout=30)
    return json.loads(result.stdout.decode("utf-8"))


def defaults():
    result = subprocess.run([str(probe), "--defaults"], check=True, capture_output=True, timeout=30)
    return json.loads(result.stdout.decode("utf-8"))


def save_report():
    write_report(report_path, report)


def qpc_ticks():
    value = ctypes.c_longlong()
    if not ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value)):
        raise ctypes.WinError()
    return value.value


def recording_probe(arguments):
    # Observe the running engine twice without doing IPC on an audio thread.
    # Raw-probe QPC timestamps later establish whether both snapshots were
    # wholly inside the recorded interval, excluding initialization/startup.
    observations = []
    with subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
        try:
            started = time.monotonic()
            delays = [1.6, 2.2]
            while delays[-1] + 0.6 < 1.2 + recording_ms / 1000 - 0.25:
                delays.append(round(delays[-1] + 0.6, 6))
            for delay in delays:
                time.sleep(max(0, started + delay - time.monotonic()))
                if child.poll() is not None:
                    break
                begin = qpc_ticks()
                runtime = state()["snapshot"]["runtime"]
                end = qpc_ticks()
                active = next(d for d in runtime["devices"] if d["id"] == identifier)
                observations.append(
                    {
                        "beginQpcTicks": begin,
                        "endQpcTicks": end,
                        "inputHistory": active.get("microphone", {}).get("inputHistory", {}),
                        "transportTiming": active.get("microphone", {}).get("transportTiming", {}),
                        "publicationRead": active.get("microphone", {}).get("publicationRead", {}),
                        "renderTiming": runtime.get("renderTiming", {}),
                        "sceneTiming": active.get("sceneTiming", {}),
                        "busLagNs": active.get("busLagNs"),
                        "renderRate": active.get("renderRate"),
                        "sources": active.get("sources", []),
                    }
                )
                if fault_mode and delay >= 2.2 and "faultInjection" not in report:
                    report["faultInjection"] = inject_deadline_failure(folder, state, identifier)
                    save_report()
            probe_timeout = 90 if recording_ms <= 30000 else recording_ms / 1000 + 180
            stdout, stderr = child.communicate(
                timeout=max(1, probe_timeout - (time.monotonic() - started))
            )
        except BaseException:
            child.kill()
            child.communicate()
            raise
        report["duringProbe"] = observations
        try:
            decoded = json.loads(stdout.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            decoded = None
        if decoded:
            report["probeResult"] = decoded
            timing = decoded.get("diagnostics", {})
            for observation in observations:
                observation["withinRecording"] = (
                    timing["recordingStartQpcTicks"] <= observation["beginQpcTicks"]
                    and observation["endQpcTicks"] <= timing["streamEndQpcTicks"]
                    if "recordingStartQpcTicks" in timing and "streamEndQpcTicks" in timing
                    else None
                )
        save_report()
        return subprocess.CompletedProcess(arguments, child.returncode, stdout, stderr)


before = state()
if automatic_setup and not before.get("microphoneSetup", {}).get("enabled"):
    raise RuntimeError("Automatic setup requires the production engine coordinator")
original_document = before["snapshot"]["document"]
original_defaults = defaults()
original_render_rates = {
    d["id"]: d.get("renderRate") for d in before["snapshot"]["runtime"]["devices"]
}
report["renderRatesBeforeFixture"] = original_render_rates
original_microphones = {
    d["id"]: d["microphone"]["uid"]
    for d in before["snapshot"]["runtime"]["devices"]
    if d.get("microphone", {}).get("uid")
}
with tempfile.TemporaryDirectory(prefix="loopz-format-") as temporary:
    request = Path(temporary) / "request.json"

    def edit(commands):
        current = state()
        request.write_text(
            json.dumps(
                {
                    "version": 1,
                    "operation": "edit",
                    "request": {
                        "version": 1,
                        "expected": current["snapshot"]["revision"],
                        "commands": commands,
                    },
                }
            ),
            encoding="utf-8",
        )
        reply = subprocess.run(
            [str(control), "--request", str(request)], capture_output=True, timeout=60
        )
        decoded = json.loads(reply.stdout.decode("utf-8"))
        assert reply.returncode == 0 and decoded["ok"], decoded

    try:
        if pause_saved_mixes:
            edit(
                [
                    {"type": "setDeviceControls", "device": d["id"], "controls": {"enabled": False}}
                    for d in original_document["devices"]
                ]
            )
        created = True  # Cleanup also covers an edit applied before a lost reply.
        edit(
            [
                {
                    "type": "createDevice",
                    "id": identifier,
                    "name": "Format acceptance",
                    "passThrough": True,
                    "channels": single_channels,
                    "sampleRate": single_rate,
                },
                {"type": "setDeviceControls", "device": identifier, "controls": {"gain": 0.5}},
            ]
        )
        created = True
        channels_to_check = (
            range(2, 65, 2)
            if mode == "full"
            else (single_channels,) if mode in ("single", "rates", "transitions") else (2, 8, 64)
        )
        rates_to_check = (
            (single_rate,)
            if mode == "single"
            else (
                (48000, 44100) * 5
                if mode == "transitions"
                else (48000, 44100, 88200, 96000, 176400, 192000)
            )
        )
        for rate in rates_to_check:
            for channels in channels_to_check:
                report["running"] = {"sampleRate": rate, "channels": channels}
                save_report()
                edit(
                    [
                        {"type": "setDeviceRate", "device": identifier, "sampleRate": rate},
                        {"type": "resizeDevice", "device": identifier, "channels": channels},
                    ]
                )
                provision_attempted = True
                if not automatic_setup:
                    setup = subprocess.run(
                        [
                            str(folder / "loopz-driver-setup.exe"),
                            "--sync",
                            str(Path(os.environ["LOCALAPPDATA"]) / "LoopZ" / "routing.json"),
                            owner,
                        ],
                        capture_output=True,
                        timeout=120,
                    )
                    assert setup.returncode == 0, (setup.stdout + setup.stderr).decode(
                        "utf-8", errors="replace"
                    )
                deadline = time.monotonic() + 45
                while True:
                    snapshot = state()
                    devices = snapshot["snapshot"]["runtime"]["devices"]
                    current = next((d for d in devices if d["id"] == identifier), {})
                    setup_state = snapshot.get("microphoneSetup", {})
                    ready = not automatic_setup or (
                        setup_state.get("accepted")
                        and setup_state.get("generation")
                        == snapshot["snapshot"]["revision"]["generation"]
                        and setup_state.get("devices", {}).get(identifier, {}).get("phase")
                        == "ready"
                    )
                    if current.get("microphone", {}).get("connected") and ready:
                        break
                    assert time.monotonic() < deadline, snapshot
                    time.sleep(0.5)
                for identity, uid in original_microphones.items():
                    assert (
                        next(d for d in devices if d["id"] == identity)["microphone"]["uid"] == uid
                    )
                report["renderRateBeforeProbe"] = current.get("renderRate")
                report["renderRatesDuringFixture"] = {d["id"]: d.get("renderRate") for d in devices}
                if expect_native_domain:
                    assert (
                        current.get("renderRate") == rate
                    ), "Independent mix is not rendered at its nominal rate"
                    assert all(
                        report["renderRatesDuringFixture"].get(identity) == original
                        for identity, original in original_render_rates.items()
                    ), "A disconnected saved mix changed render rate"
                report["inputHistoryBeforeProbe"] = current.get("microphone", {}).get(
                    "inputHistory", {}
                )
                report["publicationReadBeforeProbe"] = current.get("microphone", {}).get(
                    "publicationRead", {}
                )
                report["transportTimingBeforeProbe"] = current.get("microphone", {}).get(
                    "transportTiming", {}
                )
                report["renderTimingBeforeProbe"] = snapshot["snapshot"]["runtime"].get(
                    "renderTiming", {}
                )
                save_report()
                arguments = [str(probe), probe_mode, key, str(rate), str(channels)]
                if recording_ms != 1500:
                    arguments.append(str(recording_ms))
                result = recording_probe(arguments)
                if result.returncode:
                    report["failureRuntime"] = state()["snapshot"]["runtime"]
                    save_report()
                if fault_mode:
                    assert report.get("faultInjection", {}).get("ok"), report
                else:
                    assert result.returncode == 0, (result.stdout + result.stderr).decode(
                        "utf-8", errors="replace"
                    )
                measured = json.loads(result.stdout.decode("utf-8"))
                if fault_mode:
                    report["expectedWaveformFailure"] = not measured["ok"]
                else:
                    assert measured["ok"]
                if probe_mode == "--format-stress-exclusive-float":
                    assert measured["analysisMode"] == "bounded-streaming"
                    assert measured["diagnostics"]["probeDroppedFrames"] == 0
                    assert measured["diagnostics"]["probeInvalidSamples"] == 0
                    assert measured["diagnostics"]["probePendingPackets"] == 0
                    assert (
                        measured["diagnostics"]["probeAcceptedFrames"]
                        == measured["diagnostics"]["probeAnalyzedFrames"]
                    )
                    assert (
                        measured["diagnostics"]["probeSampleStorageBytes"]
                        <= 32 * 4096 * channels * 4
                    )
                assert measured["sampleRate"] == rate
                assert measured["channels"] == channels and len(measured["signal"]) == channels
                active = next(
                    d for d in state()["snapshot"]["runtime"]["devices"] if d["id"] == identifier
                )
                measured["transportTiming"] = active.get("microphone", {}).get(
                    "transportTiming", {}
                )
                measured["inputHistoryBeforeProbe"] = report.pop("inputHistoryBeforeProbe", {})
                measured["inputHistoryAfterProbe"] = active.get("microphone", {}).get(
                    "inputHistory", {}
                )
                measured["duringProbe"] = report.pop("duringProbe", [])
                report.pop("probeResult", None)
                assert defaults() == original_defaults, "System audio defaults changed"
                report["profiles"].append(measured)
                report.pop("running", None)
                save_report()
    except BaseException as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        cleanup_errors = []
        if created:
            try:
                if any(d["id"] == identifier for d in state()["snapshot"]["document"]["devices"]):
                    edit([{"type": "deleteDevice", "device": identifier}])
            except Exception as error:
                cleanup_errors.append(str(error))
        if provision_attempted:
            try:
                if test_adapter_exists() and not automatic_setup:
                    subprocess.run(
                        ["pnputil.exe", "/remove-device", "ROOT\\LOOPZAUDIO\\LZ_" + key],
                        check=True,
                        capture_output=True,
                        timeout=60,
                    )
                deadline = time.monotonic() + (45 if automatic_setup else 10)
                while test_adapter_exists() or (
                    automatic_setup
                    and identifier in state().get("microphoneSetup", {}).get("devices", {})
                ):
                    assert time.monotonic() < deadline, "Test adapter was not removed"
                    time.sleep(0.2)
            except Exception as error:
                cleanup_errors.append(str(error))
        if pause_saved_mixes:
            try:
                edit(
                    [
                        {
                            "type": "setDeviceControls",
                            "device": d["id"],
                            "controls": {"enabled": d["routing"]["enabled"]},
                        }
                        for d in original_document["devices"]
                    ]
                )
            except Exception as error:
                cleanup_errors.append("Restore saved mix controls: " + str(error))
        try:
            final = state()
            assert final["snapshot"]["document"] == original_document, "Existing saved mix changed"
            assert defaults() == original_defaults, "System defaults changed"
            final_microphones = {
                d["id"]: d.get("microphone", {}).get("uid")
                for d in final["snapshot"]["runtime"]["devices"]
            }
            assert all(final_microphones.get(i) == uid for i, uid in original_microphones.items())
        except Exception as error:
            cleanup_errors.append(str(error))
        report["cleanup"] = not cleanup_errors
        report["cleanupErrors"] = cleanup_errors
        report["ok"] = (
            not report.get("error")
            and not cleanup_errors
            and len(report["profiles"]) == report["expectedProfiles"]
        )
        save_report()
        if cleanup_errors:
            raise RuntimeError("Format acceptance cleanup failed: " + "; ".join(cleanup_errors))
print(
    json.dumps(
        {
            "ok": report["ok"],
            "profiles": len(report["profiles"]),
            "cleanup": report["cleanup"],
            "report": str(report_path),
        }
    )
)
