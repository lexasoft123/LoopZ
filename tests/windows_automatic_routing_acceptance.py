"""Opt-in desktop acceptance of captured audio moving between two virtual Feeds.

Uses the installed automatic coordinator to create/remove unique temporary mixes (three in group mode).
The native marker starts in silence, guards tone output to
those Feeds, contains its child in a kill-on-close job, and stops rendering before
restoring preferences. No hardware monitor or global default is changed.

LOOPZ_AUTOMATIC_ROUTING_ACCEPTANCE=1 SCRIPT APPDIR FRESH_REPORT ROUTE_PROBE CABLE_PROBE
Optional LOOPZ_AUTOMATIC_ROUTING_MODE=lifecycle runs a silent exit/relaunch policy
probe instead of the captured-tone test. Mode override runs a fully silent native
runtime save-failure/reopen/acknowledgment fixture against a separate document.
It additionally requires LOOPZ_APP_OVERRIDE_ACCEPTANCE=1. Mode group runs explicit
owned-Feed tones, verifies parent/child and independent-instance capture, and
requires LOOPZ_GROUP_CAPTURE_ACCEPTANCE=1; it never renders to hardware.
Installed mode requires LOOPZ_INSTALLED_ROUTING_ACCEPTANCE=1 and exercises the
ordinary installed engine and broker from first-use history through receipt and
sink cleanup. Sources stay silent until automatic stream movement is verified.
No mode proves general third-party application compatibility.
"""

import csv
import ctypes
from ctypes import wintypes
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

if sys.platform != "win32" or os.environ.get("LOOPZ_AUTOMATIC_ROUTING_ACCEPTANCE") != "1":
    raise RuntimeError("Requires explicit opt-in on the Windows desktop workbench")
if len(sys.argv) != 5:
    raise ValueError("Use SCRIPT APPDIR FRESH_REPORT ROUTE_PROBE CABLE_PROBE")
folder, report_path, probe, cable = (Path(value).resolve() for value in sys.argv[1:])
if report_path.exists():
    raise RuntimeError("A fresh report path is required")
control = folder / "loopz-windows-control.exe"
routing = Path(os.environ["LOCALAPPDATA"]) / "LoopZ" / "routing.json"
mode = os.environ.get("LOOPZ_AUTOMATIC_ROUTING_MODE", "capture")
if mode not in ("capture", "lifecycle", "override", "group", "installed"):
    raise ValueError(
        "Automatic routing mode must be capture, lifecycle, override, group or installed"
    )
parent_exit = os.environ.get("LOOPZ_GROUP_PARENT_EXIT_ACCEPTANCE") == "1"
if parent_exit and (mode != "group" or os.environ.get("LOOPZ_GROUP_ROUTING_ACCEPTANCE") == "1"):
    raise ValueError("Surviving-child acceptance requires capture-only group mode")
report = {"ok": False, "cleanup": False, "fullWindowsPortComplete": False, "mode": mode}
report["parentExitRequested"] = parent_exit


def run(arguments, timeout=45, allow_failure=False):
    result = subprocess.run([str(arg) for arg in arguments], capture_output=True, timeout=timeout)
    if result.returncode and not allow_failure:
        raise RuntimeError((result.stdout + result.stderr).decode("utf-8", errors="replace"))
    return json.loads(result.stdout.decode("utf-8"))


def state():
    return run([control, "--state"])


def save():
    pending = report_path.with_suffix(".pending")
    pending.write_text(json.dumps(report, indent=2), encoding="utf-8")
    pending.replace(report_path)


def adapter_exists(key):
    locate = ctypes.WinDLL("cfgmgr32").CM_Locate_DevNodeW
    locate.argtypes = (ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD)
    locate.restype = wintypes.DWORD
    node = wintypes.DWORD()
    result = locate(ctypes.byref(node), "ROOT\\LOOPZAUDIO\\LZ_" + key, 1)
    if result == 0x0D:
        return False
    if result:
        raise RuntimeError(f"Cannot inspect test root: CONFIGRET 0x{result:x}")
    return True


