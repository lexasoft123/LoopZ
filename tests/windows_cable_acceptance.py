"""Opt-in: selected marker app -> LoopZ mix -> Feed -> WASAPI recording.

Requires the test driver and an interactive desktop. Adds two temporary mixes
alongside existing saved mixes, then removes their document entries and adapters.
Existing saved mixes and microphone IDs must remain unchanged. No audio saved.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

folder = Path(sys.argv[1]).resolve()
control = folder / "loopz-windows-control.exe"
subprocess.Popen([str(folder / "LoopZ.exe"), "--engine"])


def state():
    result = subprocess.run([str(control), "--state"], capture_output=True, check=True, timeout=30)
    return json.loads(result.stdout.decode("utf-8"))


with tempfile.TemporaryDirectory(prefix="loopz-cable-") as temporary:
    request = Path(temporary) / "request.json"

    def edit(commands, expected=True):
        before = state()
        request.write_text(
            json.dumps(
                {
                    "version": 1,
                    "operation": "edit",
                    "request": {
                        "version": 1,
                        "expected": before["snapshot"]["revision"],
                        "commands": commands,
                    },
                }
            ),
            encoding="utf-8",
        )
        reply = subprocess.run(
            [str(control), "--request", str(request)],
            capture_output=True,
            check=False,
            timeout=30,
        )
        result = json.loads(reply.stdout.decode("utf-8"))
        assert result["ok"] == expected, result
        return result

    before = state()
    original_document = before["snapshot"]["document"]
    original_microphones = {
        d["id"]: d["microphone"]["uid"]
        for d in before["snapshot"]["runtime"]["devices"]
        if d.get("microphone", {}).get("uid")
    }
    mix = "cable-acceptance-" + str(uuid.uuid4())
    quiet_mix = "cable-silence-" + str(uuid.uuid4())
    marker = None
    owned_keys = set()
    report_output = None
    try:
        edit(
            [
                {"type": "createDevice", "id": mix, "name": "Cable acceptance", "passThrough": False},
                {
                    "type": "createDevice",
                    "id": quiet_mix,
                    "name": "Cable isolation",
                    "passThrough": False,
                },
            ]
        )
        owner = subprocess.check_output(
            [
                "powershell.exe",
                "-NoProfile",
                "-Command",
                "[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value",
            ],
            text=True,
            timeout=10,
        ).strip()
        owned_keys = {
            hashlib.sha256((owner + "\0" + identifier).encode("utf-8")).hexdigest()
            for identifier in (mix, quiet_mix)
        }
        installed = subprocess.run(
            [
                str(folder / "loopz-driver-setup.exe"),
                "--sync",
                str(Path(os.environ["LOCALAPPDATA"]) / "LoopZ" / "routing.json"),
                owner,
            ],
            capture_output=True,
            timeout=60,
        )
        if installed.returncode:
            raise RuntimeError((installed.stdout + installed.stderr).decode("utf-8", errors="replace"))
        for attempt in range(30):
            before = state()
            published = {d["id"]: d["microphone"] for d in before["snapshot"]["runtime"]["devices"]}
            if all(published.get(i, {}).get("installed") for i in (mix, quiet_mix)):
                break
            time.sleep(0.5)
        assert all(published.get(i, {}).get("installed") for i in (mix, quiet_mix)), published
        assert all(published.get(i, {}).get("uid") == uid
                   for i, uid in original_microphones.items()), "Existing microphone ID changed"
        microphone_uid = published[mix]["uid"]
        key, quiet_key = published[mix]["mixKey"], published[quiet_mix]["mixKey"]
        marker = subprocess.Popen(
            [str(folder / "loopz-windows-probe.exe"), "--tone", "30"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        for attempt in range(12):
            applications = state()["inventory"]["applications"]
            selected = next(
                (a for a in applications if "loopz-windows-probe.exe" in a["bundle"].lower()), None
            )
            if selected:
                break
            time.sleep(0.5)
        assert selected, "Marker app audio session not discovered"
        edit(
            [
                {
                    "type": "addSource",
                    "device": mix,
                    "id": "marker",
                    "kind": "application",
                    "muteOriginal": False,
                    "selector": selected["bundle"],
                    "channels": 2,
                },
            ]
        )
        physical = next(
            d
            for d in state()["inventory"]["devices"]
            if d["outputChannels"] >= 2 and not d.get("virtualRole") and d["defaultOutput"]
        )
        edit(
            [
                {
                    "type": "addMonitor",
                    "device": mix,
                    "id": "headphones",
                    "selector": physical["uid"],
                    "channels": 2,
                },
                {
                    "type": "setMonitorControls",
                    "device": mix,
                    "monitor": "headphones",
                    "controls": {"muted": True},
                },
            ]
        )
        refused = edit(
            [
                {
                    "type": "addSource",
                    "device": mix,
                    "id": "feedback",
                    "kind": "input-device",
                    "selector": microphone_uid,
                    "channels": 2,
                }
            ],
            expected=False,
        )
        assert "feedback" in refused.get("detail", "").lower(), refused
        time.sleep(2)
        first = state()["snapshot"]["runtime"]
        recorded = subprocess.run(
            [str(folder / "loopz-windows-cable-probe.exe"), "--record-only", key],
            capture_output=True,
            timeout=30,
        )
        if recorded.returncode:
            raise RuntimeError(
                (recorded.stdout + recorded.stderr).decode("utf-8", errors="replace")
                + json.dumps(state())
            )
        report = json.loads(recorded.stdout.decode("utf-8"))
        after = state()["snapshot"]["runtime"]
        device = next(d for d in after["devices"] if d["id"] == mix)
        monitor = next(m for m in after["monitors"] if m["device"] == mix and m["publication"])
        assert report["ok"] and monitor["connected"]
        assert device["graphFailures"] == 0 and monitor["renderFailures"] == 0
        missing = (
            device["sources"][0]["missingFrames"]
            - next(d for d in first["devices"] if d["id"] == mix)["sources"][0]["missingFrames"]
        )
        first_monitor = next(
            m for m in first["monitors"] if m["device"] == mix and m["publication"]
        )
        monitor_missing = monitor["missingFrames"] - first_monitor["missingFrames"]
        headphones = next(m for m in after["monitors"]
                          if m["device"] == mix and m["monitor"] == "headphones")
        assert headphones["connected"] and headphones["callbacks"] > 0
        assert missing == 0 and monitor_missing == 0, (missing, monitor_missing)
        before_edit_callbacks = monitor["callbacks"]
        edit([{"type": "renameDevice", "device": quiet_mix, "name": "Cable isolation renamed"}])
        time.sleep(0.5)
        after_edit = state()["snapshot"]["runtime"]
        preserved = next(
            m for m in after_edit["monitors"] if m["device"] == mix and m["publication"]
        )
        assert (
            preserved["callbacks"] > before_edit_callbacks
        ), "Unrelated edit restarted the live microphone stream"
        assert (
            next(d for d in after_edit["devices"] if d["id"] == mix)["microphone"]["uid"]
            == microphone_uid
        )
        isolated = subprocess.run(
            [str(folder / "loopz-windows-cable-probe.exe"), "--silence-only", quiet_key],
            capture_output=True,
            check=True,
            timeout=30,
        )
        assert json.loads(isolated.stdout.decode("utf-8"))["ok"]
        report_output = {
            "ok": True,
            "recording": report,
            "feedbackRejected": True,
            "independentMicrophones": True,
            "unrelatedEditPreservesOutputStream": True,
            "stableEndpointIdentity": True,
            "otherMixRemainsSilent": True,
            "simultaneousHardwareMonitor": True,
            "mutedHardwareDoesNotMuteMicrophone": True,
            "sourceMissing": missing,
            "monitorMissing": monitor_missing,
        }
    finally:
        cleanup_errors = []
        for identifier in (mix, quiet_mix):
            try:
                if any(d["id"] == identifier
                       for d in state()["snapshot"]["document"]["devices"]):
                    edit([{"type": "deleteDevice", "device": identifier}])
            except Exception as error:
                cleanup_errors.append(str(error))
        if marker is not None:
            try:
                if marker.poll() is None:
                    marker.terminate()
                marker.wait(timeout=10)
            except Exception as error:
                cleanup_errors.append(str(error))
        for key in owned_keys:
            try:
                subprocess.run(["pnputil.exe", "/remove-device", "ROOT\\LOOPZAUDIO\\LZ_" + key],
                               check=True, capture_output=True, timeout=30)
            except Exception as error:
                cleanup_errors.append(str(error))
        if cleanup_errors:
            raise RuntimeError("Acceptance cleanup failed: " + "; ".join(cleanup_errors))
    final = state()
    assert final["snapshot"]["document"] == original_document, "Existing saved mix was changed"
    final_microphones = {
        d["id"]: d.get("microphone", {}).get("uid")
        for d in final["snapshot"]["runtime"]["devices"]
    }
    assert all(final_microphones.get(i) == uid for i, uid in original_microphones.items())
    report_output["existingSavedMixesPreserved"] = True
    report_output["existingMicrophoneIdsPreserved"] = True
    print(json.dumps(report_output))
