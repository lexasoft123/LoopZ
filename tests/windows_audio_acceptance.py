"""Opt-in hardware check: default mic -> shared graph -> muted monitor.

Run in an interactive Windows desktop session. Requires an empty preview
document, and restores that empty document afterwards. No audio is saved.
This is intentionally excluded from hosted CI.
"""

import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

directory = Path(sys.argv[1]).resolve()
control = directory / "loopz-windows-control.exe"
subprocess.Popen([str(directory / "LoopZ.exe"), "--engine"])


def state():
    result = subprocess.run([str(control), "--state"], capture_output=True, timeout=30, check=True)
    return json.loads(result.stdout.decode("utf-8"))


with tempfile.TemporaryDirectory(prefix="loopz-audio-") as temporary:
    request_path = Path(temporary) / "request.json"

    def edit(commands):
        current = state()
        request_path.write_text(
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
        result = subprocess.run(
            [str(control), "--request", str(request_path)],
            capture_output=True,
            timeout=30,
            check=True,
        )
        assert json.loads(result.stdout.decode("utf-8"))["ok"] is True

    for attempt in range(20):
        try:
            before = state()
            break
        except (subprocess.CalledProcessError, json.JSONDecodeError):
            if attempt == 19:
                raise
            time.sleep(0.5)
    if before["snapshot"]["document"]["devices"]:
        raise RuntimeError("Use an empty preview document for this hardware test")
    output = next(d for d in before["inventory"]["devices"] if d["defaultOutput"])
    source = next(d for d in before["inventory"]["devices"] if d["defaultInput"])
    mix = "acceptance-" + str(uuid.uuid4())
    try:
        edit(
            [
                {
                    "type": "createDevice",
                    "id": mix,
                    "name": "Audio acceptance",
                    "passThrough": False,
                },
                {
                    "type": "addSource",
                    "device": mix,
                    "id": "mic",
                    "kind": "input-device",
                    "selector": "default",
                    "channels": min(2, source["inputChannels"]),
                },
                {
                    "type": "addMonitor",
                    "device": mix,
                    "id": "monitor",
                    "selector": output["uid"],
                    "channels": 2,
                },
                {
                    "type": "setMonitorControls",
                    "device": mix,
                    "monitor": "monitor",
                    "controls": {"muted": True},
                },
            ]
        )
        time.sleep(3)
        first = state()["snapshot"]["runtime"]
        time.sleep(2)
        after = state()["snapshot"]["runtime"]
        first_device, device = first["devices"][0], after["devices"][0]
        monitor = after["monitors"][0]
        captured = device["sources"][0]
        missing = captured["missingFrames"] - first_device["sources"][0]["missingFrames"]
        assert device["renderedFrames"] > first_device["renderedFrames"]
        assert device["graphFailures"] == 0
        assert captured["online"] and captured["meter"]["valid"]
        assert monitor["connected"] and monitor["callbacks"] > 0
        assert monitor["renderFailures"] == 0
        monitor_missing = monitor["missingFrames"] - first["monitors"][0]["missingFrames"]
        if monitor_missing or missing:
            print(
                json.dumps(
                    {
                        "monitorDiagnostic": {"device": device, "monitor": monitor},
                        "captureDiagnostic": after.get("captureDiagnostics"),
                        "sourceMissingDuringCheck": missing,
                    }
                ),
                flush=True,
            )
        assert monitor_missing == 0, f"Missing monitor history frames: {monitor_missing}"
        if missing:
            print(
                json.dumps(
                    {"captureDiagnostic": after.get("captureDiagnostics"), "device": device}
                ),
                flush=True,
            )
        assert missing == 0, f"Missing source frames during steady playback: {missing}"
        with ThreadPoolExecutor(max_workers=4) as clients:
            replies = list(clients.map(lambda _: state(), range(12)))
        assert all(reply["connection"]["ready"] for reply in replies)
        print(
            json.dumps(
                {
                    "ok": True,
                    "renderedFrames": device["renderedFrames"],
                    "sourcePeak": captured["meter"]["peak"],
                    "missingFramesDuringCheck": missing,
                    "monitorCallbacks": monitor["callbacks"],
                    "monitorRenderFailures": monitor["renderFailures"],
                    "missingMonitorFramesDuringCheck": monitor_missing,
                    "concurrentStateReads": len(replies),
                }
            )
        )
    finally:
        if any(d["id"] == mix for d in state()["snapshot"]["document"]["devices"]):
            edit([{"type": "deleteDevice", "device": mix}])
