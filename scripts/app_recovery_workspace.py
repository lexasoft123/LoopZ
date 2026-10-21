"""Cooperative guard shutdown and bounded cleanup of the recorded update workspace."""

import os
import re
import stat
import time

from app_recovery_state import CHECKER
from desktop_files import identity, remove_at
from desktop_install import requirement
from install_routing import command
from process_code import process_generation, require_process_code, same_generation
from process_record import validate_process
from suite_workspace import stop_request
from uninstall_files import open_directory
from uninstall_workspace import diagnostic_processes

CHECKER_REQUIREMENT = requirement("com.lexasoft.loopz.routing.install-check")


class AppRecoveryWorkspace:
    def __init__(
        self,
        state,
        *,
        processes=diagnostic_processes,
        generation=process_generation,
        dynamic_code=require_process_code,
        run=command,
    ):
        self.state = state
        self.processes, self.generation, self.dynamic_code, self.run = (
            processes,
            generation,
            dynamic_code,
            run,
        )

    def guard_processes(self):
        result = []
        stop_argument = " --stop-file " + str(self.state.work / "guard.stop") + " --seconds 120"
        for pid in self.processes(CHECKER):
            try:
                before = self.generation(pid)
                if before["uid"] != self.state.owner or before["executable"] != str(CHECKER):
                    continue
                arguments = self.run("/bin/ps", "-ww", "-p", pid, "-o", "command=", check=False)
                if arguments.returncode or not arguments.stdout.strip().endswith(stop_argument):
                    continue
                validate_process(before, owner=self.state.owner, executable=CHECKER)
                self.dynamic_code(pid, CHECKER_REQUIREMENT)
                if not same_generation(self.generation(pid, self.state.owner), before):
                    raise RuntimeError(
                        "The continuity guard generation changed during inspection"
                    )
                result.append(before)
            except ProcessLookupError:
                continue
        return result

    def quiesce(self):
        descriptor = self.state.open_work()
        if descriptor is None:
            if self.guard_processes():
                raise RuntimeError("A guard remains active after its workspace disappeared")
            return
        os.close(descriptor)
        observed = self.guard_processes()
        recorded = self.state.value["guard"]
        if recorded is not None:
            try:
                current = self.generation(recorded["pid"])
            except ProcessLookupError:
                current = None
            if current is not None and same_generation(recorded, current):
                if not any(same_generation(recorded, process) for process in observed):
                    raise RuntimeError(
                        "The recorded guard is not bound to this update workspace"
                    )
        # This fixed owned stop file also covers a crash just after spawning the
        # guard but before its PID can be recorded. No PID signal is sent.
        stop_request(self.state.work, self.state.owner)
        until = time.monotonic() + 5
        quiet_since = None
        while time.monotonic() < until:
            if self.guard_processes():
                quiet_since = None
            elif quiet_since is None:
                quiet_since = time.monotonic()
            elif time.monotonic() - quiet_since >= 0.2:
                return
            time.sleep(0.05)
        raise RuntimeError("The owned continuity guard did not stop; recovery remains pending")

    def clean(self):
        directory = self.state.open_work()
        if directory is None:
            return
        count, total = 0, 0

        def inspect(parent, name, depth=0):
            nonlocal count, total
            count += 1
            if count > 8192 or depth > 24:
                raise ValueError("The recovery workspace exceeds its directory bound")
            info = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if info.st_uid != self.state.owner or info.st_mode & 0o022:
                raise RuntimeError("Workspace ownership or permissions changed")
            if stat.S_ISDIR(info.st_mode):
                child = os.open(
                    name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent
                )
                try:
                    if identity(os.fstat(child)) != identity(info):
                        raise RuntimeError("The workspace changed during inspection")
                    for entry in os.listdir(child):
                        inspect(child, entry, depth + 1)
                finally:
                    os.close(child)
            elif stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
                total += info.st_size
                if info.st_size > 64 * 1024 * 1024 or total > 1024 * 1024 * 1024:
                    raise ValueError("The recovery workspace exceeds its payload bound")
            else:
                raise RuntimeError("Unsafe workspace content is retained for inspection")

        try:
            if self.guard_processes():
                raise RuntimeError("A live continuity guard still owns the workspace")
            for name in os.listdir(directory):
                if name not in {
                    "package",
                    "hardware-guard",
                    "guard.stop",
                    "guard.stderr",
                } and not re.fullmatch(r"prepared-apps-[a-z0-9_]+", name):
                    raise RuntimeError("Foreign workspace content is retained for inspection")
                inspect(directory, name)
            expected = identity(os.fstat(directory))
        finally:
            os.close(directory)
        parent = open_directory(self.state.work.parent)
        try:
            remove_at(parent, self.state.work.name, expected)
            os.fsync(parent)
        finally:
            os.close(parent)
