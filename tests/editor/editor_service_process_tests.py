"""Signed editor/engine IPC in temporary user-domain jobs, without audio IO."""

import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


source = Path(sys.argv[1]).resolve()
label = f"com.lexasoft.loopz.editor-test.{uuid.uuid4().hex}"
domain = f"gui/{os.getuid()}"
target = f"{domain}/{label}"


def pid():
    result = subprocess.run(
        ["launchctl", "print", target], capture_output=True, text=True, timeout=10, check=True
    )
    match = re.search(r"^\s*pid = (\d+)\s*$", result.stdout, re.MULTILINE)
    return int(match.group(1)) if match else None


with tempfile.TemporaryDirectory(prefix="loopz-editor-xpc-") as temporary:
    folder = Path(temporary).resolve()
    executable = folder / source.name
    shutil.copy2(source, executable)
    document = folder / "routing.json"
    previous = folder / "previous.json"
    log = folder / "engine.log"
    job = folder / "engine.plist"
    settings = {
        "Label": label,
        "ProgramArguments": [str(executable), "--server-fixture", label, str(document)],
        "MachServices": {label: True},
        "KeepAlive": False,
        "RunAtLoad": False,
        "StandardOutPath": str(log),
        "StandardErrorPath": str(log),
    }
    environment = {
        name: os.environ[name]
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS")
        if name in os.environ
    }
    if environment:
        settings["EnvironmentVariables"] = environment
    job.write_bytes(plistlib.dumps(settings))
    job.chmod(0o600)
    registered = False
    try:
        subprocess.run(["launchctl", "bootstrap", domain, str(job)], check=True, timeout=15)
        registered = True

        def client(phase):
            result = subprocess.run(
                [
                    str(executable),
                    "--client-fixture",
                    label,
                    phase,
                    str(previous),
                    "--capture-free",
                ],
                capture_output=True,
                text=True,
                timeout=35,
            )
            assert result.returncode == 0, (result.stdout, result.stderr)
            report = json.loads(result.stdout.strip().splitlines()[-1])
            assert report["passed"] and report["devices"] == 3
            return report

        first = client("first")
        previous.write_text(json.dumps(first))
        first_pid = pid()
        assert first_pid and first["busChannels"] == 8
        # The first editor has exited. Its graph remains in the running engine.
        assert pid() == first_pid
        subprocess.run(["launchctl", "kill", "SIGTERM", target], check=True, timeout=10)
        deadline = time.monotonic() + 15
        while pid() is not None and time.monotonic() < deadline:
            time.sleep(0.05)
        assert pid() is None, "The isolated fixture engine did not stop"
        second = client("replacement")
        second_pid = pid()
        assert second_pid and first_pid != second_pid
        assert second["busChannels"] == 64
        assert first["revision"]["epoch"] != second["revision"]["epoch"]
        assert len({first_pid, second_pid, first["pid"], second["pid"]}) == 4
        saved = json.loads(document.read_text())
        assert saved["devices"][0]["name"] == "Persistent edit"
        assert saved["devices"][2]["routing"]["outputChannels"] == 64
        print(
            json.dumps(
                {
                    "passed": True,
                    "enginePIDs": [first_pid, second_pid],
                    "editorPIDs": [first["pid"], second["pid"]],
                    "devices": 3,
                    "busChannels": 64,
                    "cases": [
                        "signed kernel-pinned editor/engine XPC across processes",
                        "one saved multi-device edit and real renderer meter changes",
                        "editor exit preserves the background engine",
                        "engine replacement restores complete saved intent",
                        "old-process edit rejected despite generation reuse",
                    ],
                }
            )
        )
    except BaseException:
        state = subprocess.run(
            ["launchctl", "print", target], capture_output=True, text=True, timeout=10
        )
        print(state.stdout[-7000:], state.stderr, file=sys.stderr)
        if log.exists():
            print(log.read_text()[-6000:], file=sys.stderr)
        raise
    finally:
        if registered:
            cleanup = subprocess.run(
                ["launchctl", "bootout", target], capture_output=True, text=True, timeout=15
            )
            assert cleanup.returncode == 0, cleanup.stderr
