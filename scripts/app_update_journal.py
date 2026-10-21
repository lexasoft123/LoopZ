"""Durable user-app transaction data; destinations derive only from fixed roles."""

import copy
import os
from pathlib import Path, PurePosixPath
import re
import stat

from desktop_files import identity, remove_at
from desktop_install import NAMES, bounded_files
from install_transaction import check_ancestors
from protected_record import ProtectedRecord
from uninstall_files import open_directory, rename_exclusive

NAME = ".app-update-state.json"
PHASES = {
    "preparing",
    "prepared",
    "activating",
    "validating",
    "committed",
    "restoring",
    "restored",
    "committed_cleanup",
    "restored_cleanup",
}
TRANSITIONS = {
    "preparing": {"prepared", "restoring"},
    "prepared": {"activating", "restoring"},
    "activating": {"validating", "restoring"},
    "validating": {"committed", "restoring"},
    "committed": {"committed_cleanup"},
    "committed_cleanup": {"committed_cleanup"},
    "restoring": {"restoring", "restored"},
    "restored": {"restoring", "restored_cleanup"},
    "restored_cleanup": {"restored_cleanup"},
}


def inventory(path, owner):
    path = Path(path)
    hashes = bounded_files(path)
    if not hashes or not path.is_dir():
        raise ValueError("An application inventory requires a nonempty directory")
    for item in [path, *path.rglob("*")]:
        info = item.lstat()
        if info.st_uid != owner or info.st_mode & 0o022:
            raise ValueError("Application recovery requires protected user-owned bundles")
    info = path.lstat()
    directories = sorted(str(item.relative_to(path)) for item in path.rglob("*") if item.is_dir())
    return {
        "device": info.st_dev,
        "inode": info.st_ino,
        "files": hashes,
        "directories": directories,
    }


