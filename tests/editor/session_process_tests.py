"""Real launchd restart of a signed, capture-free fixture in a private job."""

import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

runner, fixture = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="loopz-session-") as temporary:
    root = Path(temporary).resolve()
    app = root / "LoopZ Engine.app"
    binary = app / "Contents/MacOS/LoopZ Engine"
    binary.parent.mkdir(parents=True)
    shutil.copy2(fixture, binary)
    (app / "Contents/Info.plist").write_bytes(
        plistlib.dumps(
            {
                "CFBundleIdentifier": "com.lexasoft.loopz.session-fixture",
                "CFBundleExecutable": "LoopZ Engine",
                "CFBundlePackageType": "APPL",
                "CFBundleVersion": "1",
            }
        )
    )
    subprocess.run(
        ["codesign", "--force", "--sign", "-", str(app)], check=True, capture_output=True
    )
    signed = subprocess.run(
        ["codesign", "-d", "--verbose=4", str(app)], check=True, capture_output=True, text=True
    ).stderr
    digest = re.search(r"^CDHash=([0-9a-f]{40})$", signed, re.MULTILINE).group(1)
    requirement = f'cdhash H"{digest}"'
    parent = root / "State"
    parent.mkdir(mode=0o700)
    directory = parent / "Session"
    document = parent / "routes.json"
    marker = b'{"fixture":"do not modify"}'
    document.write_bytes(marker)
    label = "com.lexasoft.loopz.session-test." + uuid.uuid4().hex
    domain = f"gui/{os.getuid()}"
    service = label + ".control"
    arguments = [
        str(runner.resolve()),
        str(app),
        str(document),
        str(directory),
        label,
        service,
        requirement,
    ]

    def ensure(expected=True):
        deadline = time.monotonic() + 3
        while True:
            result = subprocess.run(arguments, capture_output=True, text=True, timeout=20)
            if not expected or result.returncode == 0 or time.monotonic() >= deadline:
                break
            time.sleep(0.1)
        if expected:
            state = subprocess.run(
                ["launchctl", "print", domain + "/" + label], capture_output=True, text=True
            )
            assert result.returncode == 0, (
                result.stdout,
                result.stderr,
                state.stdout,
                state.stderr,
            )
            assert json.loads(result.stdout)["managed"]
        else:
            assert result.returncode != 0, result.stdout
        return result

    def wait_for(predicate):
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.1)
        raise AssertionError("Session fixture did not reach its expected state")

    events = Path(str(document) + ".events")

    def pids():
        return [int(value) for value in events.read_text().splitlines()] if events.exists() else []

    try:
        ensure()
        wait_for(lambda: len(pids()) == 1)
        original = pids()[0]
        ensure()
        assert pids() == [original], "Opening another client restarted the Engine"
        job = directory / "engine.plist"
        saved = job.read_bytes()
        definition = plistlib.loads(saved)
        assert definition["KeepAlive"] is True and definition["RunAtLoad"] is True
        assert definition["MachServices"] == {service: True}
        assert not (root / "Library/LaunchAgents").exists()
        # The PID is owned by this exact fresh fixture and verified by the host
        # against its path and pinned signature immediately before signaling.
        ensure()
        state = subprocess.check_output(["launchctl", "print", domain + "/" + label], text=True)
        assert f"\tpid = {original}\n" in state and str(binary) in state
        os.kill(original, signal.SIGKILL)
        wait_for(lambda: len(pids()) == 2)
        assert pids()[1] != original
        ensure()
        assert document.read_bytes() == marker
        assert (job.stat().st_mode & 0o777) == 0o600
        definition["KeepAlive"] = False
        job.write_bytes(plistlib.dumps(definition))
        ensure(False)
        assert plistlib.loads(job.read_bytes())["KeepAlive"] is False
        assert len(pids()) == 2
        job.write_bytes(saved)
        ensure()
        print(
            "Signed session survives client exit, restarts after a crash, preserves the document and refuses changed job definitions"
        )
    finally:
        subprocess.run(
            ["launchctl", "bootout", domain + "/" + label], capture_output=True, timeout=15
        )
        wait_for(
            lambda: subprocess.run(
                ["launchctl", "print", domain + "/" + label], capture_output=True
            ).returncode
            != 0
        )
