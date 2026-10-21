"""Own only a recorded private suite diagnostic workspace and cooperative guards."""

import os
from pathlib import Path
import stat
import tempfile
import time

from desktop_files import identity, remove_at
from install_desktop import requirement
from process_code import process_generation, require_process_code, same_generation
from process_record import validate_process
from suite_artifacts import validate_artifacts
from uninstall_files import open_directory
from uninstall_workspace import diagnostic_processes

ROOT = Path("/private/tmp")
CHECKER_NAME = "loopz-suite-check"
CHECKER_ID = "com.lexasoft.loopz.routing.install-check"
CHECKER_REQUIREMENT = requirement(CHECKER_ID)
GUARD_FILES = {CHECKER_NAME, "guard.json", "guard.stop", "guard.stderr"}


def stop_request(folder, owner):
    directory = open_directory(folder)
    try:
        try:
            descriptor = os.open(
                "guard.stop",
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                0o600,
                dir_fd=directory,
            )
        except FileExistsError:
            info = os.stat("guard.stop", dir_fd=directory, follow_symlinks=False)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != owner
                or info.st_nlink != 1
                or info.st_mode & 0o022
            ):
                raise RuntimeError("The guard stop request is not a protected regular file")
        else:
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            os.fsync(directory)
    finally:
        os.close(directory)


