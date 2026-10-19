#!/usr/bin/env python3
"""Exercise the installed graph with two bounded-memory Core Audio clients.

This uses the authorized microphone and a virtual-only test app. The physical
monitor is muted. No recordings are saved, only counters and process metrics.
"""
import argparse
import fcntl
import hashlib
import platform
import json
import os
from pathlib import Path
import plistlib
import re
import signal
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--seconds", type=int, default=7200)
parser.add_argument("--latency-frames", type=int, choices=range(64, 4097, 64))
parser.add_argument("--block-frames", type=int, choices=[64, 128, 256], default=256)
parser.add_argument("--without-microphone", action="store_true")
parser.add_argument("--without-monitor", action="store_true")
parser.add_argument("--exercise-client-churn", action="store_true")
parser.add_argument("--app-mixdown", action="store_true")
parser.add_argument("--driver-queue", type=int, choices=range(0, 2049, 256))
parser.add_argument("--driver-period", type=int, choices=[64, 128, 256, 512, 1024])
args = parser.parse_args()
effective_latency = args.latency_frames if args.latency_frames is not None else 2048
if effective_latency < args.block_frames or effective_latency % args.block_frames:
    raise SystemExit("Source latency must be a positive multiple of the render block size")
if args.block_frames != 256 and (
    not args.without_microphone or not args.without_monitor or args.app_mixdown
):
    raise SystemExit("Small-block experiments require app-only device-stream capture")
if not 5 <= args.seconds <= 86400 - 120:
    raise SystemExit("duration must be between 5 and 86280 seconds")
# The agents and virtual format are shared between rehearsals. A second
# controller must not stop another run's engine or overwrite its latest state.
run_lock = (root / "artifacts" / ".audio-test.lock").open("a")
try:
    fcntl.flock(run_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    raise SystemExit("Another LoopZ audio rehearsal is already running")
domain = f"gui/{os.getuid()}"
engine_label = "com.lexasoft.loopz.engine"
source_label = "com.lexasoft.loopz.testsource"
run = root / "artifacts" / f"soak-{int(time.time())}"
run.mkdir(parents=True)
probe = run / "loopz-probe"
source_app = run / "LoopZ Test Source.app"
latest = root / "artifacts/soak-latest.json"
children = []
handles = []
recording_started = None


def interrupted(signum, frame):
    raise KeyboardInterrupt


signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)
summary = {
    "status": "starting",
    "durationSeconds": args.seconds,
    "directory": str(run),
    "microphone": None if args.without_microphone else "BuiltInMicrophoneDevice",
    "monitor": None if args.without_monitor else "BuiltInSpeakerDevice",
    "sourceLatencyFrames": effective_latency,
    "blockFrames": args.block_frames,
    "monitorMuted": None if args.without_monitor else True,
    "source": "com.lexasoft.loopz.testsource",
    "passThrough": False,
    "appMixdownRequested": args.app_mixdown,
    "clientChurn": [],
    "microphoneGain": 0.01,
    "metrics": [],
    "host": {"macOS": platform.mac_ver()[0], "architecture": platform.machine()},
    "executables": {
        str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [
            Path("/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine"),
            root / "build/loopz-probe",
            root / "build/LoopZ Test Source.app/Contents/MacOS/LoopZ Test Source",
            Path("/Library/Application Support/LoopZ/loopz-broker"),
            Path("/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver/Contents/MacOS/LoopZAudio"),
        ]
    },
}


def save():
    temp = latest.with_suffix(".tmp")
    temp.write_text(json.dumps(summary, indent=2) + "\n")
    temp.replace(latest)


def control(*arguments, check=False):
    return subprocess.run(
        ["launchctl", *arguments], capture_output=True, text=True, check=check, timeout=10
    )