def marker_processes():
    # The native fixture uses a unique copied image for every run. Existing
    # markers are not ours and must neither block their cleanup nor be stopped.
    text = (
        subprocess.check_output(
            [
                "powershell.exe",
                "-NoProfile",
                "-Command",
                "ConvertTo-Json -Compress -InputObject @(Get-CimInstance Win32_Process | "
                "Where-Object { $_.Name -eq 'marker.exe' } | "
                "Select-Object -ExpandProperty ExecutablePath)",
            ],
            timeout=30,
        )
        .decode("utf-8-sig")
        .strip()
    )
    return set(json.loads(text) if text else [])


before = state()
if not before.get("microphoneSetup", {}).get("enabled"):
    raise RuntimeError("Requires the installed automatic microphone coordinator")
document = before["snapshot"]["document"]
if any(device.get("monitors") for device in document["devices"]):
    raise RuntimeError("Virtual-only acceptance refuses saved hardware monitors")
original_bytes = routing.read_bytes()
defaults = run([cable, "--defaults"])
microphones = {
    device["id"]: device["microphone"]["uid"]
    for device in before["snapshot"]["runtime"]["devices"]
    if device.get("microphone", {}).get("uid")
}
owner_text = subprocess.check_output(
    ["whoami.exe", "/user", "/fo", "csv", "/nh"], timeout=10
).decode("utf-8", errors="replace")
owner = next(csv.reader(io.StringIO(owner_text)))[-1]
identities = [str(uuid.uuid4()) for _ in range(3 if mode in ("group", "installed") else 2)]
keys = [hashlib.sha256((owner + "\0" + identity).encode()).hexdigest() for identity in identities]
existing_markers = marker_processes()
existing_directories = set(Path(tempfile.gettempdir()).glob("LoopZ-route-*"))
report.update(
    mixIds=identities,
    mixKeys=keys,
    savedDocumentSha256=hashlib.sha256(original_bytes).hexdigest(),
    hashes={
        str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (control, probe, cable)
    },
)
save()
with tempfile.TemporaryDirectory(prefix="loopz-automatic-route-") as temporary:
    request = Path(temporary) / "request.json"

    def edit(commands):
        current = state()
        request.write_text(
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
        reply = run([control, "--request", request], timeout=60)
        if not reply.get("ok"):
            raise RuntimeError(str(reply))

    try:
        edit(
            [
                {
                    "type": "createDevice",
                    "id": identity,
                    "name": "Automatic route acceptance " + str(index + 1),
                    "passThrough": mode != "installed" or index != 2,
                    "channels": 2,
                    "sampleRate": 48000,
                }
                for index, identity in enumerate(identities)
            ]
            + (
                [
                    {
                        "type": "setDeviceControls",
                        "device": identities[2],
                        "controls": {"enabled": False},
                    }
                ]
                if mode == "group"
                else []
            )
        )
        deadline = time.monotonic() + 60
        while True:
            current = state()
            devices = {device["id"]: device for device in current["snapshot"]["runtime"]["devices"]}
            if all(
                devices.get(identity, {}).get("microphone", {}).get("connected")
                for identity in (identities if mode == "installed" else identities[:2])
            ) and (
                mode != "group"
                or (
                    devices.get(identities[2], {}).get("microphone", {}).get("installed")
                    and devices.get(identities[2], {}).get("microphone", {}).get("formatReady")
                    and not devices.get(identities[2], {}).get("microphone", {}).get("connected")
                )
            ):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Temporary microphone setup did not finish")
            time.sleep(0.2)
        endpoints = run([cable, "--endpoints"])
        feeds = []
        for key in keys:
            matches = [
                endpoint["uid"]
                for endpoint in endpoints
                if endpoint.get("mixKey") == key and endpoint["role"] == 1
            ]
            if len(matches) != 1:
                raise RuntimeError("Temporary Feed hardware identity is ambiguous")
            feeds.append(matches[0].removeprefix("wasapi:"))
        public_mic = None
        if mode in ("group", "installed"):
            matches = [e["uid"] for e in endpoints if e.get("mixKey") == keys[2] and e["role"] == 2]
            if len(matches) != 1:
                raise RuntimeError("Temporary public microphone identity is ambiguous")
            public_mic = matches[0].removeprefix("wasapi:")
        report["feeds"] = feeds
        report["runtimePublicMicrophone"] = public_mic
        save()
        arguments = (
            [probe, "--installed-route", identities[2], feeds[0], feeds[1], public_mic]
            if mode == "installed"
            else (
                [
                    probe,
                    feeds[0],
                    identities[0],
                    feeds[1],
                    identities[1],
                    feeds[2],
                    identities[2],
                    public_mic,
                ]
                if mode == "group"
                else (
                    [probe, *feeds]
                    if mode == "override"
                    else (
                        [probe, "--automatic-lifecycle-probe", feeds[0]]
                        if mode == "lifecycle"
                        else [probe, "--automatic-capture-route-probe", *feeds]
                    )
                )
            )
        )
        retained = os.environ.get("LOOPZ_GROUP_RECOVER_DIRECTORY")
        if retained:
            if mode != "group":
                raise RuntimeError("Retained fixture recovery requires group mode")
            report["priorFixtureRecovery"] = run(
                [probe, "--recover-fixture", retained, feeds[0]], timeout=30
            )
            save()
        report["native"] = run(
            arguments, timeout=180 if mode == "installed" else 90, allow_failure=True
        )
        if not report["native"].get("ok"):
            raise RuntimeError("Native automatic routing probe failed")
        if parent_exit and (
            not report["native"].get("parentExitExercised")
            or not any(
                row.get("transition") == "parent-exited-descendant-survived"
                for row in report["native"].get("membershipTransitions", [])
            )
        ):
            raise RuntimeError("Native probe did not exercise surviving-child capture")
    except BaseException as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        errors = []
        try:
            # Killing/timing out the parent closes its job and kills its owned
            # marker. Never remove a Feed while a fixture could still render.
            deadline = time.monotonic() + 20
            while marker_processes() - existing_markers:
                if time.monotonic() >= deadline:
                    raise RuntimeError("Owned marker remains; preserve its virtual Feeds")
                time.sleep(0.2)
            current_ids = {device["id"] for device in state()["snapshot"]["document"]["devices"]}
            commands = [
                {"type": "deleteDevice", "device": identity}
                for identity in identities
                if identity in current_ids
            ]
            if commands:
                edit(commands)
            deadline = time.monotonic() + 60
            while any(adapter_exists(key) for key in keys):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Temporary root removal did not finish")
                time.sleep(0.2)
        except Exception as error:
            errors.append(str(error))
        try:
            final = state()
            assert final["snapshot"]["document"] == document, "Saved document changed"
            assert routing.read_bytes() == original_bytes, "Saved document bytes changed"
            assert run([cable, "--defaults"]) == defaults, "System defaults changed"
            final_mics = {
                device["id"]: device.get("microphone", {}).get("uid")
                for device in final["snapshot"]["runtime"]["devices"]
            }
            assert all(
                final_mics.get(identity) == uid for identity, uid in microphones.items()
            ), "Original microphone changed"
            assert not (
                set(Path(tempfile.gettempdir()).glob("LoopZ-route-*")) - existing_directories
            ), "Fixture image directory remains"
        except Exception as error:
            errors.append(str(error))
        report["cleanupErrors"] = errors
        report["cleanup"] = not errors
        report["ok"] = (
            not errors and not report.get("error") and report.get("native", {}).get("ok", False)
        )
        save()
if not report["ok"]:
    raise SystemExit(1)