class SuiteWorkspaces:
    def __init__(
        self,
        *,
        owner=None,
        processes=diagnostic_processes,
        generation=process_generation,
        dynamic_code=require_process_code,
    ):
        self.owner = os.geteuid() if owner is None else owner
        self.processes, self.generation, self.dynamic_code = processes, generation, dynamic_code

    def create(self):
        path = Path(tempfile.mkdtemp(prefix="loopz-suite-run-", dir=ROOT))
        path.chmod(0o711)
        return path

    def describe(self, path):
        descriptor = open_directory(path)
        try:
            info = os.fstat(descriptor)
            value = {"path": str(path), "device": info.st_dev, "inode": info.st_ino}
        finally:
            os.close(descriptor)
        validate_artifacts({"work": value, "guards": [], "probe": None}, self.owner)
        descriptor = self.open(value)
        if descriptor is None:
            raise RuntimeError("The suite diagnostic workspace disappeared")
        os.close(descriptor)
        return value

    def open(self, value):
        validate_artifacts({"work": value, "guards": [], "probe": None}, self.owner)
        try:
            descriptor = open_directory(value["path"])
        except FileNotFoundError:
            return None
        info = os.fstat(descriptor)
        if (
            identity(info) != (value["device"], value["inode"])
            or info.st_uid != self.owner
            or stat.S_IMODE(info.st_mode) != 0o711
        ):
            os.close(descriptor)
            raise RuntimeError("The suite diagnostic workspace identity or permissions changed")
        return descriptor

    def guard_processes(self, path):
        result = []
        for pid in self.processes(path):
            before = self.generation(pid, self.owner)
            validate_process(before, owner=self.owner, executable=path)
            self.dynamic_code(pid, CHECKER_REQUIREMENT)
            if self.generation(pid, self.owner) != before:
                raise RuntimeError("A guard process changed during its running-code audit")
            result.append(before)
        return result

    def recorded_alive(self, process):
        if process is None:
            return False
        try:
            current = self.generation(process["pid"])
        except ProcessLookupError:
            return False
        return same_generation(current, process)

    def guard_folder(self, workspace, name):
        descriptor = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=workspace)
        info = os.fstat(descriptor)
        if info.st_uid != self.owner or stat.S_IMODE(info.st_mode) != 0o700:
            os.close(descriptor)
            raise RuntimeError("A continuity guard directory is not private and owned")
        return descriptor

    def quiesce_guards(self, artifacts):
        validate_artifacts(artifacts, self.owner)
        if artifacts["work"] is None:
            return
        workspace = self.open(artifacts["work"])
        try:
            for guard in artifacts["guards"]:
                path = Path(artifacts["work"]["path"]) / guard["folder"]
                try:
                    folder = (
                        self.guard_folder(workspace, guard["folder"])
                        if workspace is not None
                        else None
                    )
                except FileNotFoundError:
                    folder = None
                try:
                    running = self.guard_processes(path / CHECKER_NAME)
                    if folder is None and (running or self.recorded_alive(guard["process"])):
                        raise RuntimeError(
                            "An owned guard is running without its recorded directory"
                        )
                    if folder is not None:
                        stop_request(path, self.owner)
                    deadline = time.monotonic() + 5
                    while running or self.recorded_alive(guard["process"]):
                        if time.monotonic() >= deadline:
                            raise RuntimeError(
                                "The owned guard did not stop cooperatively; recovery retained"
                            )
                        time.sleep(0.05)
                        running = self.guard_processes(path / CHECKER_NAME)
                finally:
                    if folder is not None:
                        os.close(folder)
        finally:
            if workspace is not None:
                os.close(workspace)

    def clean(self, artifacts):
        validate_artifacts(artifacts, self.owner)
        if artifacts["work"] is None:
            return
        if artifacts["probe"] is not None:
            raise RuntimeError(
                "Withdraw and remove the owned validation job before workspace cleanup"
            )
        value = artifacts["work"]
        directory = self.open(value)
        allowed = {"package", "assembled", *[g["folder"] for g in artifacts["guards"]]}
        entries = 0
        total = 0

        def inspect(parent, name, depth=0):
            nonlocal entries, total
            entries += 1
            if depth > 64 or entries > 8192:
                raise RuntimeError("Suite workspace exceeds its bounded cleanup inventory")
            info = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if info.st_uid != self.owner or info.st_mode & 0o022:
                raise RuntimeError("Foreign ownership or permissions in the suite workspace")
            if stat.S_ISDIR(info.st_mode):
                if depth == 0 and name.startswith("guard-") and stat.S_IMODE(info.st_mode) != 0o700:
                    raise RuntimeError("A guard directory lost its private permissions")
                child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
                try:
                    if identity(os.fstat(child)) != identity(info):
                        raise RuntimeError("A suite cleanup directory changed")
                    names = os.listdir(child)
                    if depth == 0 and name.startswith("guard-") and set(names) - GUARD_FILES:
                        raise RuntimeError("Foreign guard contents are retained for inspection")
                    for entry in names:
                        inspect(child, entry, depth + 1)
                finally:
                    os.close(child)
            elif stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
                total += info.st_size
                if info.st_size > 64 * 1024 * 1024 or total > 512 * 1024 * 1024:
                    raise RuntimeError("Suite workspace exceeds its bounded payload inventory")
            else:
                raise RuntimeError("Suite workspace cleanup rejects links and special files")

        try:
            for guard in artifacts["guards"]:
                if self.guard_processes(
                    Path(value["path"]) / guard["folder"] / CHECKER_NAME
                ) or self.recorded_alive(guard["process"]):
                    raise RuntimeError("The suite workspace still has an active guard")
            if directory is None:
                return
            names = os.listdir(directory)
            if set(names) - allowed:
                raise RuntimeError("Foreign suite workspace contents are retained")
            for name in names:
                if not stat.S_ISDIR(os.stat(name, dir_fd=directory, follow_symlinks=False).st_mode):
                    raise RuntimeError("A suite workspace folder was replaced by a non-directory")
                inspect(directory, name)
        finally:
            if directory is not None:
                os.close(directory)
        parent = open_directory(ROOT)
        try:
            remove_at(parent, Path(value["path"]).name, (value["device"], value["inode"]))
            os.fsync(parent)
        finally:
            os.close(parent)

    def attach(self, context, path):
        value = self.describe(path)
        old = context.value["artifacts"]
        if old["work"] == value:
            return
        if old["work"] is not None:
            self.quiesce_guards(old)
            self.clean(old)
        context.artifacts({"work": value, "guards": [], "probe": None})