def bootstrap(label, arguments, name):
    path = run / f"{name}.plist"
    path.write_bytes(
        plistlib.dumps(
            {
                "Label": label,
                "ProgramArguments": arguments,
                "RunAtLoad": True,
                "ProcessType": "Interactive",
                "StandardOutPath": str(run / f"{name}.stdout.log"),
                "StandardErrorPath": str(run / f"{name}.stderr.log"),
            }
        )
    )
    control("bootstrap", domain, str(path), check=True)


def stop_helpers():
    errors = []
    for label in [engine_label, source_label]:
        try:
            control("bootout", f"{domain}/{label}")
        except Exception as error:
            errors.append(f"{label}: {error}")
    return errors


def process_metrics(label):
    result = control("print", f"{domain}/{label}")
    match = re.search(r"^\s*pid = (\d+)", result.stdout, re.M)
    if not match:
        raise RuntimeError(f"{label} is not running: {result.stdout[-1500:]} {result.stderr}")
    pid = int(match[1])
    stats = subprocess.run(
        ["ps", "-p", str(pid), "-o", "rss=,%cpu="], text=True, capture_output=True, check=True
    ).stdout.split()
    return {"pid": pid, "rssKiB": int(stats[0]), "cpuPercent": float(stats[1])}


save()
try:
    # Keep live clients and later churn launches on the same executable bytes,
    # even when another build replaces files in the shared build directory.
    shutil.copy2(root / "build/loopz-probe", probe)
    shutil.copytree(root / "build/LoopZ Test Source.app", source_app)
    subprocess.run(
        ["codesign", "--force", "--sign", "-", "--timestamp=none", str(source_app)],
        check=True,
        capture_output=True,
    )
    for path in [probe, source_app]:
        subprocess.run(["codesign", "--verify", "--strict", str(path)], check=True)
    summary["toolSnapshot"] = {
        str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [probe, source_app / "Contents/MacOS/LoopZ Test Source"]
    }
    save()
    stop_helpers()
    time.sleep(4)  # fresh broker region after the previous helper disconnects
    timing = subprocess.run(
        [str(probe), "--driver-timing"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if timing.returncode:
        summary["legacyDriver"] = True
        if args.driver_queue is not None or args.driver_period is not None:
            raise RuntimeError("Resident driver does not support timing configuration")
    else:
        current = json.loads(timing.stdout)
        summary["driverTimingBefore"] = current
        period = args.driver_period if args.driver_period is not None else current["periodFrames"]
        queue = args.driver_queue if args.driver_queue is not None else current["queueFrames"]
        applied = subprocess.run(
            [
                str(probe),
                "--set-timing",
                f"{period},{queue},{effective_latency}",
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
        summary["driverTimingConfigured"] = json.loads(applied.stdout)
        if any(
            summary["driverTimingConfigured"].get(key) != value
            for key, value in {
                "periodFrames": period,
                "queueFrames": queue,
                "sourceFrames": effective_latency,
            }.items()
        ):
            raise RuntimeError("Driver timing configuration mismatch")
    bootstrap(
        source_label,
        [
            str(source_app / "Contents/MacOS/LoopZ Test Source"),
            "--send-tone",
            "--timestamped",
            "--seconds",
            str(args.seconds + 100),
            "--frequency-left",
            "1100",
            "--frequency-right",
            "2200",
        ],
        "source",
    )
    for attempt in range(20):
        source_log = run / "source.stdout.log"
        if source_log.exists() and "source-started" in source_log.read_text():
            break
        time.sleep(1)
    else:
        raise RuntimeError("Virtual-only test source did not start")
    bootstrap(
        engine_label,
        [
            "/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
            "--app",
            "com.lexasoft.loopz.testsource",
            "--mute-app",
            *(["--app-mixdown"] if args.app_mixdown else []),
            *([] if args.without_microphone else ["--mic", "BuiltInMicrophoneDevice"]),
            *([] if args.without_monitor else ["--monitor-muted", "BuiltInSpeakerDevice"]),
            "--mic-gain",
            "0.01",
            "--no-pass-through",
            "--seconds",
            str(args.seconds + 80),
            "--latency-frames",
            str(effective_latency),
            "--block-frames",
            str(args.block_frames),
        ],
        "engine",
    )
    for attempt in range(90):
        out = run / "engine.stdout.log"
        error = run / "engine.stderr.log"
        if error.exists() and error.read_text().strip():
            raise RuntimeError(error.read_text())
        if out.exists() and '"event":"started"' in out.read_text():
            break
        time.sleep(1)
    else:
        raise RuntimeError("Helper did not start; check capture permission")
    started_event = next(
        json.loads(line) for line in out.read_text().splitlines() if '"event":"started"' in line
    )
    if started_event.get("sourceLatencyFrames") != effective_latency:
        raise RuntimeError("Helper source delay does not match the configured driver metadata")
    summary["renderLeadFrames"] = started_event.get("renderLeadFrames", 0)
    if started_event.get("blockFrames", 256) != args.block_frames:
        raise RuntimeError("Helper render block size does not match the requested experiment")
    # Confirm the selected app carrier before starting the sustained gate.
    baseline = subprocess.run(
        [
            str(probe),
            "--seconds",
            "3",
            "--expect-tone",
            "--frequency-left",
            "1100",
            "--frequency-right",
            "2200",
        ],
        capture_output=True,
        text=True,
        timeout=15,
    )
    summary["baseline"] = {
        "returncode": baseline.returncode,
        "stdout": baseline.stdout,
        "stderr": baseline.stderr,
    }
    if baseline.returncode:
        raise RuntimeError("The app carrier did not pass baseline validation")
    for index in [1, 2]:
        output = (run / f"consumer-{index}.stdout.log").open("w")
        error = (run / f"consumer-{index}.stderr.log").open("w")
        handles.extend([output, error])
        children.append(
            subprocess.Popen(
                [
                    str(probe),
                    "--streaming",
                    "--seconds",
                    str(args.seconds),
                    "--expect-audio",
                    "--frequency-left",
                    "1100",
                    "--frequency-right",
                    "2200",
                ],
                stdout=output,
                stderr=error,
            )
        )
    started = time.monotonic()
    recording_started = started
    summary["status"] = "running"
    save()
    print(
        json.dumps({"event": "started", "seconds": args.seconds, "directory": str(run)}), flush=True
    )
    next_metric = 0
    next_churn = 10
    while any(child.poll() is None for child in children):
        elapsed = time.monotonic() - started
        if elapsed > args.seconds + 30:
            raise RuntimeError("Recording client exceeded its shutdown deadline")
        if elapsed >= next_metric:
            metric = {
                "elapsedSeconds": round(elapsed, 3),
                "engine": process_metrics(engine_label),
                "source": process_metrics(source_label),
            }
            for index, child in enumerate(children, 1):
                if child.poll() is None:
                    stats = subprocess.run(
                        ["ps", "-p", str(child.pid), "-o", "rss=,%cpu="],
                        capture_output=True,
                        text=True,
                        check=True,
                    ).stdout.split()
                    metric[f"consumer{index}"] = {
                        "pid": child.pid,
                        "rssKiB": int(stats[0]),
                        "cpuPercent": float(stats[1]),
                    }
                progress = run / f"consumer-{index}.stdout.log"
                lines = progress.read_text().splitlines()
                if lines:
                    progress_data = json.loads(lines[-1])
                    metric[f"consumer{index}Progress"] = progress_data
                    if any(
                        progress_data.get(key, 0)
                        for key in [
                            "quietFrames",
                            "callbackFailures",
                            "nonfiniteSamples",
                            "badWindows",
                            "timestampErrors",
                            "queueOverruns",
                        ]
                    ):
                        raise RuntimeError(
                            f"Recording client {index} reported a signal or continuity failure: {progress_data}"
                        )
            summary["metrics"].append(metric)
            summary["elapsedSeconds"] = round(elapsed, 3)
            save()
            print(json.dumps({"event": "metrics", **metric}), flush=True)
            next_metric += 60
        if any(child.poll() not in (None, 0) for child in children):
            raise RuntimeError("A recording client failed")
        if args.exercise_client_churn and elapsed >= next_churn:
            churn = subprocess.run(
                [
                    str(probe),
                    "--seconds",
                    "1",
                    "--expect-tone",
                    "--frequency-left",
                    "1100",
                    "--frequency-right",
                    "2200",
                ],
                capture_output=True,
                text=True,
                timeout=15,
            )
            summary["clientChurn"].append(
                {
                    "elapsedSeconds": round(elapsed, 3),
                    "returncode": churn.returncode,
                    "stdout": churn.stdout,
                    "stderr": churn.stderr,
                }
            )
            if churn.returncode or churn.stderr.strip():
                raise RuntimeError("An additional audio client failed during churn validation")
            next_churn += 10
        time.sleep(1)
    # Measure active counters before disabling the helper or stopping capture.
    # Later counter changes are kept separately and cannot explain an active miss.
    control("kill", "SIGUSR1", f"{domain}/{engine_label}", check=True)
    for attempt in range(40):
        checkpoints = [
            json.loads(line)
            for line in out.read_text().splitlines()
            if '"event":"checkpoint"' in line
        ]
        if checkpoints:
            summary["recordingCheckpoint"] = checkpoints[-1]
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("Helper did not acknowledge the recording counter checkpoint")
    if not summary.get("legacyDriver"):
        timing = subprocess.run(
            [str(probe), "--driver-timing"],
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
        summary["driverTimingAfter"] = json.loads(timing.stdout)
    summary["consumerResults"] = []
    for index, child in enumerate(children, 1):
        lines = (run / f"consumer-{index}.stdout.log").read_text().splitlines()
        summary["consumerResults"].append(
            {"returncode": child.returncode, "result": json.loads(lines[-1]) if lines else None}
        )
    summary["status"] = (
        "validating"
        if all(x["returncode"] == 0 and x["result"]["passed"] for x in summary["consumerResults"])
        else "failed"
    )
except KeyboardInterrupt:
    summary["status"] = "cancelled"
except Exception as error:
    summary["status"] = "failed"
    summary["error"] = str(error)
finally:
    if recording_started is not None:
        summary["elapsedSeconds"] = round(time.monotonic() - recording_started, 3)
    cleanup_errors = []
    for child in children:
        try:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
        except Exception as error:
            cleanup_errors.append(str(error))
    # Always stop both agents, even if a child cannot be reaped or a check fails.
    try:
        control("kill", "SIGTERM", f"{domain}/{engine_label}")
        for attempt in range(50):
            state = control("print", f"{domain}/{engine_label}").stdout
            if not re.search(r"^\s*pid = (\d+)", state, re.M):
                break
            time.sleep(0.1)
        match = re.search(r"^\s*last exit code = (\d+)", state, re.M)
        summary["engineExitCode"] = int(match[1]) if match else None
    except Exception as error:
        cleanup_errors.append(str(error))
    finally:
        cleanup_errors.extend(stop_helpers())
    for handle in handles:
        handle.close()
    out = run / "engine.stdout.log"
    if out.exists():
        summary["engineLog"] = out.read_text()
    error = run / "engine.stderr.log"
    if error.exists():
        summary["engineErrors"] = error.read_text()
    if summary["status"] == "validating":
        try:
            events = [json.loads(line) for line in summary["engineLog"].splitlines()]
            summary["captureConfiguration"] = next(
                event for event in events if event.get("event") == "capture-config"
            )
            baseline = next(event for event in events if event.get("event") == "warmup")
            stopped = next(event for event in events if event.get("event") == "stopped")
            final = summary["recordingCheckpoint"]
            counters = [
                "lateFrames",
                "missedPublishDeadlines",
                "appMissingFrames",
                "micMissingFrames",
                "driverMissingFrames",
                "monitorMissingFrames",
                "monitorTimestampErrors",
                "appDiscontinuities",
                "micDiscontinuities",
            ]
            summary["postWarmupErrors"] = {key: final[key] - baseline[key] for key in counters}
            summary["postCheckpointCounterIncrements"] = {
                key: stopped[key] - final[key] for key in counters
            }
            summary["sourceEvidence"] = {
                key: final[key] - baseline[key]
                for key in (
                    ["appCallbacks", "appNonzeroFrames"]
                    + ([] if args.without_monitor else ["monitorFrames"])
                    + ([] if args.without_microphone else ["micCallbacks", "micNonzeroFrames"])
                )
            }
            summary["memoryGrowthKiB"] = {}
            for name in ["engine", "source", "consumer1", "consumer2"]:
                values = [
                    metric[name]["rssKiB"]
                    for metric in summary["metrics"]
                    if name in metric and metric["elapsedSeconds"] >= 60
                ]
                if not values:
                    values = [
                        metric[name]["rssKiB"] for metric in summary["metrics"] if name in metric
                    ]
                summary["memoryGrowthKiB"][name] = max(values) - min(values) if values else None
            monitor_frames = final["monitorFrames"] - baseline["monitorFrames"]
            # Compare coverage using the observed hardware clock rate. A valid
            # independent clock need not run at exactly its nominal frequency.
            measured_monitor_rate = final["monitorNominalRate"] * (
                1 + final["monitorDriftPpm"] / 1e6
            )
            expected_monitor_frames = (
                final["elapsedSeconds"] - baseline["elapsedSeconds"]
            ) * measured_monitor_rate
            summary["monitorCoverageFrames"] = {
                "received": monitor_frames,
                "expected": expected_monitor_frames,
                "tolerance": 4096,
                "measuredRate": measured_monitor_rate,
            }
            passed = (
                (args.without_monitor or abs(monitor_frames - expected_monitor_frames) <= 4096)
                and summary["engineExitCode"] == 0
                and not summary.get("engineErrors", "").strip()
                and not final["failed"]
                and not stopped["failed"]
                and not any(summary["postWarmupErrors"].values())
                and (args.block_frames == 256 or final["appAvailableMarginMs"] > 0)
                and (
                    args.block_frames == 256
                    or summary["captureConfiguration"]["mode"] == "device-stream"
                )
                and (
                    not summary["renderLeadFrames"]
                    or summary.get("driverTimingConfigured", {}).get("queueFrames") != 0
                    or final["minimumPublishSlackMicroseconds"] > 0
                )
                and all(value > 0 for value in summary["sourceEvidence"].values())
                and all(
                    value is not None and value <= 16384
                    for value in summary["memoryGrowthKiB"].values()
                )
            )
            summary["status"] = "passed" if passed else "failed"
            summary["certifies"] = (
                "timestamped digital app route, channel identity, and bounded observed memory"
                + ("" if args.without_monitor else ", plus monitor coverage")
                + ("" if args.without_microphone else ", plus microphone history continuity")
            )
            summary["latencyScope"] = (
                "Mach host timestamps, 1 ms marker search resolution; excludes acoustic microphone and external client processing latency"
            )
        except Exception as error:
            summary["status"] = "failed"
            summary["error"] = f"Final validation: {error}"
    if cleanup_errors:
        summary["cleanupErrors"] = cleanup_errors
        summary["status"] = "failed"
    summary["sustainedGateMet"] = summary["status"] == "passed" and args.seconds >= 7200
    summary["completedUnixTime"] = time.time()
    save()
    (run / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(
        json.dumps(
            {
                "event": "complete",
                "status": summary["status"],
                "directory": str(run),
                "error": summary.get("error"),
            }
        ),
        flush=True,
    )
raise SystemExit(0 if summary["status"] == "passed" else 1)
