#!/usr/bin/env python3
"""Stage apps, existing-slot driver and broker together; never install or reload services."""

import argparse
import copy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from desktop_install import bounded_files
from driver_update import verify_update
from install_desktop import verify_code
from stage_signed import ROOT, SigningError, certificate_password, signing_environment
from suite_update import BROKER, BROKER_IDENTIFIER, CAPABILITIES, inspect_suite, verify_suite


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--driver-update-package", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    parser.add_argument("--config", type=Path, default=ROOT / ".local/macos-signing.json")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Stage and inspect this package as the console user, not root")
    _, nested_digest = verify_update(args.driver_update_package, verify_code)
    source = args.build / BROKER
    if not source.is_file() or source.is_symlink() or source.stat().st_nlink != 1:
        raise ValueError("Build a regular routing broker before coordinated staging")
    config = json.loads(args.config.read_text())
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    output = ROOT / "artifacts" / f"suite-update-{stamp}-{uuid.uuid4().hex[:8]}"
    output.parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".suite-update-stage-", dir=output.parent) as temporary:
        package = Path(temporary) / "payload"
        package.mkdir()
        shutil.copytree(args.driver_update_package, package / "update", symlinks=True)
        verify_update(package / "update", verify_code, nested_digest)
        broker = package / "broker" / BROKER
        broker.parent.mkdir()
        shutil.copy2(source, broker, follow_symlinks=False)
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
                    "--timestamp=none",
                    "--identifier",
                    BROKER_IDENTIFIER,
                    str(broker),
                ],
                env=environment,
                check=True,
                capture_output=True,
                timeout=90,
            )
        manifest = {
            "schema": 1,
            "kind": "loopz-suite-update",
            "installed": False,
            "updateManifestSHA256": nested_digest,
            "capabilities": copy.deepcopy(CAPABILITIES),
            "files": bounded_files(package),
        }
        (package / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        _, digest = verify_suite(package, verify_code)
        capabilities = inspect_suite(package)
        verify_suite(package, verify_code, digest)
        package.rename(output)
    print(
        json.dumps(
            {
                "package": str(output),
                "manifestSHA256": digest,
                "compiledCapabilities": capabilities,
                "installed": False,
                "deploymentVerified": False,
                "installerAvailable": False,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, SigningError, subprocess.SubprocessError):
        raise SystemExit(
            "Coordinated staging failed; no installed files or services changed"
        ) from None
