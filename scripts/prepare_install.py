#!/usr/bin/env python3
"""Stage signed local prototype artifacts; never installs or starts services."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--engine-args-json", default='["--tone", "--seconds", "120"]')
args = parser.parse_args()
engine_args = json.loads(args.engine_args_json)
if not isinstance(engine_args, list) or not all(isinstance(x, str) for x in engine_args):
    raise SystemExit("engine arguments must be an array of strings")
stage = root / "artifacts/feasibility"
if stage.exists():
    shutil.rmtree(stage)
stage.mkdir(parents=True)
for name in [
    "LoopZAudio.driver",
    "LoopZ Engine.app",
    "loopz-broker",
    "loopz-probe",
    "loopz-install-check",
]:
    source = root / "build" / name
    if source.is_dir():
        shutil.copytree(source, stage / name)
    else:
        shutil.copy2(source, stage / name)
identities = {
    "LoopZAudio.driver": "com.lexasoft.loopz.driver",
    "LoopZ Engine.app": "com.lexasoft.loopz.engine",
    "loopz-broker": "com.lexasoft.loopz.broker",
    "loopz-probe": "com.lexasoft.loopz.probe",
    "loopz-install-check": "com.lexasoft.loopz.install-check",
}
for name, identifier in identities.items():
    subprocess.run(
        [
            "codesign",
            "--force",
            "--sign",
            "-",
            "--timestamp=none",
            "--identifier",
            identifier,
            str(stage / name),
        ],
        check=True,
    )
    subprocess.run(["codesign", "--verify", "--strict", str(stage / name)], check=True)
details = subprocess.run(
    ["codesign", "-d", "--verbose=4", str(stage / "LoopZ Engine.app")],
    text=True,
    capture_output=True,
    check=True,
).stderr
match = re.search(r"^CDHash=([a-f0-9]+)$", details, re.M)
if not match:
    raise SystemExit("Cannot pin helper code hash")
requirement = f'identifier "com.lexasoft.loopz.engine" and cdhash H"{match[1]}"'
check_details = subprocess.run(
    ["codesign", "-d", "--verbose=4", str(stage / "loopz-install-check")],
    text=True,
    capture_output=True,
    check=True,
).stderr
check_hash = re.search(r"^CDHash=([a-f0-9]+)$", check_details, re.M)
if not check_hash:
    raise SystemExit("Cannot pin installer checker hash")
control_requirement = f'identifier "com.lexasoft.loopz.install-check" and cdhash H"{check_hash[1]}"'
daemon = {
    "Label": "com.lexasoft.loopz.bridge",
    "ProgramArguments": [
        "/Library/Application Support/LoopZ/loopz-broker",
        "--engine-requirement",
        requirement,
        "--control-requirement",
        control_requirement,
    ],
    "MachServices": {"com.lexasoft.loopz.bridge": True},
    "KeepAlive": True,
    "RunAtLoad": True,
    "ProcessType": "Background",
    "ThrottleInterval": 3,
}
(stage / "com.lexasoft.loopz.bridge.plist").write_bytes(plistlib.dumps(daemon))
# Manual bootstrap only. This prototype does not enable automatic microphone
# capture or install a user login agent without an explicit setup action.
agent = {
    "Label": "com.lexasoft.loopz.engine",
    "ProgramArguments": [
        "/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
        *engine_args,
    ],
    "RunAtLoad": True,
    "ProcessType": "Interactive",
}
(stage / "com.lexasoft.loopz.engine.plist").write_bytes(plistlib.dumps(agent))
hashes = {
    str(p.relative_to(stage)): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in sorted(stage.rglob("*"))
    if p.is_file()
}
(stage / "manifest.json").write_text(
    json.dumps(
        {
            "protocol": 1,
            "developmentSigning": "ad-hoc",
            "engineRequirement": requirement,
            "controlRequirement": control_requirement,
            "engineArguments": engine_args,
            "sha256": hashes,
        },
        indent=2,
    )
    + "\n"
)
print(f"Staged and verified local artifacts: {stage}")
print("No driver, daemon, login agent or application has been installed.")
