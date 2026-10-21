#!/usr/bin/env python3
"""Update the two user-owned apps; never install or alter the system backend."""

import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import tempfile

from desktop_install import NAMES, TARGETS, bounded_files, deploy, verify_package
from install_desktop import DesktopHost, private_snapshot, verify_code
from install_transaction import blocked_signals, check_ancestors, exclusive_lock
from routing_install import SUPPORT


class MigrationRequired(RuntimeError):
    pass


def require_user_apps(targets, uid):
    for path in targets.values():
        check_ancestors([path])
        if not path.is_dir():
            raise MigrationRequired("Install the app pair and system backend first")
        bounded_files(path)
        for entry in [path, *path.rglob("*")]:
            info = entry.lstat()
            if info.st_uid != uid:
                raise MigrationRequired(
                    "The previous installer made the apps root-owned. One administrator "
                    "migration is required; later app updates run as your account."
                )
            if info.st_mode & 0o022:
                raise ValueError("Installed applications must not be writable by other accounts")


@contextmanager
def shared_backend_lock(path, *, expected_owner=0, inspect_only=False):
    """Use the same inode as root installers without writing a privileged file."""
    check_ancestors([path])
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except PermissionError as error:
        raise MigrationRequired("The system update lock needs its one-time migration") from error
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != expected_owner
            or info.st_mode & 0o022
            or info.st_nlink != 1
        ):
            raise ValueError("System update lock has unsafe ownership or permissions")
        if inspect_only:
            yield
            return
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another LoopZ installation or removal is in progress") from error
        yield
    finally:
        os.close(descriptor)


def require_no_pending_maintenance(folder):
    for name in (".uninstall-state.json", ".suite-update-state.json", ".suite-driver-state.json"):
        path = folder / name
        if path.exists() or path.is_symlink():
            raise RuntimeError("Complete the interrupted system maintenance before updating apps")


def private_updates(folder, uid):
    check_ancestors([folder])
    folder.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = folder.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid or info.st_mode & 0o077:
        raise ValueError("App update records require a private user-owned directory")


def save_result(folder, result):
    with tempfile.NamedTemporaryFile(
        mode="w", prefix=".result-", dir=folder, delete=False
    ) as stream:
        path = Path(stream.name)
        try:
            stream.write(json.dumps(result, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
            os.chmod(path, 0o600)
            os.replace(path, folder / "last-result.json")
        finally:
            path.unlink(missing_ok=True)


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest-sha", required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--guard-uid")
    options = parser.parse_args(arguments)
    if not os.getuid() or os.getuid() != os.geteuid():
        parser.error("Run app updates as your account, without sudo")
    if not re.fullmatch(r"[a-f0-9]{64}", options.manifest_sha):
        parser.error("Use the reviewed SHA-256 package digest")
    host = DesktopHost(options.guard_uid)
    if host.uid != os.getuid():
        raise RuntimeError("Only the active console account may update its applications")
    package = options.package.absolute()
    _, digest = verify_package(package, verify_code, options.manifest_sha)
    require_user_apps(TARGETS, host.uid)
    host.existing_apps(package, True, True)
    host.require_idle(host.snapshot())
    host.protected_state()
    require_no_pending_maintenance(SUPPORT)
    # Read-only review does not create a user directory or acquire a file lock.
    if options.check:
        with shared_backend_lock(SUPPORT / ".install.lock", inspect_only=True):
            pass
        print(
            json.dumps(
                {
                    "status": "ready",
                    "manifestSHA256": digest,
                    "requiresAdministrator": False,
                    "driverPreserved": True,
                    "brokerPreserved": True,
                    "deploymentVerified": False,
                }
            )
        )
        return 0
    folder = Path(host.user.pw_dir) / "Library/Application Support/LoopZ/Updates"
    private_updates(folder, host.uid)
    with exclusive_lock(folder / ".update.lock"):
        with shared_backend_lock(SUPPORT / ".install.lock"):
            require_no_pending_maintenance(SUPPORT)
            require_user_apps(TARGETS, host.uid)
            with tempfile.TemporaryDirectory(
                prefix="loopz-user-update-", dir="/private/tmp"
            ) as temporary:
                work = Path(temporary).resolve()
                private = private_snapshot(package, work)
                verify_package(private, verify_code, options.manifest_sha)
                host.existing_apps(private, True, True)
                result = deploy(private, TARGETS, host, work)
                result.update(
                    mode="user-app-update",
                    manifestSHA256=digest,
                    applicationOwnerUID=host.uid,
                    requiresAdministrator=False,
                )
                with blocked_signals():
                    save_result(folder, result)
                    print(json.dumps(result), flush=True)
                return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":

    def interrupted(signum, frame):
        raise KeyboardInterrupt("App update interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        raise SystemExit(main())
    except MigrationRequired as error:
        print(json.dumps({"status": "administratorMigrationRequired", "error": str(error)}))
        raise SystemExit(2) from None
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "failed", "mode": "user-app-update", "error": str(error)}))
        raise SystemExit(1) from None
