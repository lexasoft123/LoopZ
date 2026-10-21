"""Temporary per-transaction recovery LaunchAgent; unrelated login choice is untouched."""

import json
import os
from pathlib import Path
import plistlib
import re
import stat
import tempfile
import time

from app_login_session import login_session
from app_recovery_state import AppRecoveryState, RecoveryReady, validate_hex
from app_recovery_tools import require_private_directory, stage_tools, verify_tools
from app_recovery_workspace import AppRecoveryWorkspace
from app_update_journal import AppUpdateJournal
from desktop_files import identity
from desktop_install import TARGETS
from install_routing import command
from process_code import process_generation, require_process_code, same_generation
from uninstall_files import open_directory, rename_exclusive

PREFIX = "com.lexasoft.loopz.update-recovery."


def agent_paths(updates, token):
    validate_hex(token, 32)
    updates = Path(updates)
    if tuple(updates.parts[-4:]) != ("Library", "Application Support", "LoopZ", "Updates"):
        raise ValueError("Recovery jobs require the fixed account update directory")
    home = updates.parents[3]
    return (
        home / "Library/LaunchAgents" / (PREFIX + token + ".plist"),
        updates / "RecoveryJobs" / (token + ".permit"),
    )


def agent_definition(updates, token, tools):
    validate_hex(tools, 64)
    _, permit = agent_paths(updates, token)
    bootstrap = Path(updates) / "RecoveryTools" / tools / "bootstrap_app_recovery.py"
    # Apple's launchd PathState keeps only this transaction's repair job alive.
    # https://github.com/apple-oss-distributions/launchd/blob/main/man/launchd.plist.5
    return {
        "Label": PREFIX + token,
        "ProgramArguments": [
            "/usr/bin/python3",
            "-B",
            "-E",
            "-s",
            "-S",
            str(bootstrap),
            tools,
            token,
        ],
        "RunAtLoad": True,
        "KeepAlive": {"PathState": {str(permit): True}},
        "ThrottleInterval": 10,
        "ProcessType": "Background",
        "StandardOutPath": str(permit.with_suffix(".stdout.log")),
        "StandardErrorPath": str(permit.with_suffix(".stderr.log")),
    }


def read_owned(path, owner, limit=32768):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(descriptor)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_uid != owner
            or before.st_mode & 0o077
            or before.st_nlink != 1
            or before.st_size > limit
        ):
            raise RuntimeError(
                "Recovery job metadata is not private, regular and account-owned"
            )
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            encoded = stream.read(limit + 1)
        if len(encoded) > limit or identity(os.fstat(descriptor)) != identity(before):
            raise RuntimeError("Recovery job metadata changed during inspection")
        return encoded, identity(before)
    finally:
        os.close(descriptor)


def publish(path, encoded, owner):
    directory = open_directory(path.parent)
    try:
        with tempfile.NamedTemporaryFile(
            dir=path.parent, prefix=".recovery-", delete=False
        ) as stream:
            temporary = Path(stream.name)
            try:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(encoded)
                stream.flush()
                os.fsync(stream.fileno())
                rename_exclusive(directory, directory, temporary.name, path.name)
                os.fsync(directory)
            finally:
                temporary.unlink(missing_ok=True)
    finally:
        os.close(directory)
    read_owned(path, owner)


def job_state(updates, token, tools, *, run=command):
    path, _ = agent_paths(updates, token)
    expected = agent_definition(updates, token, tools)
    if path.exists() or path.is_symlink():
        encoded, _ = read_owned(path, os.getuid())
        if plistlib.loads(encoded) != expected:
            raise RuntimeError("A different definition occupies the recovery job path")
    label = f"gui/{os.getuid()}/{PREFIX}{token}"
    job = run("/bin/launchctl", "print", label, check=False)
    if job.returncode:
        if (
            job.returncode not in (3, 113)
            or "Could not find service" not in job.stdout + job.stderr
        ):
            raise RuntimeError("The recovery job could not be inspected")
        return None
    lines = {line.strip() for line in job.stdout.splitlines()}
    if f"path = {path}" not in lines or "program = /usr/bin/python3" not in lines:
        raise RuntimeError("Another job occupies the recovery label")
    found = re.search(r"^\s*pid = (\d+)\s*$", job.stdout, re.M)
    return {"label": label, "pid": int(found[1]) if found else None}


def unlink_owned(path, owner):
    try:
        _, expected = read_owned(path, owner)
    except FileNotFoundError:
        return
    parent = open_directory(path.parent)
    try:
        if identity(os.stat(path.name, dir_fd=parent, follow_symlinks=False)) != expected:
            raise RuntimeError("Recovery metadata changed before removal")
        os.unlink(path.name, dir_fd=parent)
        os.fsync(parent)
    finally:
        os.close(parent)


def cleanup_agent(updates, token, tools, *, run=command, bootout=True):
    owner = os.getuid()
    path, permit = agent_paths(updates, token)
    loaded = job_state(updates, token, tools, run=run)
    with RecoveryReady(updates) as ready:
        if ready.pending() and ready.load()["token"] == token:
            ready.remove_record()
    if permit.exists() or permit.is_symlink():
        encoded, _ = read_owned(permit, owner)
        if json.loads(encoded) != {"version": 1, "token": token, "tools": tools}:
            raise RuntimeError("The recovery permit belongs to a different transaction")
    # Remove the admission barrier last among mutable state. A leftover plist at
    # next login performs only this fixed cleanup when its transaction is gone.
    unlink_owned(permit, owner)
    unlink_owned(path, owner)
    for log in (permit.with_suffix(".stdout.log"), permit.with_suffix(".stderr.log")):
        if log.exists():
            info = log.lstat()
            if stat.S_ISREG(info.st_mode) and info.st_uid == owner and info.st_nlink == 1:
                log.unlink()
    if bootout and loaded is not None:
        run("/bin/launchctl", "bootout", loaded["label"], check=False)


