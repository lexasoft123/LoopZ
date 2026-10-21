#!/usr/bin/env python3
"""Deploy a signed desktop/Engine pair without changing HAL, broker or login state."""

import argparse
from functools import partial
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import tempfile
import time

from application_ownership import application_owners
from desktop_files import DesktopFileTransaction
from desktop_install import (
    IDENTIFIERS,
    NAMES,
    TARGETS,
    deploy,
    requirement,
    validate_runtime_capabilities,
    verify_package,
)
from install_local import service_identity
from install_routing import ContinuityGuard, MacHost, command, executable, verify_code
from install_transaction import blocked_signals, check_ancestors, exclusive_lock, files
from routing_install import SUPPORT

CHECKER = SUPPORT / "loopz-routing-install-check"
CONFIG = SUPPORT / "loopz-config"


def protected_file(path, *, directory=False):
    check_ancestors([path])
    info = path.lstat()
    shape = stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)
    if not shape or info.st_uid != 0 or info.st_mode & 0o022:
        raise RuntimeError("Installed LoopZ support files must be root-owned and protected")


def job_generation(label):
    result = command("/bin/launchctl", "print", label, check=False)
    if result.returncode:
        raise RuntimeError("The installed routing broker must already be running")
    matched = re.search(r"^\s*pid = (\d+)\s*$", result.stdout, re.M)
    if not matched:
        raise RuntimeError("A broker job has no live process")
    return command("/bin/ps", "-p", matched[1], "-o", "pid=,lstart=,command=").stdout.strip()


class DesktopHost(MacHost):
    def __init__(self, guard_uid=None):
        # Reuse the installed, protected configuration and continuity tools.
        # The desktop package contains only the two application bundles.
        super().__init__(SUPPORT, guard_uid)
        for path in (SUPPORT,):
            protected_file(path, directory=True)
        for path, identifier in (
            (CHECKER, "com.lexasoft.loopz.routing.install-check"),
            (CONFIG, "com.lexasoft.loopz.config"),
        ):
            protected_file(path)
            verify_code(path, identifier, requirement(identifier), False)
        self.checker = CHECKER

    def start_guard(self, work, before):
        # Audio IO belongs to the console session, like the Engine itself.
        # Only the report directory is user-owned. The stop request remains
        # in the administrator-owned workspace, outside that writable tree.
        reports = work / "hardware-guard"
        reports.mkdir(mode=0o700)
        prefix = ()
        if os.geteuid() == 0:
            os.chown(reports, self.uid, self.user.pw_gid)
            prefix = (
                "/bin/launchctl",
                "asuser",
                str(self.uid),
                "/usr/bin/sudo",
                "-n",
                "-H",
                "-u",
                f"#{self.uid}",
            )
        elif os.geteuid() != self.uid:
            raise RuntimeError("The continuity check requires the console user")
        return ContinuityGuard(
            self.checker,
            work,
            self.guard_uid or before["defaultOutput"],
            command_prefix=prefix,
            report_directory=reports,
            report_owner=self.uid,
        )

    def user_command(self, *arguments, **options):
        if os.getuid() == self.uid and os.geteuid() == self.uid:
            return command(*arguments, **options)
        return super().user_command(*arguments, **options)

    def require_idle(self, snapshot):
        if snapshot.get("routingDevices") or snapshot.get("routingBlockingRoutes"):
            raise RuntimeError("This desktop deployment requires a stopped, empty routing graph")
        for label in ("com.lexasoft.loopz.engine", "com.lexasoft.loopz.session"):
            job = self.user_command(
                "/bin/launchctl", "print", f"{self.domain}/{label}", check=False
            )
            if not job.returncode:
                raise RuntimeError("Stop the existing LoopZ background session before deployment")
            if (
                job.returncode not in (3, 113)
                or "Could not find service" not in job.stderr + job.stdout
            ):
                raise RuntimeError("The current background session could not be inspected")
        for name, suffix in (
            ("LoopZ Engine", "/LoopZ Engine.app/Contents/MacOS/LoopZ Engine"),
            ("LoopZ", "/LoopZ.app/Contents/MacOS/LoopZ"),
        ):
            processes = command("/usr/bin/pgrep", "-x", name, check=False)
            if processes.returncode not in (0, 1):
                raise RuntimeError("Running LoopZ applications could not be inspected")
            for value in processes.stdout.split():
                if executable(int(value)).endswith(suffix):
                    raise RuntimeError("Quit LoopZ and stop its Engine before deployment")

    def protected_state(self):
        self.require_app_recovery_clear()
        paths = [
            Path("/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver"),
            Path("/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver"),
            SUPPORT / "loopz-routing-broker",
            SUPPORT / "routing-service.json",
            Path("/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist"),
            SUPPORT / "loopz-broker",
            SUPPORT / "loopz-probe",
            SUPPORT / "loopz-install-check",
            Path("/Library/LaunchDaemons/com.lexasoft.loopz.bridge.plist"),
            CHECKER,
            CONFIG,
        ]
        state = {}
        for path in paths:
            check_ancestors([path])
            state[str(path)] = {
                "files": files(path),
                "inode": path.stat().st_ino if path.exists() else None,
            }
        state["routingBroker"] = job_generation("system/com.lexasoft.loopz.routing")
        # Do not follow user-controlled links while running as administrator.
        support = Path(self.user.pw_dir) / "Library/Application Support/LoopZ"
        check_ancestors([support])
        if support.exists():
            info = support.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != self.uid or info.st_mode & 0o022:
                raise RuntimeError("The user routing directory has unsafe ownership or permissions")
        state["userSupportDirectoryExists"] = support.exists()
        for name in (
            "routes.json",
            "routes.json.backup",
            "routes.json.audio-settings.json",
            "routes.json.audio-settings.json.backup",
            "Session/engine.plist",
            "Session/session.lock",
        ):
            path = support / name
            check_ancestors([path])
            if path.exists():
                info = path.lstat()
                if (
                    not stat.S_ISREG(info.st_mode)
                    or info.st_uid != self.uid
                    or info.st_mode & 0o022
                    or info.st_nlink != 1
                    or info.st_size > 16 * 1024 * 1024
                ):
                    raise RuntimeError("A user routing file is not a protected regular file")
                state[str(path)] = {"files": files(path), "inode": info.st_ino}
            else:
                state[str(path)] = None
        return state

    def existing_apps(self, package, replace_engine, replace_editor):
        for name in NAMES:
            path = TARGETS[name]
            if path.exists():
                check_ancestors([path])
                verify_code(
                    path,
                    IDENTIFIERS[name],
                    requirement(IDENTIFIERS[name]),
                    name == "LoopZ Engine.app",
                )
                allowed = replace_engine if name == "LoopZ Engine.app" else replace_editor
                if files(path) != files(package / name) and not allowed:
                    raise RuntimeError(
                        "Replacing an installed app requires its explicit replace flag"
                    )
        if TARGETS["LoopZ.app"].exists():
            # A registered login agent needs a separate update/re-registration
            # transaction. Initial deployment must not silently disable it.
            state = self.app_status()
            if state["login"]["registered"]:
                raise RuntimeError(
                    "An enabled login helper requires the forthcoming active-update path"
                )

    def app_status(self):
        result = self.user_command(
            TARGETS["LoopZ.app"] / "Contents/MacOS/LoopZ", "--background-status"
        )
        status = json.loads(result.stdout)
        if (
            status.get("version") != 1
            or not isinstance(status.get("login"), dict)
            or type(status["login"].get("registered")) is not bool
        ):
            raise RuntimeError("Installed desktop login state could not be established")
        return status

    def validate_apps(self):
        self.user_command(TARGETS["LoopZ.app"] / "Contents/MacOS/LoopZ", "--verify-interface")
        capabilities = json.loads(
            self.user_command(
                TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine",
                "--runtime-capabilities",
            ).stdout
        )
        validate_runtime_capabilities(capabilities)
        status = self.app_status()
        if not status["login"].get("available") or status["login"]["registered"]:
            raise RuntimeError(
                "Login startup changed or the desktop is not available at its installed location"
            )
        return {"engine": capabilities, "desktop": status, "interfaceSealVerified": True}

    def cleanup_validation(self):
        label = self.probe_label
        try:
            super().cleanup_validation()
        except BaseException:
            self.probe_label = label
            raise
        if label:
            end = time.monotonic() + 10
            while time.monotonic() < end:
                if self.user_command(
                    "/bin/launchctl", "print", f"{self.domain}/{label}", check=False
                ).returncode:
                    return
                time.sleep(0.1)
            self.probe_label = label
            raise RuntimeError("The temporary validation job did not unload")

    def activation_diagnostics(self):
        return {
            **super().activation_diagnostics(),
            "validationJob": self.probe_label,
            "validationDocument": str(self.probe_document) if self.probe_document else None,
        }


