"""Bounded stdio controller for the opt-in continuous-client audio diagnostic."""

import json
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import selectors
import subprocess
import time


class StableReadiness:
    """Require two continuous healthy seconds with an unchanged timing signature."""

    def __init__(self):
        self.previous = None
        self.since = None

    def observe(self, healthy, signature, now):
        current = tuple(signature) if healthy else None
        if current is None or current != self.previous:
            self.since = now if current is not None else None
        self.previous = current
        return self.since is not None and now - self.since >= 2


def verify_recording_graph_snapshots(baseline, observations, after_stop, expected_devices):
    """Keep live timing fixed; stopping a reader may renegotiate public IO timing."""

    def unchanged_document(snapshot):
        if any(snapshot[key] != baseline[key] for key in ("revision", "document")):
            raise RuntimeError("The recording graph document changed")

    if not observations:
        raise RuntimeError("The recording has no live graph observations")
    expected = {device["id"]: device for device in baseline["runtime"]["devices"]}
    if len(expected) != expected_devices or len(baseline["runtime"]["devices"]) != expected_devices:
        raise RuntimeError("The recording graph has the wrong device count")
    for snapshot in [baseline, *observations]:
        unchanged_document(snapshot)
        runtime = snapshot["runtime"]
        devices = {device["id"]: device for device in runtime["devices"]}
        if (
            not runtime["restored"]
            or len(runtime["devices"]) != expected_devices
            or devices.keys() != expected.keys()
        ):
            raise RuntimeError("The live recording graph is not fully restored")
        for identity, device in devices.items():
            previous = expected[identity]
            if (
                device["transport"]["generation"] != previous["transport"]["generation"]
                or device["latencyFrames"] != previous["latencyFrames"]
            ):
                raise RuntimeError("The live recording graph timing changed")
            if int(device["graphFailures"]) or int(device["sinkFailures"]):
                raise RuntimeError("The recording graph reported an audio failure")
    # Input.stop() precedes the captured acknowledgement. Its timing changes
    # belong to shutdown, while the native result still checks every sample
    # through the callback gate's actual end of recording.
    unchanged_document(after_stop)
    if (
        after_stop["runtime"]["renderError"]
        or after_stop["runtime"]["controlError"]
        or any(
            int(device[key])
            for device in after_stop["runtime"]["devices"]
            for key in ("graphFailures", "sinkFailures")
        )
    ):
        raise RuntimeError("The graph reported an error after recording stopped")


class ContinuousProbe:
    def __init__(self, arguments, record):
        self.record = Path(record)
        self.events = []
        self.transcript = bytearray()
        self.pending = bytearray()
        self.deadline = time.monotonic() + 45
        self.stderr = self.record.with_suffix(".stderr.log").open("wb")
        self.process = None
        self.selector = selectors.DefaultSelector()
        try:
            self.process = subprocess.Popen(
                arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr
            )
            self.selector.register(self.process.stdout, selectors.EVENT_READ)
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def command(self, value):
        if value not in ("begin-baseline", "begin-disconnect", "restored"):
            raise ValueError("Unknown recovery control command")
        if self.process.poll() is not None:
            raise RuntimeError("The existing-client diagnostic ended before recovery")
        self.process.stdin.write((value + "\n").encode())
        self.process.stdin.flush()

    def event(self, expected, timeout=10):
        deadline = min(self.deadline, time.monotonic() + timeout)
        while time.monotonic() < deadline:
            if b"\n" in self.pending:
                line, _, remainder = self.pending.partition(b"\n")
                self.pending = bytearray(remainder)
                try:
                    value = json.loads(line)
                except (UnicodeDecodeError, ValueError) as error:
                    raise RuntimeError("Invalid recovery diagnostic response") from error
                if not isinstance(value, dict) or value.get("event") != expected:
                    raise RuntimeError("Unexpected recovery diagnostic event: " + str(value))
                self.events.append(value)
                return value
            ready = self.selector.select(max(0, min(0.2, deadline - time.monotonic())))
            if not ready:
                continue
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                raise RuntimeError("The existing-client diagnostic closed before " + expected)
            self.transcript.extend(chunk)
            self.pending.extend(chunk)
            if len(self.transcript) > 65536:
                raise RuntimeError("Recovery diagnostic output exceeded its bound")
        raise RuntimeError("Timed out waiting for recovery diagnostic event: " + expected)

    def result(self):
        result = self.event("result", timeout=35)
        code = self.process.wait(timeout=3)
        if code or result.get("passed") is not True or result.get("clientsReopened") is not False:
            raise RuntimeError("Existing audio clients did not survive recovery")
        return result

    def close(self):
        try:
            if self.process is not None:
                if self.process.poll() is None:
                    self.process.terminate()
                    try:
                        self.process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait(timeout=3)
                for stream in (self.process.stdin, self.process.stdout):
                    if stream is not None:
                        stream.close()
        finally:
            self.selector.close()
            self.stderr.close()
            self.record.write_text(
                json.dumps(
                    {
                        "pid": self.process.pid if self.process is not None else None,
                        "exitCode": self.process.returncode if self.process is not None else None,
                        "events": self.events,
                        "stdout": self.transcript.decode(errors="replace"),
                        "stderrFile": str(self.record.with_suffix(".stderr.log")),
                        "stopped": self.process is None or self.process.returncode is not None,
                    },
                    indent=2,
                )
                + "\n"
            )


def continuous_results(probes):
    """Observe every owned client before reporting an aggregate failure."""
    if not probes:
        raise ValueError("Continuous recovery requires at least one client")
    measurements, failures = [], []
    with ThreadPoolExecutor(max_workers=len(probes)) as pool:
        pending = [pool.submit(probe.result) for probe in probes]
        for index, completed in enumerate(pending):
            try:
                measurements.append(completed.result())
            except Exception as error:
                failures.append({"client": index, "error": str(error)})
    return {"passed": not failures, "measurements": measurements, "failures": failures}
