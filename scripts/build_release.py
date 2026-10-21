#!/usr/bin/env python3
"""Build a signed, self-contained DMG. Python is a build tool, never shipped."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

from desktop_install import requirement
from distribution_signing import require_distribution_signature
from stage_signed import ROOT, certificate_password, signing_environment

VERSION = "0.1.0"
TEAM = "USJ7H3X44X"


def run(args, **kwargs):
    result = subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=180, **kwargs
    )
    if result.returncode:
        raise RuntimeError(f"Release operation failed: {Path(str(args[0])).name}")
    return result


def sign(path, identifier, environment, engine=False):
    args = [
        "/usr/bin/codesign",
        "--force",
        "--sign",
        environment["LOOPZ_ENGINE_SIGNING_IDENTITY"],
        "--keychain",
        environment["LOOPZ_ENGINE_SIGNING_KEYCHAIN"],
        "--options",
        "runtime",
        "--timestamp",
        "--identifier",
        identifier,
    ]
    if engine:
        args += ["--entitlements", ROOT / "packaging/entitlements.engine.plist"]
    run([*args, path], env=environment)
    require_distribution_signature(path, identifier, requirement(identifier), engine=engine)


def build(build_dir, output, config):
    cache = (build_dir / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        raise ValueError("Distribution requires a Release build")
    binaries = [
        "LoopZ.app/Contents/MacOS/LoopZ",
        "LoopZ.app/Contents/MacOS/LoopZLogin",
        "LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
        "LoopZSetup",
        "routing-migration/LoopZAudio.driver/Contents/MacOS/LoopZAudio",
        "loopz-routing-broker",
        "loopz-config",
        "loopz-routing-install-check",
    ]
    for binary in binaries:
        metadata = run(["/usr/bin/xcrun", "vtool", "-show-build", build_dir / binary]).stdout
        if not any(line.strip() == "minos 14.2" for line in metadata.splitlines()):
            raise ValueError("Every shipped executable must target macOS 14.2")
    architecture = run(
        ["/usr/bin/lipo", "-archs", build_dir / "LoopZ.app/Contents/MacOS/LoopZ"]
    ).stdout.strip()
    if architecture not in ("arm64", "x86_64"):
        raise ValueError("Release builds must contain exactly one supported architecture")
    tag = f"v{VERSION}"
    if json.loads((ROOT / "interface/package.json").read_text())["version"] != VERSION:
        raise ValueError("Release versions disagree")
    output.mkdir(parents=True, exist_ok=True)
    dmg = output / f"LoopZ-{VERSION}-macOS-{architecture}.dmg"
    if dmg.exists():
        raise ValueError("Refusing to replace an existing release artifact")
    certificate = Path(config["certificateFile"]).expanduser().resolve()
    password_store = Path(config.get("passwordStore", "")).expanduser()
    with tempfile.TemporaryDirectory(prefix="loopz-release-", dir=output) as temporary:
        root = Path(temporary)
        image = root / "image"
        image.mkdir()
        app = image / "LoopZ.app"
        shutil.copytree(build_dir / "LoopZ.app", app)
        audio = app / "Contents/Resources/AudioComponents"
        audio.mkdir(parents=True)
        helper = app / "Contents/Library/Helpers/LoopZSetup"
        helper.parent.mkdir(parents=True)
        shutil.copy2(build_dir / "LoopZSetup", helper)
        shutil.copytree(build_dir / "LoopZ Engine.app", audio / "LoopZ Engine.app")
        shutil.copytree(
            build_dir / "routing-migration/LoopZAudio.driver", audio / "LoopZAudio.driver"
        )
        for name in ("loopz-routing-broker", "loopz-config", "loopz-routing-install-check"):
            shutil.copy2(build_dir / name, audio / name)
        broker_requirement = (
            'anchor apple generic and identifier "com.lexasoft.loopz.routing.broker" and '
            f'certificate leaf[subject.OU] = "{TEAM}" and '
            "certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
        )
        engine_requirement = (
            'anchor apple generic and identifier "com.lexasoft.loopz.engine" and '
            f'certificate leaf[subject.OU] = "{TEAM}" and '
            "certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
        )
        pin = json.dumps({"protocol": 2, "brokerRequirement": broker_requirement}, indent=2) + "\n"
        (audio / "routing-service.json").write_text(pin)
        resources = audio / "LoopZAudio.driver/Contents/Resources"
        resources.mkdir(exist_ok=True)
        (resources / "routing-service.json").write_text(pin)
        daemon = {
            "Label": "com.lexasoft.loopz.routing",
            "ProgramArguments": [
                "/Library/Application Support/LoopZ/loopz-routing-broker",
                "--engine-requirement",
                engine_requirement,
            ],
            "MachServices": {"com.lexasoft.loopz.routing.bridge": True},
            "KeepAlive": True,
            "RunAtLoad": True,
            "ProcessType": "Background",
            "ThrottleInterval": 3,
        }
        (audio / "com.lexasoft.loopz.routing.plist").write_bytes(plistlib.dumps(daemon))
        components = {
            helper: "com.lexasoft.loopz.setup",
            app / "Contents/MacOS/LoopZLogin": "com.lexasoft.loopz.login",
            audio / "LoopZ Engine.app": "com.lexasoft.loopz.engine",
            audio / "LoopZAudio.driver": "com.lexasoft.loopz.driver",
            audio / "loopz-routing-broker": "com.lexasoft.loopz.routing.broker",
            audio / "loopz-config": "com.lexasoft.loopz.config",
            audio / "loopz-routing-install-check": "com.lexasoft.loopz.routing.install-check",
        }
        with signing_environment(certificate, certificate_password(password_store)) as environment:
            for path, identifier in components.items():
                sign(path, identifier, environment, engine=identifier.endswith(".engine"))
            sign(app, "com.lexasoft.loopz.editor", environment)
            # Read-only end-to-end payload validation. No admin prompt or installation.
            run([helper, "--verify-payload"])
            (image / "Applications").symlink_to("/Applications")
            (image / "Read me.txt").write_text(
                "LoopZ 0.1.0 for macOS 14.2 or later\n\n"
                "Drag LoopZ to Applications, then open it. On a fresh Mac, choose\n"
                "Settings > Install audio components and authorize macOS once.\n"
                "The driver and background service stay installed separately.\n"
                "Python, Node.js and developer tools are not required to use LoopZ.\n\n"
                "Existing LoopZ installations keep their driver and audio defaults.\n"
                "This preview does not overwrite existing audio components.\n\n"
                "https://github.com/lexasoft123/LoopZ\n"
            )
            run(
                [
                    "/usr/bin/hdiutil",
                    "create",
                    "-volname",
                    f"LoopZ {VERSION}",
                    "-srcfolder",
                    image,
                    "-format",
                    "UDZO",
                    "-ov",
                    dmg,
                ]
            )
            run(
                [
                    "/usr/bin/codesign",
                    "--sign",
                    environment["LOOPZ_ENGINE_SIGNING_IDENTITY"],
                    "--keychain",
                    environment["LOOPZ_ENGINE_SIGNING_KEYCHAIN"],
                    "--timestamp",
                    dmg,
                ],
                env=environment,
            )
        record = {
            "version": VERSION,
            "tag": tag,
            "architecture": architecture,
            "minimumMacOS": "14.2",
            "artifact": str(dmg.resolve()),
            "sha256": hashlib.sha256(dmg.read_bytes()).hexdigest(),
            "createdAt": datetime.now(timezone.utc).isoformat(),
            "notarized": False,
            "payloadVerified": True,
            "pythonRequiredByUser": False,
            "firstInstallVerified": False,
            "commit": run(["git", "rev-parse", "HEAD"]).stdout.strip(),
        }
        (output / f"LoopZ-{VERSION}-{architecture}.json").write_text(
            json.dumps(record, indent=2) + "\n"
        )
        print(json.dumps(record, indent=2))
        return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/release-0.1.0")
    parser.add_argument("--config", type=Path, default=ROOT / ".local/macos-signing.json")
    args = parser.parse_args()
    build(args.build.resolve(), args.output.resolve(), json.loads(args.config.read_text()))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError):
        raise SystemExit(
            "Release packaging failed. No installed components were changed."
        ) from None
