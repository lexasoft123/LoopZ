"""Opt-in read-only snapshots around a separately authorized driver upgrade.

Run capture before stopping the engine and check after restarting it. Records
all six Windows default roles, installed driver versions and every published mix
identity. Never rewrites endpoint IDs, defaults, saved mixes or boot settings.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("mode", choices=("capture", "check"))
parser.add_argument("app_directory", type=Path)
parser.add_argument("baseline", type=Path)
parser.add_argument("--expected-version", required=True)
args = parser.parse_args()
folder = args.app_directory.resolve()
if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", args.expected_version):
    parser.error("Expected a four-component driver version")


def run_json(command):
    result = subprocess.run(command, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).decode("utf-8", errors="replace"))
    return json.loads(result.stdout.decode("utf-8-sig"))


def snapshot():
    state = run_json([str(folder / "loopz-windows-control.exe"), "--state"])
    assert state["platform"] == "windows" and state["connection"]["ready"], state.get("connection")
    microphones = {}
    definitions = {d["id"]: d for d in state["snapshot"]["document"]["devices"]}
    for device in state["snapshot"]["runtime"]["devices"]:
        mic = device.get("microphone", {})
        if not mic.get("installed"):
            continue
        key = mic["mixKey"]
        assert re.fullmatch(r"[0-9a-f]{64}", key), "Invalid native mix identity"
        assert mic.get("uid"), "Missing microphone endpoint identity"
        if device.get("active"):
            assert mic.get("connected"), mic
        microphones[device["id"]] = {"mixKey": key, "endpoint": mic["uid"],
            "sampleRate": int(definitions[device["id"]]["sampleRate"]),
            "channels": definitions[device["id"]]["routing"]["outputChannels"]}
    assert microphones, "Requires an installed microphone to prove a real upgrade"
    defaults = run_json([str(folder / "loopz-windows-cable-probe.exe"), "--defaults"])
    assert set(defaults) == {flow + "." + role for flow in ("capture", "render")
                             for role in ("console", "multimedia", "communications")}
    assert all(isinstance(value, str) for value in defaults.values())
    # Only validated hexadecimal keys enter this fixed PowerShell expression.
    keys = ",".join("'" + mic["mixKey"] + "'" for mic in microphones.values())
    command = (
        "$ErrorActionPreference='Stop'; $values=@(foreach($key in @(" + keys + ")) { "
        "[PSCustomObject]@{mixKey=$key; version=(Get-PnpDeviceProperty -InstanceId "
        "('ROOT\\LOOPZAUDIO\\LZ_'+$key) -KeyName 'DEVPKEY_Device_DriverVersion').Data} }); "
        "ConvertTo-Json -InputObject $values -Compress"
    )
    versions = run_json(["powershell.exe", "-NoProfile", "-Command", command])
    assert len(versions) == len(microphones)
    assert all(row["version"] == args.expected_version for row in versions), versions
    return {"version": 1, "driverVersion": args.expected_version,
            "microphones": microphones, "defaults": defaults}


# EndpointBuilder and the background engine can need a few seconds after PnP.
deadline = time.monotonic() + 30
while True:
    try:
        current = snapshot()
        break
    except (AssertionError, RuntimeError, subprocess.TimeoutExpired):
        if time.monotonic() >= deadline:
            raise
        time.sleep(1)

if args.mode == "capture":
    assert not args.baseline.exists(), "Use a new baseline file; do not replace earlier evidence"
    args.baseline.write_text(json.dumps(current, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"ok": True, "captured": len(current["microphones"]),
                      "driverVersion": current["driverVersion"]}))
else:
    before = json.loads(args.baseline.read_text(encoding="utf-8"))
    assert before["version"] == 1 and before["microphones"], "Invalid baseline"
    assert before["driverVersion"] != current["driverVersion"], "Requires an actual version upgrade"
    same_mixes = set(before["microphones"]) == set(current["microphones"])
    same_keys = same_mixes and all(current["microphones"][key]["mixKey"] == mic["mixKey"]
                                  for key, mic in before["microphones"].items())
    same_endpoints = same_keys and all(current["microphones"][key]["endpoint"] == mic["endpoint"]
                                     for key, mic in before["microphones"].items())
    reopened = []
    reopen_errors = []
    for key, mic in before["microphones"].items():
        profile = current["microphones"].get(key, mic)
        try:
            reopened.append(run_json([str(folder / "loopz-windows-cable-probe.exe"), "--reopen-capture",
                mic["endpoint"], str(mic.get("sampleRate", profile.get("sampleRate", 48000))),
                str(mic.get("channels", profile.get("channels", 2)))]))
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            reopen_errors.append(str(error))
    same_defaults = before["defaults"] == current["defaults"]
    report = {"ok": same_endpoints and same_defaults and not reopen_errors, "fromVersion": before["driverVersion"],
              "toVersion": current["driverVersion"], "mixIdentitiesPreserved": same_keys,
              "endpointIdsPreserved": same_endpoints, "allDefaultRolesPreserved": same_defaults,
              "reopenedCachedSelections": reopened, "reopenErrors": reopen_errors,
              "before": before, "after": current}
    print(json.dumps(report))
    raise SystemExit(0 if report["ok"] else 1)
