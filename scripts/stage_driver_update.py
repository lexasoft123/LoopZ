#!/usr/bin/env python3
"""Stage a signed desktop pair plus existing-slot routing driver; do not install."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from driver_update import BROKER_PIN, DRIVER_NAME, verify_update
from install_desktop import verify_code
from desktop_install import bounded_files, verify_package
from stage_signed import ROOT, SigningError, certificate_password, signing_environment
from prepare_routing_install import SERVICE
from distribution_signing import require_distribution_signature
from desktop_install import IDENTIFIERS, requirement


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--desktop-package", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    parser.add_argument("--config", type=Path, default=ROOT / ".local/macos-signing.json")
    parser.add_argument(
        "--distribution",
        action="store_true",
        help="Require timestamped desktop and driver signatures for notarization",
    )
    args = parser.parse_args()
    _, desktop_digest = verify_package(args.desktop_package, verify_code)
    if args.distribution:
        for name, identifier in IDENTIFIERS.items():
            require_distribution_signature(
                args.desktop_package / name,
                identifier,
                requirement(identifier),
                engine=name == "LoopZ Engine.app",
            )
    source = args.build / "routing-migration" / DRIVER_NAME
    if not source.is_dir() or not bounded_files(source):
        raise ValueError("Build the existing-slot routing driver before staging")
    config = json.loads(args.config.read_text())
    output = (
        ROOT
        / "artifacts"
        / (
            "driver-update-"
            + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
            + "-"
            + uuid.uuid4().hex[:8]
        )
    )
    with tempfile.TemporaryDirectory(
        prefix=".driver-update-stage-", dir=output.parent
    ) as temporary:
        package = Path(temporary) / "payload"
        package.mkdir()
        shutil.copytree(args.desktop_package, package / "desktop", symlinks=True)
        driver = package / "driver" / DRIVER_NAME
        shutil.copytree(source, driver, symlinks=True)
        resources = driver / "Contents/Resources"
        resources.mkdir(exist_ok=True)
        (resources / "routing-service.json").write_text(json.dumps(BROKER_PIN, indent=2) + "\n")
        certificate = Path(config["certificateFile"]).expanduser().resolve()
        store = Path(config["passwordStore"]).expanduser().resolve()
        with signing_environment(certificate, certificate_password(store)) as environment:
            subprocess.run(
                [
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
                    "com.lexasoft.loopz.driver",
                    str(driver),
                ],
                env=environment,
                check=True,
                capture_output=True,
            )
        if args.distribution:
            require_distribution_signature(
                driver, "com.lexasoft.loopz.driver", requirement("com.lexasoft.loopz.driver")
            )
        checked = json.loads(
            subprocess.run(
                [
                    str(args.build / "loopz-routing-pin-check"),
                    str(driver / "Contents/MacOS/LoopZAudio"),
                ],
                check=True,
                capture_output=True,
                text=True,
            ).stdout
        )
        if checked != {
            "brokerRequirement": BROKER_PIN["brokerRequirement"],
            "service": SERVICE,
            "serverUID": 0,
            "privileged": True,
        }:
            raise ValueError("Native driver pin loader disagrees with the expected broker")
        manifest = {
            "schema": 1,
            "kind": "loopz-desktop-driver-update",
            "installed": False,
            "desktopManifestSHA256": desktop_digest,
            "distributionSignatures": args.distribution,
            "files": bounded_files(package),
        }
        (package / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        _, digest = verify_update(package, verify_code)
        package.rename(output)
    print(
        json.dumps({"package": str(output), "manifestSHA256": digest, "installed": False}, indent=2)
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, SigningError, subprocess.SubprocessError) as error:
        raise SystemExit(
            "Driver update staging failed; no installation performed: " + str(error)
        ) from None
