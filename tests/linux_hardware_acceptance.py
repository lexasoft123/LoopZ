#!/usr/bin/env python3
"""Opt-in silent physical transport acceptance; obtain an exclusive audio window first.

This script never stops another client, changes defaults/mixer settings, or claims
an audible-path/end-to-end latency result. It opens the selected physical devices
through an isolated Engine only after --exclusive-confirmed is supplied.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def integer(value, label):
    if isinstance(value, bool) or value is None:
        raise ValueError(f"Unavailable counter: {label}")
    text = str(value)
    if not text.isdecimal():
        raise ValueError(f"Invalid counter {label}: {value!r}")
    return int(text)


def item(entries, key, value):
    matches = [entry for entry in entries if entry.get(key) == value]
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one {key}={value!r}; found {len(matches)}")
    return matches[0]


def observation(envelope, capture_uid, output_uid):
    snapshot = envelope["snapshot"]
    runtime = snapshot["runtime"]
    capture = item(runtime["inputs"], "id", capture_uid)
    output = item(runtime["outputs"], "id", output_uid)
    bus = item(runtime["devices"], "id", "qualification")
    source = item(bus["sources"], "id", "capture")
    monitor = item(runtime["monitors"], "monitor", "silent-output")
    metadata = capture.get("capture")
    if not metadata or not capture.get("sourceSequenceAvailable"):
        raise ValueError("Capture continuity provenance is unavailable")
    if capture.get("sourceCounterHardware") is not False:
        raise ValueError("Capture counter origin must explicitly distinguish software frames")
    if capture.get("sourceCounterOrigin") != "received-pcm":
        raise ValueError("Expected received-PCM capture provenance")
    counters = {
        "capture.callbacks": capture.get("callbacks"),
        "capture.sequence": metadata.get("sequence"),
        "capture.receivedFrames": metadata.get("sourceFrame"),
        "capture.observedXruns": capture.get("observedXruns"),
        "capture.boundaries": capture.get("captureBoundaries"),
        "capture.historyDiscontinuities": capture.get("discontinuities"),
        "source.missingFrames": source.get("missingFrames"),
        "monitor.callbacks": monitor.get("callbacks"),
        "monitor.missingFrames": monitor.get("missingFrames"),
        "monitor.graphFailures": monitor.get("graphFailures"),
        "monitor.invalidBlocks": monitor.get("invalidBlocks"),
        "output.callbacks": output.get("callbacks"),
        "output.renderedFrames": output.get("renderedFrames"),
        "output.xruns": output.get("xruns"),
        "output.discontinuities": output.get("discontinuities"),
        "output.deadlineMisses": output.get("deadlineMisses"),
        "output.renderFailures": output.get("renderFailures"),
        "output.invalidCallbacks": output.get("invalidCallbacks"),
        "engine.renderedFrames": runtime.get("renderedFrames"),
        "engine.graphFailures": runtime.get("graphFailures"),
        "engine.missedDeadlines": runtime.get("missedDeadlines"),
        "engine.skippedFrames": runtime.get("renderSkippedFrames"),
        "engine.reanchors": runtime.get("renderReanchors"),
    }
    return {
        "epoch": snapshot["revision"]["epoch"],
        "desiredGeneration": snapshot["restoration"]["desiredGeneration"],
        "activeGeneration": snapshot["restoration"]["activeGeneration"],
        "restored": snapshot["restoration"]["phase"] == "restored",
        "captureConnected": capture["connected"],
        "outputConnected": output["connected"],
        "sourceOnline": source["online"],
        "monitorOnline": monitor["sourceOnline"],
        "captureGeneration": metadata["streamGeneration"],
        "captureClockDomain": metadata["clockDomain"],
        "outputGeneration": output["streamGeneration"],
        "counters": {key: integer(value, key) for key, value in counters.items()},
        "captureHistoryReads": capture.get("historyReads"),
        "captureHistoryClock": capture.get("historyClock"),
        "monitorHistoryBudgetFrames": monitor.get("historyBudgetFrames"),
        "monitorHistoryBudget": monitor.get("historyBudget"),
        "renderLateFrames": runtime.get("renderLateFrames"),
    }


def process_audio_observation(pid):
    """Read kernel reporting only; never open/configure an ALSA PCM stream."""
    result = {"observedUTC": datetime.now(timezone.utc).isoformat(), "pid": pid}
    try:
        # comm (field 2) may contain spaces and closing parentheses. Remaining
        # fields begin at state (3), so utime/stime are offsets 11 and 12.
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        result["cpu"] = {
            "available": True,
            "userTicks": int(fields[11]),
            "systemTicks": int(fields[12]),
            "startTimeTicks": int(fields[19]),
            "clockTicksPerSecond": os.sysconf("SC_CLK_TCK"),
            "observedMonotonicNs": time.monotonic_ns(),
        }
    except (OSError, ValueError, IndexError) as error:
        result["cpu"] = {"available": False, "error": str(error)}
    parameters = []
    for path in sorted(Path("/proc/asound").glob("card*/pcm*/sub*/hw_params")):
        entry = {"path": str(path)}
        try:
            entry["text"] = path.read_text()
        except OSError as error:
            entry["error"] = str(error)
        parameters.append(entry)
    result["alsaHwParams"] = parameters
    result["alsaHwParamsAvailable"] = bool(parameters)
    return result


def cpu_interval(before, after):
    result = {
        "scope": "Engine process only; informational, no cross-product comparison",
        "units": "percent of one CPU core; multiple Engine threads may exceed 100%",
        "available": False,
    }
    first, last = before["cpu"], after["cpu"]
    if not first["available"] or not last["available"]:
        result["error"] = "Process CPU counters were unavailable"
        return result
    if first["startTimeTicks"] != last["startTimeTicks"] or before["pid"] != after["pid"]:
        result["error"] = "Process identity changed during CPU observation"
        return result
    ticks = last["userTicks"] + last["systemTicks"] - first["userTicks"] - first["systemTicks"]
    duration = (last["observedMonotonicNs"] - first["observedMonotonicNs"]) / 1e9
    if ticks < 0 or duration <= 0 or first["clockTicksPerSecond"] != last["clockTicksPerSecond"]:
        result["error"] = "CPU counter or interval changed unexpectedly"
        return result
    result.update(
        {
            "available": True,
            "deltaTicks": ticks,
            "clockTicksPerSecond": first["clockTicksPerSecond"],
            "durationSeconds": duration,
            "perCorePercent": ticks / first["clockTicksPerSecond"] / duration * 100,
        }
    )
    return result


class Acceptance:
    def __init__(self, options):
        self.options = options
        options.artifacts.mkdir(parents=True, exist_ok=True)
        self.artifacts = Path(tempfile.mkdtemp(prefix="linux-hardware-", dir=options.artifacts))
        self.private = Path(tempfile.mkdtemp(prefix="lzhw-"))
        self.address = self.private / "run" / "control.sock"
        self.state = self.private / "state" / "engine.json"
        self.process = None
        self.log = None
        self.launches = 0
        self.reads = 0
        self.report = {
            "version": 1,
            "scope": "silent physical transport only",
            "startedUTC": datetime.now(timezone.utc).isoformat(),
            "system": platform.uname()._asdict(),
            "exclusiveWindowConfirmed": True,
            "claimsExcluded": ["audibility", "analog continuity", "end-to-end latency"],
            "sourceProvenance": "Associate frozen binary SHA256 values with their source build receipts; no revision is inferred from a version string.",
            "configuration": vars(options) | {"artifacts": str(options.artifacts)},
            "checks": [],
            "passed": False,
        }
        self.binaries = self.artifacts / "binaries"
        self.binaries.mkdir(mode=0o700)
        self.report["binaries"] = {}
        for name in ("engine", "cli"):
            original = Path(getattr(options, name)).resolve(strict=True)
            frozen = self.binaries / original.name
            if frozen.exists():
                raise ValueError("Engine and CLI must have distinct filenames")
            shutil.copy2(original, frozen)
            frozen.chmod(0o500)
            setattr(self, name, frozen)
            details = {"original": str(original), "executed": str(frozen), "sha256": sha256(frozen)}
            for program in ("file", "ldd"):
                if shutil.which(program):
                    result = subprocess.run(
                        [program, str(frozen)], capture_output=True, text=True, timeout=10
                    )
                    details[program] = {
                        "exitCode": result.returncode,
                        "stdout": result.stdout,
                        "stderr": result.stderr,
                    }
            self.report["binaries"][name] = details
        script = Path(__file__).resolve()
        frozen_script = self.artifacts / "harness.py"
        shutil.copy2(script, frozen_script)
        frozen_script.chmod(0o400)
        self.report["harness"] = {
            "source": str(script),
            "frozen": str(frozen_script),
            "sha256": sha256(frozen_script),
        }

    def check(self, name, condition, detail=None):
        self.report["checks"].append({"name": name, "passed": bool(condition), "detail": detail})

    def request(self, message):
        if self.process is None or self.process.poll() is not None:
            raise RuntimeError("The exact qualification Engine is no longer running")
        result = subprocess.run(
            [str(self.cli), "--socket", str(self.address), "--stdin"],
            input=json.dumps(message) + "\n",
            capture_output=True,
            text=True,
            timeout=8,
        )
        if result.returncode not in (0, 2):
            raise RuntimeError(f"CLI failed ({result.returncode}): {result.stderr}")
        return json.loads(result.stdout)

    def snapshot(self, label):
        result = self.request({"version": 1, "operation": "state"})
        self.reads += 1
        write_json(self.artifacts / f"{self.reads:04d}-{label}.json", result)
        return result

    def launch(self):
        self.launches += 1
        self.log = (self.artifacts / f"engine-{self.launches}.stderr.log").open("wb")
        self.process = subprocess.Popen(
            [str(self.engine), "--socket", str(self.address), "--state", str(self.state)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=self.log,
        )
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("Qualification Engine exited before its socket was ready")
            if self.address.exists():
                try:
                    self.request({"version": 1, "operation": "hello"})
                    executable = Path(f"/proc/{self.process.pid}/exe").resolve(strict=True)
                    if executable != self.engine.resolve():
                        raise RuntimeError(
                            "Launched Engine executable identity differs from the frozen binary"
                        )
                    self.report.setdefault("engineExecutions", []).append(
                        {
                            "pid": self.process.pid,
                            "executable": str(executable),
                            "sha256": sha256(executable),
                        }
                    )
                    return
                except (OSError, RuntimeError, json.JSONDecodeError):
                    pass
            time.sleep(0.1)
        raise TimeoutError("Qualification Engine socket did not become ready")

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                self.check(
                    "Engine stopped gracefully", False, "SIGKILL required after SIGTERM timeout"
                )
        self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None

    def edit(self, envelope, commands=None, history=None, initialize=False):
        request = {"version": 1, "expected": envelope["snapshot"]["revision"]}
        request.update({"history": history} if history else {"commands": commands})
        result = self.request(
            {"version": 1, "operation": "initialize" if initialize else "edit", "request": request}
        )
        write_json(self.artifacts / f"edit-{self.reads:04d}.json", result)
        if not result.get("ok"):
            raise RuntimeError(f"Routing edit failed: {result}")
        return result

    def restored(self, label, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            envelope = self.snapshot(label)
            state = envelope["snapshot"]["restoration"]
            if (
                state["phase"] == "restored"
                and state["desiredGeneration"] == state["activeGeneration"]
            ):
                return envelope
            time.sleep(0.5)
        raise TimeoutError(f"Full desired routing did not restore: {label}")

    def run(self):
        self.launch()
        initial = self.snapshot("inventory")
        devices = initial["inventory"]["devices"]
        capture = item(devices, "uid", self.options.capture)
        output = item(devices, "uid", self.options.output)
        if capture["inputChannels"] < self.options.capture_channels or output["outputChannels"] < 2:
            raise ValueError("Selected inventory does not advertise the requested channel layout")
        if not initial["inventory"].get("devicesFresh"):
            raise ValueError("Native inventory is unavailable or stale")
        first = 8 if self.options.capture_channels >= 10 else 0
        wires = [{"sourceChannel": first + c, "outputChannel": c, "gain": 1} for c in range(2)]
        self.edit(
            initial,
            [
                {
                    "type": "createDevice",
                    "id": "qualification",
                    "name": "Silent physical qualification",
                    "channels": 2,
                    "sampleRate": 48000,
                    "passThrough": False,
                },
                {
                    "type": "addSource",
                    "device": "qualification",
                    "id": "capture",
                    "kind": "input-device",
                    "selector": self.options.capture,
                    "channels": self.options.capture_channels,
                    "muteOriginal": False,
                    "connectByDefault": False,
                },
                {
                    "type": "setSourceWiring",
                    "device": "qualification",
                    "source": "capture",
                    "connections": wires,
                },
                {
                    "type": "addMonitor",
                    "device": "qualification",
                    "id": "silent-output",
                    "selector": self.options.output,
                    "channels": 2,
                    "connectByDefault": True,
                },
                {
                    "type": "setMonitorControls",
                    "device": "qualification",
                    "monitor": "silent-output",
                    "controls": {"gain": 0},
                },
            ],
            initialize=True,
        )
        self.restored("initial-restore")
        warmup = time.monotonic() + 5
        while time.monotonic() < warmup:
            self.snapshot("warmup")
            time.sleep(0.5)
        baseline_envelope = self.snapshot("baseline")
        baseline = observation(baseline_envelope, self.options.capture, self.options.output)
        baseline_process = process_audio_observation(self.process.pid)
        write_json(self.artifacts / "baseline-kernel.json", baseline_process)
        started = time.monotonic()
        samples = [baseline]
        # Every CLI call exits. This gap also lets temporary telemetry demand
        # expire; the saved monitor must keep the Engine and hardware alive.
        time.sleep(min(2, self.options.duration))
        while time.monotonic() - started < self.options.duration:
            samples.append(
                observation(self.snapshot("measurement"), self.options.capture, self.options.output)
            )
            time.sleep(min(1, max(0, self.options.duration - (time.monotonic() - started))))
        post_envelope = self.snapshot("post")
        post = observation(post_envelope, self.options.capture, self.options.output)
        post_process = process_audio_observation(self.process.pid)
        write_json(self.artifacts / "post-kernel.json", post_process)
        samples.append(post)
        deltas = {
            key: post["counters"][key] - baseline["counters"][key] for key in baseline["counters"]
        }
        self.report["measurement"] = {
            "durationSeconds": time.monotonic() - started,
            "baselineObservedUTC": baseline_process["observedUTC"],
            "postObservedUTC": post_process["observedUTC"],
            "engineCpu": cpu_interval(baseline_process, post_process),
            "baselineKernel": baseline_process,
            "postKernel": post_process,
            "declaredOutputGain": 0,
            "baseline": baseline,
            "post": post,
            "deltas": deltas,
        }
        stable = (
            "epoch",
            "desiredGeneration",
            "activeGeneration",
            "captureGeneration",
            "captureClockDomain",
            "outputGeneration",
        )
        for key in stable:
            self.check(f"Stable {key}", all(s[key] == baseline[key] for s in samples))
        for key in (
            "captureConnected",
            "outputConnected",
            "sourceOnline",
            "monitorOnline",
            "restored",
        ):
            self.check(key, all(s[key] for s in samples))
        self.check(
            "Active routing equals desired",
            all(s["desiredGeneration"] == s["activeGeneration"] for s in samples),
        )
        advancing = {
            "capture.callbacks",
            "capture.sequence",
            "capture.receivedFrames",
            "monitor.callbacks",
            "output.callbacks",
            "output.renderedFrames",
            "engine.renderedFrames",
        }
        for key, delta in deltas.items():
            self.check(key, delta > 0 if key in advancing else delta == 0, {"warmDelta": delta})
            self.check(
                f"Monotonic {key}",
                all(
                    second["counters"][key] >= first["counters"][key]
                    for first, second in zip(samples, samples[1:])
                ),
            )
        for key in ("captureGeneration", "captureClockDomain", "outputGeneration"):
            self.check(f"Available {key}", integer(baseline[key], key) > 0)
        self.check("Engine survives CLI exit", self.process.poll() is None)
        # The replacement cannot open. Desired state must remain pending while
        # the exact prior output continues advancing with unchanged generation.
        self.edit(
            post_envelope,
            [
                {
                    "type": "setMonitorEndpoint",
                    "device": "qualification",
                    "monitor": "silent-output",
                    "selector": "loopz-qualification-deliberately-missing",
                    "channels": 2,
                }
            ],
        )
        pending = self.snapshot("failed-replacement")
        restoration = pending["snapshot"]["restoration"]
        self.check(
            "Failed replacement remains pending",
            restoration["desiredGeneration"] != restoration["activeGeneration"],
        )
        old_before = item(pending["snapshot"]["runtime"]["outputs"], "id", self.options.output)
        time.sleep(2)
        pending = self.snapshot("retained-output")
        old_after = item(pending["snapshot"]["runtime"]["outputs"], "id", self.options.output)
        self.check(
            "Failed replacement preserves output callbacks",
            integer(old_after.get("callbacks"), "retained output callbacks")
            > integer(old_before.get("callbacks"), "retained output callbacks"),
        )
        self.check(
            "Failed replacement preserves output generation",
            old_after["streamGeneration"] == old_before["streamGeneration"],
        )
        self.edit(pending, history="undo")
        saved = self.restored("undo-restore")
        saved_document = saved["snapshot"]["document"]
        saved_revision = saved["snapshot"]["revision"]
        self.stop()
        self.launch()
        restarted = self.restored("restart-restore")
        self.check(
            "Engine restart restores full saved routing",
            restarted["snapshot"]["document"] == saved_document,
        )
        self.check(
            "Engine restart preserves desired generation",
            restarted["snapshot"]["revision"]["generation"] == saved_revision["generation"],
        )
        self.check(
            "Engine restart changes epoch",
            restarted["snapshot"]["revision"]["epoch"] != saved_revision["epoch"],
        )
        time.sleep(5)
        restart = observation(
            self.snapshot("restart-warm"), self.options.capture, self.options.output
        )
        self.check(
            "Restart stays fully restored after warmup",
            restart["restored"]
            and restart["activeGeneration"] == restart["desiredGeneration"]
            and restart["captureConnected"]
            and restart["outputConnected"]
            and restart["sourceOnline"]
            and restart["monitorOnline"],
        )
        self.check(
            "Restart capture and output callbacks advance",
            restart["counters"]["capture.callbacks"] > 0
            and restart["counters"]["output.callbacks"] > 0,
        )

    def finish(self):
        self.stop()
        self.check(
            "Frozen binary identity remains unchanged",
            all(
                sha256(Path(details["executed"])) == details["sha256"]
                for details in self.report["binaries"].values()
            ),
        )
        if self.state.exists() and self.state.is_file():
            shutil.copy2(self.state, self.artifacts / "saved-engine.json")
        self.report["finishedUTC"] = datetime.now(timezone.utc).isoformat()
        self.report["passed"] = not self.report.get("error") and all(
            c["passed"] for c in self.report["checks"]
        )
        write_json(self.artifacts / "report.json", self.report)
        shutil.rmtree(self.private)
        print(
            json.dumps(
                {"passed": self.report["passed"], "report": str(self.artifacts / "report.json")}
            )
        )
        return 0 if self.report["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--capture", required=True, help="Opaque native capture UID")
    parser.add_argument("--output", required=True, help="Opaque native output UID")
    parser.add_argument("--capture-channels", type=int, default=10)
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--exclusive-confirmed", action="store_true", required=True)
    options = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("Physical qualification requires Linux")
    if not 2 <= options.capture_channels <= 64 or not 1 <= options.duration <= 86400:
        parser.error("Use 2-64 capture channels and a duration from 1 to 86400 seconds")
    run = Acceptance(options)

    def interrupted(signum, _frame):
        raise InterruptedError(f"Interrupted by signal {signum}")

    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        run.run()
    except BaseException as error:
        run.report["error"] = f"{type(error).__name__}: {error}"
    finally:
        # A second ordinary termination signal must not interrupt draining the
        # exact child and strand its physical streams during the handback.
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        try:
            result = run.finish()
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
    return result


if __name__ == "__main__":
    sys.exit(main())