def private_snapshot(source, work):
    package = work / "package"
    package.mkdir(mode=0o700)
    for name in (*NAMES, "manifest.json"):
        path = source / name
        if path.is_dir():
            shutil.copytree(path, package / name, symlinks=True)
        else:
            shutil.copy2(path, package / name, follow_symlinks=False)
    return package


def write_result(result):
    path = SUPPORT / "desktop-install-result.json"
    check_ancestors([path])
    with tempfile.NamedTemporaryFile(
        mode="w", prefix=".desktop-result-", dir=SUPPORT, delete=False
    ) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(json.dumps(result, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
            os.fchmod(stream.fileno(), 0o644)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


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
    manifest, digest = verify_package(package, verify_code, args.manifest_sha)
    if args.check and os.geteuid() == 0:
        parser.error("Run --check as the console user")
    if not args.check and (os.geteuid() != 0 or not args.manifest_sha):
        parser.error(
            "Deployment requires administrator authorization and the reviewed --manifest-sha"
        )
    host = DesktopHost(args.guard_uid)
    if os.stat("/private/tmp").st_dev != os.stat("/Applications").st_dev:
        raise RuntimeError("Atomic deployment requires the standard application filesystem")
    host.existing_apps(package, args.replace_engine, args.replace_editor)
    host.require_idle(host.snapshot())
    host.protected_state()
    if args.check:
        print(
            json.dumps(
                {
                    "status": "ready",
                    "manifestSHA256": digest,
                    "targets": {name: str(path) for name, path in TARGETS.items()},
                    "driverPreserved": True,
                    "brokerPreserved": True,
                    "loginRegistered": False,
                    "deploymentVerified": False,
                }
            )
        )
        return
    with exclusive_lock(SUPPORT / ".install.lock", readable=True):
        with tempfile.TemporaryDirectory(
            prefix="loopz-desktop-install-", dir="/private/tmp"
        ) as temporary:
            work = Path(temporary).resolve()
            work.chmod(0o711)
            private = private_snapshot(package, work)
            manifest, digest = verify_package(private, verify_code, args.manifest_sha)
            host.existing_apps(private, args.replace_engine, args.replace_editor)
            result = deploy(
                private,
                TARGETS,
                host,
                work,
                transaction_factory=partial(
                    DesktopFileTransaction, owners=application_owners(host.uid, host.user.pw_gid)
                ),
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
        raise KeyboardInterrupt("Desktop deployment interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from None