def cleanup_orphan_permits(updates, *, run=command):
    jobs = Path(updates) / "RecoveryJobs"
    if not jobs.exists():
        return
    require_private_directory(jobs, os.getuid())
    for path in jobs.glob("*.permit"):
        if not re.fullmatch(r"[a-f0-9]{32}\.permit", path.name):
            raise RuntimeError("An unknown recovery permit is retained for inspection")
        encoded, _ = read_owned(path, os.getuid())
        value = json.loads(encoded)
        if (
            not isinstance(value, dict)
            or set(value) != {"version", "token", "tools"}
            or value["version"] != 1
        ):
            raise RuntimeError("The recovery permit has an unsupported schema")
        if path.stem != value["token"]:
            raise RuntimeError("A recovery permit changed transaction identity")
        cleanup_agent(updates, value["token"], value["tools"], run=run)


class AppRecoveryLease:
    def __init__(self, host, folder, *, session_provider=login_session, run=command):
        self.host, self.folder, self.run = host, Path(folder), run
        self.session_provider = session_provider
        self.state = None
        self.tools = None

    @property
    def work(self):
        return self.state.work

    def __enter__(self):
        self.tools = stage_tools(self.folder)
        with RecoveryReady(self.folder) as ready:
            if ready.pending():
                stale = ready.load()
                cleanup_agent(self.folder, stale["token"], stale["tools"], run=self.run)
        self.state = AppRecoveryState(self.folder)
        try:
            self.state.begin(self.tools, self.session_provider())
            path, permit = agent_paths(self.folder, self.state.value["token"])
            require_private_directory(permit.parent, self.host.uid)
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            directory = open_directory(path.parent)
            try:
                info = os.fstat(directory)
                if info.st_uid != self.host.uid or info.st_mode & 0o022:
                    raise ValueError("The account LaunchAgents directory is unsafe")
            finally:
                os.close(directory)
            publish(
                permit,
                json.dumps(
                    {"version": 1, "token": self.state.value["token"], "tools": self.tools}
                ).encode(),
                self.host.uid,
            )
            publish(
                path,
                plistlib.dumps(
                    agent_definition(self.folder, self.state.value["token"], self.tools)
                ),
                self.host.uid,
            )
            try:
                self.run("/bin/launchctl", "bootstrap", self.host.domain, path)
            except Exception:
                # Bootstrap can return an ambiguous result; inspect this exact
                # job rather than starting a second recovery owner.
                if (
                    job_state(self.folder, self.state.value["token"], self.tools, run=self.run)
                    is None
                ):
                    raise
            self.require_ready()
            return self
        except BaseException:
            self.close()
            raise

    def require_ready(self):
        until = time.monotonic() + 12
        with RecoveryReady(self.folder) as ready:
            while time.monotonic() < until:
                try:
                    if ready.pending():
                        value = ready.load()
                        if (
                            value["token"] != self.state.value["token"]
                            or value["tools"] != self.tools
                            or value["session"] != self.state.value["session"]
                        ):
                            raise RuntimeError(
                                "The recovery acknowledgement belongs to another update or login"
                            )
                        observed = job_state(
                            self.folder, value["token"], self.tools, run=self.run
                        )
                        guard_ready = (
                            self.state.value["guard"] is None
                            or value.get("guard") == self.state.value["guard"]
                        )
                        if (
                            guard_ready
                            and observed is not None
                            and observed["pid"] == value["process"]["pid"]
                        ):
                            process = process_generation(observed["pid"], self.host.uid)
                            if not same_generation(process, value["process"]):
                                raise RuntimeError(
                                    "The recovery owner restarted before acknowledgement"
                                )
                            require_process_code(process["pid"], "anchor apple")
                            verify_tools(
                                self.folder / "RecoveryTools" / self.tools,
                                self.tools,
                                self.host.uid,
                            )
                            return
                except ProcessLookupError:
                    pass
                time.sleep(0.05)
        raise RuntimeError("The temporary recovery job did not acknowledge readiness")

    def observe_guard(self, guard):
        if not hasattr(guard, "process"):
            return
        process = process_generation(guard.process.pid, self.host.uid)
        self.state.guard_started(process)

    def close(self):
        if self.state is None:
            return
        try:
            if self.state.value is None or not self.state.pending():
                return
            with AppUpdateJournal(self.folder, TARGETS) as journal:
                if journal.pending():
                    return  # The independent owner takes over when our lock closes.
            workspace = AppRecoveryWorkspace(self.state)
            self.state.resolve_creation()
            workspace.quiesce()
            self.state.finishing()
            workspace.clean()
            value = self.state.value
            self.state.remove_record()
            cleanup_agent(self.folder, value["token"], value["tools"], run=self.run)
        finally:
            self.state.close()

    def __exit__(self, *_):
        self.close()
