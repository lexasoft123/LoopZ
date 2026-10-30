"""Exercise production engine lifecycle with a private fault-injecting bridge."""

import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time


engine = Path(sys.argv[1])


def read_until_event(process, event_name, timeout, count=1):
    """Collect stdout through a JSON event without blocking on a slow start."""
    descriptor = process.stdout.fileno()
    output = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        ready, _, _ = select.select([descriptor], [], [], remaining)
        if not ready:
            break
        chunk = os.read(descriptor, 4096)
        if not chunk:
            break
        output.extend(chunk)
        matching = 0
        for line in output.splitlines():
            if line.startswith(b"{"):
                try:
                    payload = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if payload.get("event") == event_name:
                    matching += 1
                    if matching >= count:
                        return output.decode()
    raise AssertionError(
        f"timed out waiting for {event_name!r}; stdout={output.decode(errors='replace')}"
    )


def run(extra, pause=False):
    process = subprocess.Popen(
        [str(engine), "--tone", "--no-pass-through", "--seconds", "6", *extra],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        stdout_prefix = ""
        if pause:
            # An OS scheduling/suspend gap must rebuild the render clock while
            # retaining the bridge and desired endpoints.
            # Wait for both bridge faults to recover into a running session,
            # rather than assuming startup finishes within a fixed delay.
            stdout_prefix = read_until_event(process, "started", timeout=15, count=3)
            os.kill(process.pid, signal.SIGSTOP)
            try:
                time.sleep(0.08)
            finally:
                os.kill(process.pid, signal.SIGCONT)
        stdout, stderr = process.communicate(timeout=20)
        stdout = stdout_prefix + stdout
    except BaseException:
        process.kill()
        process.communicate()
        raise
    events = [json.loads(line) for line in stdout.splitlines() if line.startswith("{")]
    started = [event for event in events if event.get("event") == "started"]
    stopped = [event for event in events if event.get("event") == "stopped"]
    attached = [event for event in events if event.get("event") == "test-bridge-connect"]
    assert len(started) >= 3, (stdout, stderr)
    assert started[0]["rate"] == 48000 and started[-1]["rate"] == 44100
    assert [event["attempt"] for event in attached] == [1, 2, 3, 4], (stdout, stderr)
    assert len(stopped) == len(started), (stdout, stderr)
    assert stopped[-1]["renderedFrames"] > 0 and not stopped[-1]["failed"]
    if extra:
        # Absent sources/monitor remain requested; a timed validation reports
        # incomplete restoration rather than accepting a silently reduced graph.
        assert process.returncode == 1, (stdout, stderr)
        for name in ("application", "microphone", "monitor"):
            assert stderr.count(f"{name}: waiting to reconnect") >= 2, stderr
    else:
        assert process.returncode == 0, (stdout, stderr)
    if pause:
        assert len(started) >= 4 and "Realtime render failed (4)" in stderr, (stdout, stderr)
    print(
        json.dumps(
            {
                "case": "absent-endpoints" if extra else "clock-gap" if pause else "service-loss",
                "sessions": len(started),
                "bridgeConnections": len(attached),
                "finalFrames": stopped[-1]["renderedFrames"],
                "passed": True,
            }
        )
    )


run([])
run([], pause=True)
run(
    [
        "--app",
        "com.lexasoft.loopz.recovery-test.absent",
        "--mic",
        "com.lexasoft.loopz.recovery-test.absent",
        "--monitor-muted",
        "com.lexasoft.loopz.recovery-test.absent",
    ]
)
