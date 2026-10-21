#!/usr/bin/env python3
"""Stage the signed routing runtime and sealed trust pins; never install or start it."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
SERVICE = "com.lexasoft.loopz.routing.bridge"
COMPONENTS = {
    "LoopZ Engine.app": "com.lexasoft.loopz.engine",
    "loopz-routing-broker": "com.lexasoft.loopz.routing.broker",
    "loopz-config": "com.lexasoft.loopz.config",
    "loopz-probe": "com.lexasoft.loopz.probe",
    "loopz-routing-pin-check": "com.lexasoft.loopz.routing.pin-check",
    "loopz-routing-install-check": "com.lexasoft.loopz.routing.install-check",
    "LoopZRoutingAudio.driver": "com.lexasoft.loopz.driver.routing",
}


def component_identities(legacy_slot=False):
    result = dict(COMPONENTS)
    if legacy_slot:
        result["LoopZRoutingAudio.driver"] = "com.lexasoft.loopz.driver"
    return result


def command(*arguments):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(f"{arguments[0]} failed: {result.stderr.strip()}")
    return result


def stable_requirement(identifier, team):
    if not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise RuntimeError("Invalid Apple signing team")
    return (
        f'anchor apple generic and identifier "{identifier}" and '
        f'certificate leaf[subject.OU] = "{team}" and '
        "certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
    )


def sign(path, identifier, identity, keychain, engine=False):
    arguments = [
        "codesign",
        "--force",
        "--sign",
        identity,
        "--timestamp",
        "--options",
        "runtime",
        "--identifier",
        identifier,
    ]
    if keychain:
        arguments += ["--keychain", keychain]
    if engine:
        arguments += ["--entitlements", ROOT / "packaging/entitlements.engine.plist"]
    command(*arguments, path)
    details = command("codesign", "-d", "--verbose=4", path).stderr
    match = re.search(r"^TeamIdentifier=([A-Z0-9]{10})$", details, re.M)
    if not match or not re.search(r"^Authority=Developer ID Application:", details, re.M):
        raise RuntimeError("Production routing components require Developer ID Application signing")
    if not re.search(r"^CodeDirectory .*flags=\S*\(runtime\)", details, re.M):
        raise RuntimeError("Routing component lacks Hardened Runtime")
    requirement = stable_requirement(identifier, match[1])
    command("codesign", "--verify", "--strict", "-R", "=" + requirement, path)
    entitlements = command("codesign", "-d", "--entitlements", "-", "--xml", path).stdout
    actual = plistlib.loads(entitlements.encode()) if entitlements.strip() else {}
    expected = {"com.apple.security.device.audio-input": True} if engine else {}
    if actual != expected:
        raise RuntimeError("Unexpected production routing component entitlements")
    return {"team": match[1], "requirement": requirement, "identifier": identifier}


def stage(build, output, document, identity, keychain=None, legacy_slot=False):
    if not identity or identity == "-":
        raise RuntimeError("Use stage_signed.py --routing with the stable Developer ID identity")
    if output.exists() or output.is_symlink():
        raise RuntimeError("Routing package output already exists; select a new destination")
    if not document.is_absolute():
        raise RuntimeError("The routing document path must be absolute")
    components = component_identities(legacy_slot)

    def source_path(name):
        if legacy_slot and name == "LoopZRoutingAudio.driver":
            return build / "routing-migration/LoopZAudio.driver"
        return build / name

    for name in components:
        source = source_path(name)
        if not source.exists() or source.is_symlink():
            raise RuntimeError(f"Missing or linked build component: {name}")
        if source.is_dir() and any(item.is_symlink() for item in source.rglob("*")):
            raise RuntimeError(f"Linked files in native build component: {name}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".loopz-routing-stage-", dir=output.parent
    ) as temporary:
        folder = Path(temporary) / "package"
        folder.mkdir()
        for name in components:
            source = source_path(name)
            if source.is_dir():
                shutil.copytree(source, folder / name)
            else:
                shutil.copy2(source, folder / name)
        signatures = {}
        for name, identifier in components.items():
            if name == "LoopZRoutingAudio.driver":
                continue
            signatures[name] = sign(
                folder / name, identifier, identity, keychain, engine=name == "LoopZ Engine.app"
            )
        teams = {value["team"] for value in signatures.values()}
        if len(teams) != 1:
            raise RuntimeError("Routing components have different signing teams")
        pin = {
            "protocol": 2,
            "brokerRequirement": signatures["loopz-routing-broker"]["requirement"],
        }
        pin_bytes = (json.dumps(pin, indent=2) + "\n").encode()
        driver = folder / "LoopZRoutingAudio.driver"
        info = plistlib.loads((driver / "Contents/Info.plist").read_bytes())
        if info.get("AudioServerPlugIn_MachServices") != [SERVICE]:
            raise RuntimeError("Driver does not declare its exact broker Mach service")
        resources = driver / "Contents/Resources"
        resources.mkdir(exist_ok=True)
        (resources / "routing-service.json").write_bytes(pin_bytes)
        (folder / "routing-service.json").write_bytes(pin_bytes)
        signatures[driver.name] = sign(driver, components[driver.name], identity, keychain)
        if signatures[driver.name]["team"] not in teams:
            raise RuntimeError("Routing driver has a different signing team")
        checked = json.loads(
            command(
                folder / "loopz-routing-pin-check",
                driver / "Contents/MacOS" / ("LoopZAudio" if legacy_slot else "LoopZRoutingAudio"),
            ).stdout
        )
        if checked != {
            "brokerRequirement": pin["brokerRequirement"],
            "service": SERVICE,
            "serverUID": 0,
            "privileged": True,
        }:
            raise RuntimeError("Native bundle-pin loader disagrees with staged broker trust")
        daemon = {
            "Label": "com.lexasoft.loopz.routing",
            "ProgramArguments": [
                "/Library/Application Support/LoopZ/loopz-routing-broker",
                "--engine-requirement",
                signatures["LoopZ Engine.app"]["requirement"],
            ],
            "MachServices": {SERVICE: True},
            "KeepAlive": True,
            "RunAtLoad": True,
            "ProcessType": "Background",
            "ThrottleInterval": 3,
        }
        agent = {
            "Label": "com.lexasoft.loopz.engine",
            "ProgramArguments": [
                "/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
                "--routing-document",
                str(document),
            ],
            "KeepAlive": True,
            "RunAtLoad": True,
            "ProcessType": "Interactive",
            "ThrottleInterval": 3,
        }
        (folder / "com.lexasoft.loopz.routing.plist").write_bytes(plistlib.dumps(daemon))
        (folder / "com.lexasoft.loopz.engine.plist").write_bytes(plistlib.dumps(agent))
        manifest = {
            "schema": 1,
            "kind": "loopz-routing-migration" if legacy_slot else "loopz-routing-install",
            "audioMappingProtocol": 2,
            "snapshotSchema": 3,
            "serviceEnvelopeProtocol": 2,
            "createdAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "signatures": signatures,
            "bundlePinValidated": checked,
            "documentPath": str(document),
            "installed": False,
            "loginRegistered": False,
            "files": {
                str(path.relative_to(folder)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(folder.rglob("*"))
                if path.is_file()
            },
        }
        (folder / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        folder.rename(output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--build", type=Path, default=ROOT / "build/routing-macos")
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--legacy-slot",
        action="store_true",
        help="Stage an explicit candidate for the existing LoopZ HAL slot",
    )
    parser.add_argument(
        "--document",
        type=Path,
        default=Path.home() / "Library/Application Support/LoopZ/routes.json",
    )
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    output = args.output or ROOT / "artifacts" / f"routing-install-{stamp}-{uuid.uuid4().hex[:8]}"
    output = output.absolute()
    manifest = stage(
        args.build.resolve(),
        output,
        args.document,
        os.environ.get("LOOPZ_ENGINE_SIGNING_IDENTITY"),
        os.environ.get("LOOPZ_ENGINE_SIGNING_KEYCHAIN"),
        legacy_slot=args.legacy_slot,
    )
    print(
        json.dumps(
            {
                "package": str(output),
                "components": list(manifest["signatures"]),
                "bundlePinValidated": True,
                "installed": False,
            }
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(f"Routing staging failed: {error}") from None
