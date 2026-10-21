#!/usr/bin/env python3
"""Update a stopped desktop pair and existing routing driver with guarded rollback."""

import argparse
from functools import partial
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

from application_ownership import application_owners
from desktop_files import DesktopFileTransaction
from desktop_install import deploy
from driver_update import (
    DriverTransition,
    DriverUpdateHost,
    UPDATE_NAMES,
    UPDATE_TARGETS,
    assemble,
    verify_update,
)
from install_desktop import verify_code, write_result
from install_transaction import blocked_signals, exclusive_lock
from routing_install import SUPPORT


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest-sha")
    parser.add_argument("--replace-engine", action="store_true")
    parser.add_argument("--replace-editor", action="store_true")
    parser.add_argument("--guard-uid")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    package = args.package.absolute()
    _, digest = verify_update(package, verify_code, args.manifest_sha)
    if args.check and os.geteuid() == 0:
        parser.error("Run --check as the console user")
    if not args.check and (os.geteuid() != 0 or not args.manifest_sha):
        parser.error("Installation requires administrator access and the reviewed manifest hash")
    host = DriverUpdateHost(args.guard_uid)
    host.validate_installed_driver(verify_code)
    host.existing_apps(package / "desktop", args.replace_engine, args.replace_editor)
    host.require_idle(host.snapshot())
    host.protected_state()
    if (
        len(
            {
                os.stat(path).st_dev
                for path in ("/private/tmp", "/Applications", "/Library/Audio/Plug-Ins/HAL")
            }
        )
        != 1
    ):
        raise RuntimeError(
            "Atomic update requires one filesystem for apps, HAL and private preparation"
        )
    if args.check:
        print(
            json.dumps(
                {
                    "status": "ready",
                    "manifestSHA256": digest,
                    "targets": {key: str(path) for key, path in UPDATE_TARGETS.items()},
                    "brokerPreserved": True,
                    "loginRegistered": False,
                    "isolatedHostInspectionRequiresAdministrator": True,
                    "deploymentVerified": False,
                }
            )
        )
        return
    with exclusive_lock(SUPPORT / ".install.lock", readable=True):
        with tempfile.TemporaryDirectory(
            prefix="loopz-driver-update-", dir="/private/tmp"
        ) as temporary:
            work = Path(temporary).resolve()
            work.chmod(0o711)
            private_parent = work / "private"
            private_parent.mkdir(mode=0o700)
            private = private_parent / "package"
            shutil.copytree(package, private, symlinks=True)
            _, digest = verify_update(private, verify_code, args.manifest_sha)
            host.existing_apps(private / "desktop", args.replace_engine, args.replace_editor)
            host.validate_installed_driver(verify_code)
            payload = assemble(private, work)
            result = deploy(
                payload,
                UPDATE_TARGETS,
                host,
                work,
                names=UPDATE_NAMES,
                transaction_factory=partial(
                    DesktopFileTransaction, owners=application_owners(host.uid, host.user.pw_gid)
                ),
                driver_transition=DriverTransition(host),
            )
            result["applicationOwnerUID"] = host.uid
            result["ordinaryUpdatesRequireAdministrator"] = False
            result["manifestSHA256"] = digest
            with blocked_signals():
                write_result(result)
                print(json.dumps(result), flush=True)
            if result["status"] != "passed":
                raise SystemExit(1)


if __name__ == "__main__":

    def interrupted(signum, frame):
        raise KeyboardInterrupt("Driver update interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from None
