"""Manage only the private diagnostic workspace named by a removal journal."""

import os
from pathlib import Path
import stat
import tempfile
import time

from desktop_files import identity, remove_at
from install_routing import command, executable
from uninstall_files import open_directory

CHECKER = "loopz-removal-check"
CONTENTS = {CHECKER, "guard.json", "guard.stop", "guard.stderr"}


def diagnostic_processes(path):
    """No PID signals: verify matching kernel executable paths, then wait."""
    result = command("/bin/ps", "-axo", "pid=,comm=")
    found = []
    for line in result.stdout.splitlines():
        parts = line.strip().split(maxsplit=1)
        if len(parts) == 2 and parts[1] == str(path):
            pid = int(parts[0])
            observed = executable(pid)
            if observed and observed != str(path):
                raise RuntimeError("Diagnostic process path changed during inspection")
            if observed:
                found.append(pid)
    return found


class DiagnosticWorkspaces:
    def __init__(
        self,
        root=Path("/private/tmp"),
        owner=None,
        processes=diagnostic_processes,
        report_owner=None,
    ):
        self.root = Path(root)
        self.owner = os.geteuid() if owner is None else owner
        self.processes = processes
        self.report_owner = report_owner

    def create(self):
        return Path(tempfile.mkdtemp(prefix="loopz-uninstall-", dir=self.root))

    def describe(self, path):
        descriptor = open_directory(path)
        try:
            info = os.fstat(descriptor)
            value = {"path": str(path), "device": info.st_dev, "inode": info.st_ino}
        finally:
            os.close(descriptor)
        return value

    def open(self, value):
        path = Path(value["path"])
        if (
            path.parent != self.root
            or not path.name.startswith("loopz-uninstall-")
            or ".." in path.parts
        ):
            raise ValueError("Diagnostic workspace is outside its fixed namespace")
        try:
            descriptor = open_directory(path)
        except FileNotFoundError:
            return None
        info = os.fstat(descriptor)
        if (
            identity(info) != (value["device"], value["inode"])
            or info.st_uid != self.owner
            or info.st_mode & 0o077
        ):
            os.close(descriptor)
            raise RuntimeError("Diagnostic workspace identity or privacy changed")
        return descriptor

    def quiesce(self, value, *, request_stop):
        path = Path(value["path"])
        descriptor = self.open(value)
        try:
            if descriptor is not None and request_stop:
                try:
                    stop = os.open(
                        "guard.stop",
                        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                        0o600,
                        dir_fd=descriptor,
                    )
                except FileExistsError:
                    info = os.stat("guard.stop", dir_fd=descriptor, follow_symlinks=False)
                    if (
                        not stat.S_ISREG(info.st_mode)
                        or info.st_uid != self.owner
                        or info.st_nlink != 1
                        or info.st_mode & 0o022
                    ):
                        raise RuntimeError(
                            "Diagnostic stop request is not a protected regular file"
                        )
                else:
                    try:
                        os.fsync(stop)
                    finally:
                        os.close(stop)
                    os.fsync(descriptor)
            deadline = time.monotonic() + 5
            while self.processes(path / CHECKER):
                if descriptor is None or not request_stop or time.monotonic() >= deadline:
                    raise RuntimeError(
                        "The owned diagnostic is still running; its bounded exit must complete before cleanup"
                    )
                time.sleep(0.05)
        finally:
            if descriptor is not None:
                os.close(descriptor)

    def validate_reports(self, descriptor):
        reports = os.open(
            "hardware-guard", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor
        )
        try:
            info = os.fstat(reports)
            if (
                self.report_owner is None
                or info.st_uid != self.report_owner
                or info.st_mode & 0o077
            ):
                raise RuntimeError("Diagnostic reports have an unexpected owner or permissions")
            for name in os.listdir(reports):
                info = os.stat(name, dir_fd=reports, follow_symlinks=False)
                if (
                    name != "guard.json"
                    or not stat.S_ISREG(info.st_mode)
                    or info.st_uid != self.report_owner
                    or info.st_mode & 0o022
                    or info.st_nlink != 1
                ):
                    raise RuntimeError("Foreign or unsafe diagnostic report is retained")
        finally:
            os.close(reports)

    def clean(self, value):
        descriptor = self.open(value)
        if descriptor is None:
            if self.processes(Path(value["path"]) / CHECKER):
                raise RuntimeError("A diagnostic remains active after its workspace disappeared")
            return
        try:
            if self.processes(Path(value["path"]) / CHECKER):
                raise RuntimeError("Diagnostic workspace is still in use")
            for name in os.listdir(descriptor):
                info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
                if name == "hardware-guard" and stat.S_ISDIR(info.st_mode):
                    self.validate_reports(descriptor)
                    continue
                if (
                    name not in CONTENTS
                    or not stat.S_ISREG(info.st_mode)
                    or info.st_uid != self.owner
                    or info.st_mode & 0o022
                    or info.st_nlink != 1
                ):
                    raise RuntimeError("Foreign or unsafe diagnostic workspace content is retained")
        finally:
            os.close(descriptor)
        path = Path(value["path"])
        parent = open_directory(path.parent)
        try:
            remove_at(parent, path.name, (value["device"], value["inode"]))
            os.fsync(parent)
        finally:
            os.close(parent)
