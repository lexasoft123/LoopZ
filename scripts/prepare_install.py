#!/usr/bin/env python3
"""Stage signed local prototype artifacts; never installs or starts services."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(allow_abbrev=False)
parser.add_argument("--engine-args-json", default='["--tone", "--seconds", "120"]')
parser.add_argument(
    "--engine-signing-identity",
    default=os.environ.get("LOOPZ_ENGINE_SIGNING_IDENTITY", "-"),
    help="Apple Development/Developer ID identity or fingerprint; defaults to ad-hoc signing",
)
parser.add_argument(
    "--engine-signing-keychain",
    default=os.environ.get("LOOPZ_ENGINE_SIGNING_KEYCHAIN") or os.environ.get("CSC_KEYCHAIN"),
    help="Prepared signing keychain, as used by SingZ; never changes the login keychain",
)
args = parser.parse_args()
if not args.engine_signing_identity.strip():
    parser.error("Engine signing identity must not be empty")
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
    signing_identity = args.engine_signing_identity if name == "LoopZ Engine.app" else "-"
    certificate_engine = name == "LoopZ Engine.app" and signing_identity != "-"
    signing_options = []
    if certificate_engine:
        if args.engine_signing_keychain:
            signing_options.extend(["--keychain", args.engine_signing_keychain])
        signing_options.extend(
            [
                "--options",
                "runtime",
                "--entitlements",
                str(root / "packaging/entitlements.engine.plist"),
            ]
        )
    subprocess.run(
        [
            "codesign",
            "--force",
            "--sign",
            signing_identity,
            "--timestamp" if certificate_engine else "--timestamp=none",
            *signing_options,
            "--identifier",
            identifier,
            str(stage / name),
        ],
        check=True,
        timeout=90,
    )
    subprocess.run(["codesign", "--verify", "--strict", str(stage / name)], check=True)
details = subprocess.run(
    ["codesign", "-d", "--verbose=4", str(stage / "LoopZ Engine.app")],
    text=True,
    capture_output=True,
    check=True,
).stderr
team = re.search(r"^TeamIdentifier=(.+)$", details, re.M)
team_identifier = team[1] if team and team[1] != "not set" else None
certificate_engine = args.engine_signing_identity != "-"
if certificate_engine and not team_identifier:
    raise SystemExit("Certificate-signed engine must have an Apple team identifier")
engine_entitlements = {}
if certificate_engine:
    if not re.search(r"^CodeDirectory .*flags=\S*\(runtime\)", details, re.M):
        raise SystemExit("Certificate-signed engine must enable Hardened Runtime")
    entitlement_details = subprocess.run(
        ["codesign", "-d", "--entitlements", "-", "--xml", str(stage / "LoopZ Engine.app")],
        text=True,
        capture_output=True,
        check=True,
    )
    engine_entitlements = plistlib.loads(entitlement_details.stdout.encode())
    if engine_entitlements != {"com.apple.security.device.audio-input": True}:
        raise SystemExit("Engine must carry exactly its Hardened Runtime microphone entitlement")
designated_details = subprocess.run(
    ["codesign", "-d", "-r-", str(stage / "LoopZ Engine.app")],
    text=True,
    capture_output=True,
    check=True,
)
designated = re.search(
    r"^(?:# )?designated => (.+)$",
    designated_details.stdout + "\n" + designated_details.stderr,
    re.M,
)
if not designated or (certificate_engine and "cdhash" in designated[1]):
    raise SystemExit(
        "Engine designated requirement is missing or not stable for certificate signing"
    )
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
            "developmentSigning": "certificate-signed-engine" if certificate_engine else "ad-hoc",
            "engineSigning": {
                "teamIdentifier": team_identifier,
                "designatedRequirement": designated[1],
                "hardenedRuntime": certificate_engine,
                "entitlements": engine_entitlements,
            },
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
