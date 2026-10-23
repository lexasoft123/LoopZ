"""Opt-in desktop/admin test: public playback -> saved DSP mix -> public microphone.

Creates and deletes one test mix alongside existing user mixes. Provisioning also
repairs their microphones. Never changes boot security, audio defaults or volumes.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

folder = Path(sys.argv[1]).resolve()
control = folder / "loopz-windows-control.exe"
mix = "pass-acceptance-" + str(uuid.uuid4())
key = None
created = False
provisioned = False


def state():
    reply = subprocess.run([str(control), "--state"], check=True, capture_output=True, timeout=30)
    return json.loads(reply.stdout.decode("utf-8"))


with tempfile.TemporaryDirectory(prefix="loopz-pass-") as temporary:
    request = Path(temporary) / "request.json"

    def edit(commands):
        before = state()
        request.write_text(json.dumps({"version": 1, "operation": "edit", "request": {
            "version": 1, "expected": before["snapshot"]["revision"], "commands": commands,
        }}), encoding="utf-8")
        reply = subprocess.run([str(control), "--request", str(request)],
                               capture_output=True, timeout=30)
        result = json.loads(reply.stdout.decode("utf-8"))
        assert reply.returncode == 0 and result["ok"], result

    try:
        original_microphones = {
            d["id"]: d.get("microphone", {}).get("uid")
            for d in state()["snapshot"]["runtime"]["devices"]
            if d.get("microphone", {}).get("uid")
        }
        edit([{"type": "createDevice", "id": mix, "name": "Pass-through acceptance", "passThrough": True},
              {"type": "setDeviceControls", "device": mix, "controls": {"gain": 0.5}}])
        created = True
        owner = subprocess.check_output(["whoami.exe", "/user", "/fo", "csv", "/nh"], text=True).strip().split(",")[-1].strip('"')
        key = hashlib.sha256((owner + "\0" + mix).encode("utf-8")).hexdigest()
        # Use the authoritative saved document, not a stale API snapshot.
        routing = Path.home() / "AppData" / "Local" / "LoopZ" / "routing.json"
        installed = subprocess.run([str(folder / "loopz-driver-setup.exe"), "--sync", str(routing), owner],
                                   capture_output=True, timeout=90)
        assert installed.returncode == 0, (installed.stdout + installed.stderr).decode("utf-8", errors="replace")
        provisioned = True
        microphone = {}
        for _ in range(60):
            current = state()["snapshot"]["runtime"]["devices"]
            microphone = next((d["microphone"] for d in current if d["id"] == mix), {})
            if microphone.get("connected"):
                break
            time.sleep(0.5)
        assert microphone.get("connected"), microphone
        after_microphones = {
            d["id"]: d.get("microphone", {}).get("uid")
            for d in state()["snapshot"]["runtime"]["devices"]
        }
        preserved = all(after_microphones.get(d) == uid for d, uid in original_microphones.items())
        probe = subprocess.run([str(folder / "loopz-windows-cable-probe.exe"), "--engine-roundtrip", key],
                               capture_output=True, timeout=40)
        if probe.returncode:
            diagnostic = state()["snapshot"]["runtime"]
            print(json.dumps({"diagnostic": diagnostic}), file=sys.stderr)
        assert probe.returncode == 0, (probe.stdout + probe.stderr).decode("utf-8", errors="replace")
        result = json.loads(probe.stdout.decode("utf-8"))
        assert result["ok"] and result["engineRoundtrip"] and len(result["phases"]) == 3, result
        assert preserved, {"original": original_microphones, "after": after_microphones, "signal": result}
        print(json.dumps({"ok": True, "processedPassThrough": True,
                          "existingMicrophonesPreserved": preserved, "gain": 0.5, "signal": result}))
    finally:
        if created:
            edit([{"type": "deleteDevice", "device": mix}])
        if key and provisioned:
            subprocess.run(["pnputil.exe", "/remove-device", "ROOT\\LOOPZAUDIO\\LZ_" + key],
                           check=True, capture_output=True, timeout=30)
