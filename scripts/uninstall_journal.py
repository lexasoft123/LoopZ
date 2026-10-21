"""Durable file-disposition records for an interrupted macOS uninstall.

Callers hold the shared installer lock. Production supplies fixed product paths;
tests supply temporary paths. File recovery never starts or signals a service,
changes a sound default, executes a stored command, or overwrites a destination.
Runtime/workspace verification remains a separate finalization requirement.
"""

import copy
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import uuid

from application_ownership import snapshot_owner
from desktop_files import identity
from uninstall_files import (
    RemovalFiles,
    open_directory,
    remaining_snapshot,
    rename_exclusive,
    scan_identity,
)
from uninstall_policy import digest

NAME = ".uninstall-state.json"
LIMIT = 32 * 1024 * 1024
PHASES = {"removing", "purging", "restored", "removed"}


class RemovalJournal:
    def __init__(
        self,
        folder,
        targets,
        *,
        owner=None,
        workspace_root=Path("/private/tmp"),
        user_components=(),
    ):
        self.folder = Path(folder)
        self.targets = {name: Path(path) for name, path in targets.items()}
        self.owner = os.geteuid() if owner is None else owner
        self.user_components = frozenset(user_components)
        if not self.user_components <= set(self.targets):
            raise ValueError("Unknown user application component")
        self.workspace_root = Path(workspace_root)
        self.directory = open_directory(self.folder)
        info = os.fstat(self.directory)
        if info.st_uid != self.owner or info.st_mode & 0o022:
            os.close(self.directory)
            raise ValueError("Recovery journal directory must be protected and owned")
        self.value = None
        self.file_identity = None
        self.created = False

    def close(self):
        if self.directory is not None:
            os.close(self.directory)
            self.directory = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def pending(self):
        try:
            info = os.stat(NAME, dir_fd=self.directory, follow_symlinks=False)
        except FileNotFoundError:
            return False
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != self.owner
            or info.st_mode & 0o077
            or info.st_nlink != 1
            or info.st_size > LIMIT
        ):
            raise ValueError("Recovery journal is not a protected bounded regular file")
        return True

    def save(self, value, *, initial=False):
        value = copy.deepcopy(value)
        self.validate(value)
        encoded = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
        if len(encoded) > LIMIT:
            raise ValueError("Recovery journal exceeds its payload limit")
        temporary = f".uninstall-state-{uuid.uuid4().hex}.tmp"
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
            dir_fd=self.directory,
        )
        try:
            with os.fdopen(descriptor, "wb", closefd=False) as output:
                output.write(encoded)
                output.flush()
                os.fsync(descriptor)
                fcntl.fcntl(descriptor, fcntl.F_FULLFSYNC)
            if initial:
                rename_exclusive(self.directory, self.directory, temporary, NAME)
            else:
                current = os.stat(NAME, dir_fd=self.directory, follow_symlinks=False)
                if identity(current) != self.file_identity:
                    raise RuntimeError("Recovery journal changed before update")
                os.rename(temporary, NAME, src_dir_fd=self.directory, dst_dir_fd=self.directory)
            self.file_identity = identity(os.fstat(descriptor))
            self.value = value
            self.created = True
            os.fsync(self.directory)
            fcntl.fcntl(descriptor, fcntl.F_FULLFSYNC)
        finally:
            os.close(descriptor)
            try:
                os.unlink(temporary, dir_fd=self.directory)
            except FileNotFoundError:
                pass

    def begin(self, plan, removal, work):
        if self.pending():
            raise RuntimeError("An unfinished uninstall requires recovery before another removal")
        work = Path(work)
        if work.parent != self.workspace_root or not work.name.startswith("loopz-uninstall-"):
            raise ValueError("Recovery workspace must use the fixed private temporary namespace")
        descriptor = open_directory(work)
        try:
            info = os.fstat(descriptor)
            if info.st_uid != self.owner or info.st_mode & 0o077:
                raise ValueError("Recovery workspace must be private and owned")
            workspace = {"path": str(work), "device": info.st_dev, "inode": info.st_ino}
        finally:
            os.close(descriptor)
        parents = {
            str(path): {"device": os.fstat(fd).st_dev, "inode": os.fstat(fd).st_ino}
            for path, fd in removal.parents.items()
        }
        value = {
            "schema": 1,
            "phase": "removing",
            "token": removal.token,
            "plan": plan,
            "planSHA256": digest(plan),
            "parents": parents,
            "quarantines": {},
            "workspace": workspace,
        }
        self.validate(value)
        self.save(value, initial=True)
        removal.journal = self

    def record_quarantine(self, removal, parent, descriptor):
        if (
            self.value["phase"] != "removing"
            or removal.token != self.value["token"]
            or removal.originals != self.value["plan"]["inventory"]
        ):
            raise RuntimeError("Quarantine does not belong to the recorded removal")
        value = copy.deepcopy(self.value)
        info = os.fstat(descriptor)
        value["quarantines"][str(parent)] = {"device": info.st_dev, "inode": info.st_ino}
        self.save(value)

    def replace_workspace(self, work):
        work = Path(work)
        descriptor = open_directory(work)
        try:
            info = os.fstat(descriptor)
            if info.st_uid != self.owner or info.st_mode & 0o077:
                raise ValueError("Recovery workspace must be private and owned")
            value = copy.deepcopy(self.value)
            value["workspace"] = {"path": str(work), "device": info.st_dev, "inode": info.st_ino}
            self.save(value)
        finally:
            os.close(descriptor)

    def begin_purge(self):
        if self.value["phase"] != "removing":
            raise RuntimeError("The durable removal decision is already final")
        value = copy.deepcopy(self.value)
        value["phase"] = "purging"
        self.save(value)

    def finish(self, phase):
        if phase not in ("restored", "removed"):
            raise ValueError("Invalid recovery completion phase")
        committed = self.value["phase"] in ("purging", "removed")
        if (phase == "removed") != committed:
            raise ValueError("Recovery completion cannot cross the durable commit decision")
        with self.adopt() as inspect:
            if inspect.moved:
                raise RuntimeError("File recovery must finish before journal finalization")
            inspect.clean_empty_quarantines()
        value = copy.deepcopy(self.value)
        value["phase"] = phase
        self.save(value)
        if (
            identity(os.stat(NAME, dir_fd=self.directory, follow_symlinks=False))
            != self.file_identity
        ):
            raise RuntimeError("Recovery journal changed before finalization")
        os.unlink(NAME, dir_fd=self.directory)
        os.fsync(self.directory)

    def load(self):
        if not self.pending():
            raise RuntimeError("No unfinished uninstall was found")
        descriptor = os.open(
            NAME, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.directory
        )
        try:
            before = os.fstat(descriptor)
            if (
                before.st_uid != self.owner
                or before.st_mode & 0o077
                or not stat.S_ISREG(before.st_mode)
                or before.st_nlink != 1
                or before.st_size > LIMIT
            ):
                raise ValueError("Recovery journal changed while opening")
            with os.fdopen(descriptor, "rb", closefd=False) as source:
                encoded = source.read(LIMIT + 1)
            if len(encoded) > LIMIT or scan_identity(os.fstat(descriptor)) != scan_identity(before):
                raise RuntimeError("Recovery journal changed while reading")
            current = os.stat(NAME, dir_fd=self.directory, follow_symlinks=False)
            if identity(current) != identity(before):
                raise RuntimeError("Recovery journal was replaced while reading")
            value = json.loads(encoded)
            self.validate(value)
            self.value, self.file_identity = value, identity(before)
            return copy.deepcopy(value)
        finally:
            os.close(descriptor)

    def validate(self, value):
        fields = {
            "schema",
            "phase",
            "token",
            "plan",
            "planSHA256",
            "parents",
            "quarantines",
            "workspace",
        }
        if (
            not isinstance(value, dict)
            or set(value) != fields
            or type(value["schema"]) is not int
            or value["schema"] != 1
            or not isinstance(value["phase"], str)
            or value["phase"] not in PHASES
            or not isinstance(value["token"], str)
            or not re.fullmatch(r"[a-f0-9]{32}", value["token"])
        ):
            raise ValueError("Unsupported recovery journal schema")
        plan = value["plan"]
        if (
            not isinstance(plan, dict)
            or type(plan.get("version")) is not int
            or plan.get("version") != 1
            or plan.get("mode") != "loopz-uninstall"
            or plan.get("targets") != {name: str(path) for name, path in self.targets.items()}
            or not isinstance(plan.get("inventory"), dict)
            or set(plan["inventory"]) != set(self.targets)
            or value["planSHA256"] != digest(plan)
        ):
            raise ValueError("Recovery plan does not match the fixed product inventory")
        allowed_parents = {str(path.parent) for path in self.targets.values()}
        for key in ("parents", "quarantines"):
            entries = value[key]
            if not isinstance(entries, dict) or not set(entries) <= allowed_parents:
                raise ValueError("Recovery journal contains foreign parent paths")
            for node in entries.values():
                if (
                    not isinstance(node, dict)
                    or set(node) != {"device", "inode"}
                    or any(type(number) is not int or number <= 0 for number in node.values())
                ):
                    raise ValueError("Recovery journal has an invalid directory identity")
        if not set(value["quarantines"]) <= set(value["parents"]):
            raise ValueError("Quarantine lacks a recorded parent identity")
        for name, node in plan["inventory"].items():
            if node is not None:
                self.validate_snapshot(
                    node,
                    expected_owner=snapshot_owner(name, node, self.owner, self.user_components),
                )
                if str(self.targets[name].parent) not in value["parents"]:
                    raise ValueError("Recovery component lacks its parent identity")
        work = value["workspace"]
        if (
            not isinstance(work, dict)
            or set(work) != {"path", "device", "inode"}
            or not isinstance(work["path"], str)
        ):
            raise ValueError("Invalid recovery workspace")
        path = Path(work["path"])
        if (
            path.parent != self.workspace_root
            or not path.name.startswith("loopz-uninstall-")
            or "\0" in work["path"]
            or ".." in path.parts
            or any(type(work[k]) is not int or work[k] <= 0 for k in ("device", "inode"))
        ):
            raise ValueError("Recovery workspace is outside its fixed namespace")

    def validate_snapshot(self, node, depth=0, count=None, expected_owner=None):
        expected_owner = self.owner if expected_owner is None else expected_owner
        count = [0] if count is None else count
        count[0] += 1
        basic = {"device", "inode", "mode", "owner", "group", "modifiedNs"}
        if (
            depth > 64
            or count[0] > 4096
            or not isinstance(node, dict)
            or not basic <= set(node)
            or any(type(node[k]) is not int or node[k] < 0 for k in basic)
            or node["owner"] != expected_owner
            or node["mode"] & 0o022
        ):
            raise ValueError("Invalid recovery component snapshot")
        if stat.S_ISDIR(node["mode"]):
            if set(node) != basic | {"entries"} or not isinstance(node["entries"], dict):
                raise ValueError("Invalid directory snapshot")
            for name, child in node["entries"].items():
                if (
                    not isinstance(name, str)
                    or not name
                    or name in (".", "..")
                    or "/" in name
                    or "\0" in name
                ):
                    raise ValueError("Invalid snapshot entry name")
                self.validate_snapshot(child, depth + 1, count, expected_owner)
        elif (
            not stat.S_ISREG(node["mode"])
            or set(node) != basic | {"size", "sha256"}
            or type(node.get("size")) is not int
            or not 0 <= node["size"] <= 64 * 1024 * 1024
            or not isinstance(node.get("sha256"), str)
            or not re.fullmatch(r"[a-f0-9]{64}", node["sha256"])
        ):
            raise ValueError("Invalid file snapshot")

    def adopt(self):
        """Verify every placement before recovery makes its first file change."""
        if self.value is None:
            self.load()
        value = self.value
        removal = RemovalFiles(self.targets, owner=self.owner, user_components=self.user_components)
        removal.token = value["token"]
        removal.originals = copy.deepcopy(value["plan"]["inventory"])
        committed = value["phase"] in ("purging", "removed")

        def optional_scan(parent, name, component):
            if parent is None:
                return None
            try:
                os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                return None
            return removal.scan(parent, name, component=component)

        try:
            for text, expected in value["parents"].items():
                parent = Path(text)
                folder = parent / f".loopz-remove-{removal.token}"
                descriptor = removal.parent(folder)
                if identity(os.fstat(descriptor)) != (expected["device"], expected["inode"]):
                    raise RuntimeError("Recovery parent directory changed")
                try:
                    child = os.open(
                        folder.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor
                    )
                except FileNotFoundError:
                    continue
                info = os.fstat(child)
                recorded = value["quarantines"].get(text)
                if (
                    info.st_uid != self.owner
                    or info.st_mode & 0o077
                    or recorded
                    and identity(info) != (recorded["device"], recorded["inode"])
                ):
                    os.close(child)
                    raise RuntimeError("Recovery quarantine identity changed")
                allowed = {
                    target.name
                    for name, target in self.targets.items()
                    if target.parent == parent and removal.originals[name] is not None
                }
                entries = set(os.listdir(child))
                if not entries <= allowed or recorded is None and entries:
                    os.close(child)
                    raise RuntimeError("Unrecorded files occupy a recovery quarantine")
                removal.quarantines[parent] = (folder, child)
            for name, target in self.targets.items():
                original = removal.originals[name]
                if original is None:
                    continue
                parent = removal.parents[target.parent]
                saved = removal.quarantines.get(target.parent)
                observed = optional_scan(saved[1] if saved else None, target.name, name)
                if committed:
                    if observed is not None:
                        if not remaining_snapshot(observed, original):
                            raise RuntimeError("Committed quarantine contains changed data")
                        removal.moved.append(name)
                    continue
                current = optional_scan(parent, target.name, name)
                if observed is not None:
                    if observed != original or current is not None:
                        raise RuntimeError(
                            "Recovery refuses a changed original or destination collision"
                        )
                    removal.moved.append(name)
                elif current != original:
                    raise RuntimeError("An original recovery component is missing or replaced")
            return removal
        except BaseException:
            removal.close()
            raise

    def recover_files(self):
        """Recover file placement; keep journal until runtime/workspace finalization."""
        with self.adopt() as removal:
            committed = self.value["phase"] in ("purging", "removed")
            if committed:
                failures = removal.purge(allow_partial=True) if removal.moved else []
                if not removal.moved:
                    removal.clean_empty_quarantines()
                return {
                    "status": "cleanupRequired" if failures else "committedFilesCleaned",
                    "committed": True,
                    "cleanupRequired": failures,
                    "runtimeFinalizationRequired": True,
                    "journalRetained": True,
                }
            removal.rollback()
            return {
                "status": "originalFilesRestored",
                "committed": False,
                "runtimeFinalizationRequired": True,
                "journalRetained": True,
            }
