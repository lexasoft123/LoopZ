#!/usr/bin/env python3
"""Stage the desktop/Engine pair with SingZ's disposable signing keychain."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid

from stage_signed import ROOT, SigningError, certificate_password, signing_environment
from reuse_signed_component import reuse_component
from distribution_signing import require_distribution_signature


def requirement(identifier):
    return (
        f'identifier "{identifier}" and anchor apple generic '
        "and certificate 1[field.1.2.840.113635.100.6.2.6] exists "
        "and certificate leaf[field.1.2.840.113635.100.6.1.13] exists "
        'and certificate leaf[subject.OU] = "USJ7H3X44X"'
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-macos")
    parser.add_argument("--config", type=Path, default=ROOT / ".local/macos-signing.json")
    parser.add_argument(
        "--distribution",
        action="store_true",
        help="Require secure Apple timestamps; do not reuse development signatures",
    )
    args = parser.parse_args()
    cache = (args.build / "CMakeCache.txt").read_text()
    match = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache, re.MULTILINE)
    build_type = match.group(1) if match else "unspecified"
    if args.distribution and build_type != "Release":
        raise SigningError("Distribution staging requires an optimized Release build")
    config = json.loads(args.config.read_text())
    certificate = Path(config["certificateFile"]).expanduser().resolve()
    store = Path(config["passwordStore"]).expanduser().resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    output = ROOT / "artifacts" / f"desktop-{stamp}-{uuid.uuid4().hex[:8]}"
    output.parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".desktop-stage-", dir=output.parent) as temporary:
        staged = Path(temporary) / "payload"
        staged.mkdir()
        components = [
            ("LoopZ.app", "com.lexasoft.loopz.editor", None),
            (
                "LoopZ Engine.app",
                "com.lexasoft.loopz.engine",
                ROOT / "packaging/entitlements.engine.plist",
            ),
        ]
        for name, _, _ in components:
            source = args.build.resolve() / name
            if not source.is_dir() or source.is_symlink():
                raise SigningError(f"Build {name} before signed staging")
            shutil.copytree(source, staged / name, symlinks=True)
        identities = {}
        reused = []
        signing_components = [
            ("LoopZ.app/Contents/Library/Helpers/LoopZSetup", "com.lexasoft.loopz.setup", None),
            ("LoopZ.app/Contents/MacOS/LoopZLogin", "com.lexasoft.loopz.login", None),
            *components,
        ]
        with signing_environment(certificate, certificate_password(store)) as environment:
            for name, identifier, entitlements in signing_components:
                bundle = staged / name
                command = [
                    "/usr/bin/codesign",
                    "--force",
                    "--sign",
                    environment["LOOPZ_ENGINE_SIGNING_IDENTITY"],
                    "--keychain",
                    environment["LOOPZ_ENGINE_SIGNING_KEYCHAIN"],
                    "--options",
                    "runtime",
                    "--timestamp" if args.distribution else "--timestamp=none",
                    "--identifier",
                    identifier,
                ]
                if entitlements:
                    command += ["--entitlements", str(entitlements)]
                subprocess.run(
                    [*command, str(bundle)], check=True, env=environment, capture_output=True
                )
                subprocess.run(
                    [
                        "/usr/bin/codesign",
                        "--verify",
                        "--strict",
                        "-R",
                        "=" + requirement(identifier),
                        str(bundle),
                    ],
                    check=True,
                    capture_output=True,
                )
                data = subprocess.run(
                    ["/usr/bin/codesign", "-d", "--verbose=4", str(bundle)],
                    check=True,
                    text=True,
                    capture_output=True,
                ).stderr
                if (
                    f"Identifier={identifier}" not in data
                    or "TeamIdentifier=USJ7H3X44X" not in data
                    or "runtime" not in data
                ):
                    raise SigningError("The staged desktop pair has an unexpected signing identity")
                identities[name] = {
                    "identifier": identifier,
                    "team": "USJ7H3X44X",
                    "hardenedRuntime": True,
                }
                if not args.distribution and name in (
                    "LoopZ.app/Contents/MacOS/LoopZLogin",
                    "LoopZ Engine.app",
                ):
                    if reuse_component(
                        bundle, Path("/Applications") / name, requirement(identifier)
                    ):
                        reused.append(name)
                if args.distribution:
                    require_distribution_signature(
                        bundle, identifier, requirement(identifier), engine=bool(entitlements)
                    )
        files = {}
        for path in sorted(staged.rglob("*")):
            if path.is_symlink():
                raise SigningError("Desktop payload contains a symbolic link")
            if path.is_file():
                files[str(path.relative_to(staged))] = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest = {
            "schema": 1,
            "kind": "desktop-review",
            "buildConfiguration": build_type,
            "identities": identities,
            "files": files,
            "installed": False,
            "loginRegistered": False,
            "reusedInstalledComponents": reused,
            "distributionSignatures": args.distribution,
        }
        (staged / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        os.rename(staged, output)
    print(
        json.dumps(
            {
                "package": str(output),
                "manifestSHA256": hashlib.sha256(
                    (output / "manifest.json").read_bytes()
                ).hexdigest(),
                "installed": False,
                "reusedInstalledComponents": reused,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (SigningError, OSError, ValueError, KeyError, subprocess.CalledProcessError):
        raise SystemExit("Desktop staging failed; no installation was performed") from None