class AppUpdateJournal(ProtectedRecord):
    def __init__(self, folder, targets, *, owner=None):
        self.targets = {name: Path(path) for name, path in targets.items()}
        if set(self.targets) != set(NAMES) or any(
            path.name != name for name, path in self.targets.items()
        ):
            raise ValueError("Recovery requires the two fixed application roles")
        super().__init__(folder, NAME, owner=owner)
        if self.owner < 501 or os.getuid() != self.owner or os.geteuid() != self.owner:
            self.close()
            raise ValueError("User app recovery must not run with root authority")

    def validate(self, value):
        if not isinstance(value, dict) or set(value) != {
            "version",
            "owner",
            "token",
            "names",
            "phase",
            "old",
            "new",
            "context",
            "tools",
            "parents",
        }:
            raise ValueError("Invalid application recovery record")
        if (
            type(value["version"]) is not int
            or value["version"] != 1
            or type(value["owner"]) is not int
            or value["owner"] != self.owner
        ):
            raise ValueError("Unsupported application recovery version or owner")
        if not isinstance(value["token"], str) or not re.fullmatch(r"[a-f0-9]{32}", value["token"]):
            raise ValueError("Invalid application recovery token")
        names = value["names"]
        if (
            names not in ([NAMES[0]], list(NAMES))
            or not isinstance(value["phase"], str)
            or value["phase"] not in PHASES
        ):
            raise ValueError("Invalid application recovery roles or phase")
        for key in ("old", "new"):
            if not isinstance(value[key], dict) or set(value[key]) != set(names):
                raise ValueError("Incomplete application recovery inventory")
            for node in value[key].values():
                if (
                    node is None
                    and key == "new"
                    and value["phase"] in ("preparing", "restoring", "restored", "restored_cleanup")
                ):
                    continue
                if not isinstance(node, dict) or set(node) != {
                    "device", "inode", "files", "directories"
                }:
                    raise ValueError("Invalid application recovery node")
                if any(type(node[k]) is not int or node[k] <= 0 for k in ("device", "inode")):
                    raise ValueError("Invalid application inode identity")
                if not isinstance(node["files"], dict) or not 1 <= len(node["files"]) <= 1024:
                    raise ValueError("Invalid bounded application file list")
                directories = node["directories"]
                if (
                    not isinstance(directories, list)
                    or len(directories) > 1024
                    or any(not isinstance(name, str) for name in directories)
                    or directories != sorted(set(directories))
                ):
                    raise ValueError("Invalid bounded application directory list")
                for name in [*node["files"], *directories]:
                    if not isinstance(name, str) or not 1 <= len(name) <= 4096:
                        raise ValueError("Invalid relative application file name")
                    path = PurePosixPath(name)
                    if (
                        not path.parts
                        or path.is_absolute()
                        or str(path) != name
                        or any(part in (".", "..") for part in path.parts)
                        or "\0" in name
                    ):
                        raise ValueError("Invalid relative application path")
                if any(
                    not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest)
                    for digest in node["files"].values()
                ):
                    raise ValueError("Invalid relative application file hash")
        parents = value["parents"]
        if (
            not isinstance(parents, dict)
            or set(parents) != set(names)
            or any(
                not isinstance(node, list)
                or len(node) != 2
                or any(type(part) is not int or part <= 0 for part in node)
                for node in parents.values()
            )
        ):
            raise ValueError("Invalid application parent identities")
        if (
            not isinstance(value["context"], dict)
            or not isinstance(value["tools"], str)
            or not re.fullmatch(r"[a-f0-9]{64}", value["tools"])
        ):
            raise ValueError("Invalid application recovery context")

    def begin(self, transaction, context, tools):
        if self.pending():
            raise RuntimeError("Recover the interrupted app update before starting another")
        old = {name: inventory(transaction.targets[name], self.owner) for name in transaction.names}
        new = {name: None for name in transaction.names}
        self.save(
            {
                "version": 1,
                "owner": self.owner,
                "token": transaction.token,
                "names": list(transaction.names),
                "phase": "preparing",
                "old": old,
                "new": new,
                "context": copy.deepcopy(context),
                "tools": tools,
                "parents": {
                    name: list(identity(transaction.targets[name].parent.stat()))
                    for name in transaction.names
                },
            },
            initial=True,
        )

    def prepared_node(self, name, prepared):
        if (
            self.value["phase"] != "preparing"
            or name not in self.value["names"]
            or self.value["new"][name] is not None
        ):
            raise RuntimeError("Unexpected prepared application role")
        value = copy.deepcopy(self.value)
        value["new"][name] = inventory(prepared, self.owner)
        self.save(value)

    def phase(self, name):
        if name not in TRANSITIONS[self.value["phase"]]:
            raise RuntimeError("Unsupported application recovery phase transition")
        value = copy.deepcopy(self.value)
        value["phase"] = name
        self.save(value)

    def slot(self, name, kind):
        if name not in self.value["names"] or kind not in ("new", "old", "aborted"):
            raise ValueError("Unknown application recovery slot")
        target = self.targets[name]
        return target.parent / f".{target.name}.loopz-{kind}-{self.value['token']}"

    def matches(self, path, node, *, partial=False):
        check_ancestors([path])
        if not path.exists():
            return False
        if node is None:
            raise RuntimeError("An unrecorded prepared application exists")
        info = path.lstat()
        if (
            not stat.S_ISDIR(info.st_mode)
            or info.st_uid != self.owner
            or identity(info) != (node["device"], node["inode"])
        ):
            raise RuntimeError("An application recovery slot changed identity")
        actual = bounded_files(path)
        directories = {str(item.relative_to(path)) for item in path.rglob("*") if item.is_dir()}
        for item in [path, *path.rglob("*")]:
            child = item.lstat()
            if child.st_uid != self.owner or child.st_mode & 0o022:
                raise RuntimeError("Recovery component ownership or permissions changed")
        file_mismatch = (
            any(node["files"].get(k) != v for k, v in actual.items())
            if partial
            else actual != node["files"]
        )
        directory_mismatch = (
            not directories <= set(node["directories"])
            if partial
            else directories != set(node["directories"])
        )
        if file_mismatch or directory_mismatch:
            raise RuntimeError("An application recovery slot contains unrecorded changes")
        return True

    def require_match(self, path, node, *, partial=False):
        if not self.matches(path, node, partial=partial):
            raise RuntimeError("A required application recovery component is missing")

    def open_parent(self, name):
        descriptor = open_directory(self.targets[name].parent)
        if list(identity(os.fstat(descriptor))) != self.value["parents"][name]:
            os.close(descriptor)
            raise RuntimeError("An application recovery parent changed identity")
        return descriptor

    def restore_files(self, verify_code):
        """Rollback is restartable between every rename; no stored path is executed."""
        if self.value["phase"] in ("committed", "committed_cleanup"):
            raise RuntimeError("A committed update must complete cleanup, not roll back")
        if self.value["phase"] == "restored_cleanup":
            for name in self.value["names"]:
                self.require_match(self.targets[name], self.value["old"][name])
                verify_code(name, self.targets[name])
            return
        self.phase("restoring")
        parents = {}
        try:
            # Inspect and verify every available original before any mutation.
            for name in self.value["names"]:
                target, old = self.targets[name], self.value["old"][name]
                parent = target.parent
                check_ancestors([parent])
                if parent not in parents:
                    parents[parent] = self.open_parent(name)
                if target.exists() and identity(target.lstat()) == (old["device"], old["inode"]):
                    self.matches(target, old)
                    verify_code(name, target)
                else:
                    backup = self.slot(name, "old")
                    if not self.matches(backup, old):
                        raise RuntimeError("The original application backup is missing")
                    verify_code(name, backup)
                    if target.exists():
                        self.matches(target, self.value["new"][name])
            for name in reversed(self.value["names"]):
                target, old, new = (
                    self.targets[name],
                    self.value["old"][name],
                    self.value["new"][name],
                )
                parent = parents[target.parent]
                if target.exists() and identity(target.lstat()) == (old["device"], old["inode"]):
                    continue
                aborted = self.slot(name, "aborted")
                if target.exists():
                    self.matches(target, new)
                    if aborted.exists():
                        raise RuntimeError("A duplicate aborted application already exists")
                    rename_exclusive(parent, parent, target.name, aborted.name)
                    os.fsync(parent)
                self.require_match(self.slot(name, "old"), old)
                if target.exists():
                    raise RuntimeError("The application target changed before restoration")
                rename_exclusive(parent, parent, self.slot(name, "old").name, target.name)
                os.fsync(parent)
            for name in self.value["names"]:
                self.require_match(self.targets[name], self.value["old"][name])
                verify_code(name, self.targets[name])
            self.phase("restored")
        finally:
            for descriptor in parents.values():
                os.close(descriptor)

    def finish(self, verify_code, *, committed=False):
        expected_phases = (
            ("committed", "committed_cleanup") if committed else ("restored", "restored_cleanup")
        )
        if self.value["phase"] not in expected_phases:
            raise RuntimeError("Verify the application outcome before removing recovery files")
        desired = "new" if committed else "old"
        for name in self.value["names"]:
            descriptor = self.open_parent(name)
            os.close(descriptor)
            self.require_match(self.targets[name], self.value[desired][name])
            verify_code(name, self.targets[name])
        self.phase("committed_cleanup" if committed else "restored_cleanup")
        for name in self.value["names"]:
            for kind, node_key in (("new", "new"), ("aborted", "new"), ("old", "old")):
                path = self.slot(name, kind)
                if self.matches(path, self.value[node_key][name], partial=True):
                    descriptor = self.open_parent(name)
                    try:
                        remove_at(
                            descriptor,
                            path.name,
                            (
                                self.value[node_key][name]["device"],
                                self.value[node_key][name]["inode"],
                            ),
                        )
                        os.fsync(descriptor)
                    finally:
                        os.close(descriptor)
        self.remove_record()
