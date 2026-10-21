#!/usr/bin/env python3
"""Guarded initial routing activation; no global audio reset or login registration."""

import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import plistlib
import pwd
import re
import shutil
import signal
import stat
import subprocess
import tempfile
import time
import uuid

from install_transaction import exclusive_lock, files
from install_local import service_identity
from prepare_routing_install import COMPONENTS
from routing_install import (
    AGENT,
    DAEMON,
    NAMES,
    SUPPORT,
    TARGETS,
    activate,
    preflight,
    verify_package,
)

LABEL = "system/com.lexasoft.loopz.routing"


def command(*arguments, check=True, timeout=20, input=None):
    return subprocess.run(
        list(map(str, arguments)),
        capture_output=True,
        text=True,
        check=check,
        timeout=timeout,
        input=input,
    )


def verify_code(path, identifier, requirement, engine):
    command("/usr/bin/codesign", "--verify", "--strict", "-R", "=" + requirement, path)
    details = command("/usr/bin/codesign", "-d", "--verbose=4", path).stderr
    if f"Identifier={identifier}\n" not in details or not re.search(
        r"^CodeDirectory .*flags=\S*\(runtime\)", details, re.M
    ):
        raise ValueError("Routing component identity or Hardened Runtime is invalid")
    encoded = command("/usr/bin/codesign", "-d", "--entitlements", "-", "--xml", path).stdout
    entitlements = plistlib.loads(encoded.encode()) if encoded.strip() else {}
    if entitlements != ({"com.apple.security.device.audio-input": True} if engine else {}):
        raise ValueError("Routing component has unexpected entitlements")


def executable(pid):
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    if library.proc_pidpath(pid, buffer, len(buffer)) <= 0:
        if ctypes.get_errno() == errno.ESRCH:
            return ""
        raise RuntimeError("Cannot establish a candidate Engine process identity")
    return buffer.value.decode()


class ContinuityGuard:
    def __init__(
        self, checker, work, uid, *, command_prefix=(), report_directory=None, report_owner=None
    ):
        self.report = (report_directory or work) / "guard.json"
        self.report_owner = os.geteuid() if report_owner is None else report_owner
        self.uid = uid
        self.stop_file = work / "guard.stop"
        self.stderr = (work / "guard.stderr").open("w")
        self.process = subprocess.Popen(
            [
                *command_prefix,
                str(checker),
                "--guard",
                uid,
                "--report",
                str(self.report),
                "--stop-file",
                str(self.stop_file),
                "--seconds",
                "120",
            ],
            stdout=subprocess.DEVNULL,
            stderr=self.stderr,
        )
        self.stopped = False

    def read_report(self):
        descriptor = os.open(self.report, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(descriptor, "r") as stream:
            info = os.fstat(stream.fileno())
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != self.report_owner
                or info.st_nlink != 1
                or info.st_mode & 0o022
                or info.st_size > 65536
            ):
                raise RuntimeError("Physical continuity report is not a protected regular file")
            result = json.loads(stream.read(65537))
            if not isinstance(result, dict):
                raise RuntimeError("Physical continuity report must be an object")
            if result.get("uid") != self.uid:
                raise RuntimeError("Physical continuity report identifies a different device")
            return result

    def check(self):
        if self.process.poll() is not None:
            raise RuntimeError("Physical continuity guard ended unexpectedly")

    def ready(self):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            self.check()
            if self.report.exists() and self.read_report().get("state") == "running":
                return
            time.sleep(0.05)
        raise RuntimeError("Physical continuity guard did not start")

    def stop(self):
        self.check()
        self.stop_file.touch()
        self.process.wait(timeout=5)
        self.stopped = True
        result = self.read_report()
        if (
            self.process.returncode
            or result.get("passed") is not True
            or result.get("stoppedByRequest") is not True
            or result.get("state") != "stopped"
            or type(result.get("frames")) is not int
            or result["frames"] <= 0
            or type(result.get("timestampErrors")) is not int
            or result["timestampErrors"] != 0
        ):
            raise RuntimeError("Physical continuity guard did not pass")
        return result

    def close(self):
        self.stop_file.touch()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.stderr.close()


