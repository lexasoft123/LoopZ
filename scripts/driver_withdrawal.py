"""Withdraw published devices without editing the user's saved routing intent."""

import json
from pathlib import Path
import tempfile
import time


def withdraw_devices(session):
    if not session.paused or session.job_state() is not None:
        raise RuntimeError("Stop the canonical Engine before withdrawing devices")
    session.unchanged_files()
    catalog = session.host.snapshot()
    for key in ("routingBlockingRoutes", "loopzBlockingRoutes"):
        if key not in catalog or catalog[key]:
            raise RuntimeError("A client is still using LoopZ; devices were not withdrawn")
    for key in ("defaultInput", "defaultOutput", "defaultSystemOutput"):
        uid = catalog.get(key)
        if not isinstance(uid, str) or uid.startswith("com.lexasoft.loopz."):
            raise RuntimeError("LoopZ must not be a default device during replacement")
    if not catalog.get("routingDevices"):
        return
    # The broker retains publication after its owner exits. A capture-free,
    # temporary Engine publishes an empty catalog; the canonical file is untouched.
    with tempfile.TemporaryDirectory(prefix="loopz-withdraw-") as temporary:
        document = Path(temporary) / "routes.json"
        document.write_text('{"version":2,"devices":[]}\n')
        document.chmod(0o600)
        result = session.host.user_command(
            "/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
            "--routing-document", document,
            "--installation-validation", "true",
            "--seconds", "2",
            timeout=10,
        )
        states = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        if not any(s.get("phase") == "configured" and s.get("devices") == [] for s in states):
            raise RuntimeError("The temporary Engine did not acknowledge withdrawal")
    until = time.monotonic() + 8
    while time.monotonic() < until:
        session.unchanged_files()
        if session.host.snapshot().get("routingDevices") == []:
            return
        time.sleep(0.1)
    raise RuntimeError("LoopZ devices remained published after withdrawal")