class MacHost:
    allow_pending_app_update = False

    def __init__(self, package, guard_uid=None):
        self.package = package
        self.checker = package / "loopz-routing-install-check"
        self.guard_uid = guard_uid
        self.uid = os.stat("/dev/console").st_uid
        if not self.uid:
            raise RuntimeError("An active non-root console login is required")
        self.user = pwd.getpwuid(self.uid)
        self.domain = f"gui/{self.uid}"
        self.require_app_recovery_clear()
        self.probe_label = None
        self.probe_document = None
        self.probe_log = None

    def require_app_recovery_clear(self):
        if not self.allow_pending_app_update:
            from app_update_admission import require_no_pending_app_update

            require_no_pending_app_update(self.uid)

    def user_command(self, *arguments, **options):
        return command(
            "/bin/launchctl",
            "asuser",
            self.uid,
            "/usr/bin/sudo",
            "-n",
            "-H",
            "-u",
            f"#{self.uid}",
            *arguments,
            **options,
        )

    def snapshot(self):
        return json.loads(command(self.checker, "--snapshot").stdout)

    def require_idle(self, snapshot):
        if snapshot.get("routingDevices") or snapshot.get("routingBlockingRoutes"):
            raise RuntimeError(
                "Initial activation requires an empty routing catalog; active graph updates remain a separate migration path"
            )
        job = command(
            "/bin/launchctl", "print", f"{self.domain}/com.lexasoft.loopz.engine", check=False
        )
        if not job.returncode:
            raise RuntimeError("Stop the existing Engine user job before this initial migration")
        pids = command("/usr/bin/pgrep", "-x", "LoopZ Engine", check=False)
        for value in pids.stdout.split():
            if executable(int(value)) == str(
                TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine"
            ):
                raise RuntimeError("The installed Engine is still running")

    def service_identity(self):
        return service_identity()

    def protected_state(self):
        self.require_app_recovery_clear()
        result = {}
        for path in [
            Path("/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver"),
            SUPPORT / "loopz-broker",
            SUPPORT / "loopz-probe",
            SUPPORT / "loopz-install-check",
            Path("/Library/LaunchDaemons/com.lexasoft.loopz.bridge.plist"),
        ]:
            result[str(path)] = {
                "files": files(path),
                "inode": path.stat().st_ino if path.exists() else None,
            }
        return result

    def daemon_loaded(self):
        return command("/bin/launchctl", "print", LABEL, check=False).returncode == 0

    def stop_broker(self):
        if self.daemon_loaded():
            command("/bin/launchctl", "bootout", LABEL)

    def start_broker(self):
        command("/bin/launchctl", "bootstrap", "system", TARGETS[DAEMON])

    def transition_driver(self, transaction, guard):
        pass

    def prepare_driver_rollback(self, transaction):
        pass

    def restore_driver(self, transaction):
        pass

    def start_guard(self, work, before):
        return ContinuityGuard(self.checker, work, self.guard_uid or before["defaultOutput"])

    def latest(self):
        if not self.probe_log or not self.probe_log.exists():
            return {}
        # Only the last bounded output window is needed; partial final lines
        # are ignored until the helper finishes its next atomic report.
        with self.probe_log.open("rb") as stream:
            stream.seek(0, 2)
            length = stream.tell()
            stream.seek(max(0, length - 1024 * 1024))
            lines = stream.read().decode(errors="replace").splitlines()
        for line in reversed(lines):
            try:
                value = json.loads(line)
                if value.get("mode") == "routingDocument":
                    return value
            except (ValueError, AttributeError):
                pass
        return {}

    @staticmethod
    def configured(state):
        return (
            state.get("installationValidation") is True
            and state.get("serviceSession", 0) > 0
            and state.get("phase") == "configured"
            and state.get("desiredGeneration", 0) > 0
            and state.get("desiredGeneration") == state.get("activeGeneration")
            and state.get("render", {}).get("running")
            and state["render"].get("blocks", 0) > 0
        )

    def wait(self, predicate, seconds=30):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            state = self.latest()
            if self.configured(state) and predicate(state):
                return state
            time.sleep(0.1)
        state = self.latest()
        raise RuntimeError(
            "Routing activation timeout: "
            + json.dumps(
                {
                    key: state.get(key)
                    for key in ("phase", "configurationDetail", "serviceError", "controlError")
                }
            )
        )

    def validate_activation(self, work):
        folder = work / "probe"
        folder.mkdir(mode=0o700)
        os.chown(folder, self.uid, self.user.pw_gid)
        self.probe_document = folder / "routes.json"
        self.probe_log = folder / "engine.jsonl"
        document = json.loads(
            self.user_command(
                self.package / "loopz-config",
                "create",
                self.probe_document,
                "LoopZ installation validation",
            ).stdout
        )
        identity = document["devices"][0]["id"]
        uid = "com.lexasoft.loopz.device." + identity
        self.probe_label = "com.lexasoft.loopz.install-probe." + uuid.uuid4().hex
        job = work / "probe.plist"
        job.write_bytes(
            plistlib.dumps(
                {
                    "Label": self.probe_label,
                    "ProgramArguments": [
                        str(TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine"),
                        "--routing-document",
                        str(self.probe_document),
                        "--installation-validation",
                        "true",
                        "--seconds",
                        "90",
                    ],
                    "RunAtLoad": True,
                    "ProcessType": "Interactive",
                    "StandardOutPath": str(self.probe_log),
                    "StandardErrorPath": str(folder / "engine.stderr"),
                }
            )
        )
        job.chmod(0o644)
        self.user_command("/bin/launchctl", "bootstrap", self.domain, job)
        state = self.wait(
            lambda state: len(state.get("devices", [])) == 1
            and state["devices"][0].get("uid") == uid
        )
        snapshot = self.snapshot()
        entries = snapshot.get("routingDevices", [])
        expected = {
            "uid": uid,
            "alive": True,
            "running": False,
            "sampleRate": 48000,
            "inputChannels": 2,
            "outputChannels": 2,
            "inputLatencyFrames": 129,
        }
        if entries != [expected]:
            raise RuntimeError("Published routing endpoint differs from its graph/timing plan")
        return {
            "publicDevice": entries[0],
            "helper": state,
            "captureOpened": False,
            "physicalMonitorsOpened": False,
            "source": "actual Core Audio catalog and authenticated helper acknowledgement",
        }

    def activation_diagnostics(self):
        result = {"helper": self.latest(), "audioCatalog": self.snapshot()}
        if self.probe_log:
            path = self.probe_log.parent / "engine.stderr"
            if path.is_file():
                with path.open("rb") as stream:
                    stream.seek(0, 2)
                    stream.seek(max(0, stream.tell() - 16384))
                    result["helperStderr"] = stream.read().decode(errors="replace")
        job = command("/bin/launchctl", "print", LABEL, check=False)
        result["brokerJobPresent"] = job.returncode == 0
        return result

    def cleanup_validation(self):
        if not self.probe_label:
            return
        label = self.probe_label
        loaded = (
            self.user_command(
                "/bin/launchctl", "print", f"{self.domain}/{label}", check=False
            ).returncode
            == 0
        )
        error = None
        try:
            if loaded and self.snapshot().get("routingDevices"):
                self.user_command(
                    self.package / "loopz-config",
                    "write",
                    self.probe_document,
                    input='{"version":2,"devices":[]}',
                )
                self.wait(lambda state: state.get("devices") == [], seconds=15)
                if self.snapshot().get("routingDevices"):
                    raise RuntimeError("Validation endpoint is still published after withdrawal")
        except Exception as failure:
            error = failure
        finally:
            if loaded:
                self.user_command("/bin/launchctl", "bootout", f"{self.domain}/{label}")
            self.probe_label = None
        if error:
            raise error


def private_snapshot(source, work):
    folder = work / "package"
    folder.mkdir(mode=0o700)
    for name in (*NAMES, "manifest.json"):
        path = source / name
        if path.is_dir():
            shutil.copytree(path, folder / name, symlinks=True)
        else:
            shutil.copy2(path, folder / name, follow_symlinks=False)
    files(folder)
    for path in folder.rglob("*"):
        os.chmod(path, 0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644)
    return folder


def check_support():
    for target in (
        SUPPORT,
        SUPPORT / "routing",
        Path("/Library/Audio/Plug-Ins/HAL"),
        Path("/Library/LaunchDaemons"),
    ):
        for path in (target, *target.parents):
            if path.is_symlink():
                raise RuntimeError("Symlink in the routing service installation path")
            if path.exists():
                info = path.stat()
                if info.st_uid != 0 or info.st_mode & 0o022:
                    raise RuntimeError("Routing service parent must be root-owned and protected")


def write_result(result):
    path = SUPPORT / "routing-install-result.json"
    if path.is_symlink():
        raise RuntimeError("Refusing a linked routing result path")
    with tempfile.NamedTemporaryFile(
        mode="w", prefix=".routing-result-", dir=SUPPORT, delete=False
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


def main(*, legacy_slot=False):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--manifest-sha")
    parser.add_argument("--replace-engine", action="store_true")
    parser.add_argument("--guard-uid")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    targets = TARGETS
    host_class = MacHost
    if legacy_slot:
        from routing_migration import LegacySlotHost, MIGRATION_TARGETS

        targets = MIGRATION_TARGETS
        host_class = LegacySlotHost
    source = args.package.absolute()
    check_support()
    if args.check and os.geteuid() == 0:
        parser.error(
            "Run --check as the console user; installation uses a private verified root snapshot"
        )
    manifest, digest = verify_package(
        source, verify_code, args.manifest_sha, legacy_slot=legacy_slot
    )
    if args.check:
        names = preflight(source, targets, args.replace_engine, legacy_slot=legacy_slot)
        host = host_class(source, args.guard_uid)
        before = host.snapshot()
        host.require_idle(before)
        print(
            json.dumps(
                {
                    "status": "ready",
                    "manifestSHA256": digest,
                    "replace": names,
                    "before": before,
                    "coreAudio": host.service_identity(),
                    "activationVerified": False,
                    "loginRegistered": False,
                    "existingSlotMigration": legacy_slot,
                    "rootHostInspectionRequired": legacy_slot,
                }
            )
        )
        return
    if os.geteuid() != 0 or not args.manifest_sha:
        parser.error(
            "Installation requires administrator privileges and the reviewed --manifest-sha"
        )
    for path in (SUPPORT, SUPPORT / "routing"):
        if not path.exists():
            path.mkdir(mode=0o755)
            path.chmod(0o755)
    check_support()
    with exclusive_lock(SUPPORT / ".install.lock"):
        with tempfile.TemporaryDirectory(
            prefix="loopz-routing-install-", dir="/private/tmp"
        ) as temporary:
            work = Path(temporary).resolve()
            work.chmod(0o711)
            package = private_snapshot(source, work)
            manifest, digest = verify_package(
                package, verify_code, args.manifest_sha, legacy_slot=legacy_slot
            )
            package.chmod(0o755)  # Verified public code only; console helper needs traversal.
            names = preflight(package, targets, args.replace_engine, legacy_slot=legacy_slot)
            result = activate(package, targets, names, host_class(package, args.guard_uid), work)
            result["manifestSHA256"] = digest
            write_result(result)
            print(json.dumps(result), flush=True)
            if result["status"] != "passed":
                raise SystemExit(1)


if __name__ == "__main__":

    def interrupted(signum, frame):
        raise KeyboardInterrupt("Routing installation interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from None
