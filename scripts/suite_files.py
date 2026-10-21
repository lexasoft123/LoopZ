"""Durable four-component replacement, separate from service activation.

The caller holds the shared installation lock and verifies the signed private
source before preparation. Target paths come from fixed product policy. Tests
substitute private targets. Opening or closing this object never changes files.
Originals stay available until a caller certifies the new runtime; restored files
also retain their journal until the original runtime has been verified.
"""

import copy
import fcntl
import os
from pathlib import Path
import shutil
import uuid
import stat

from application_ownership import APPLICATIONS
from desktop_files import identity, remove_at
from suite_update import NAMES
from suite_journal import NAME, SuiteJournal, node_identity
from uninstall_files import RemovalFiles, rename_exclusive


class SuiteFiles(SuiteJournal):
    def __init__(self, folder, targets, *, owner=None, checkpoint=lambda event: None):
        self.inspector = RemovalFiles(targets, owner=owner, user_components=APPLICATIONS)
        super().__init__(folder, targets, owner=owner)
        self.checkpoint = checkpoint

    def close(self):
        self.inspector.close()
        super().close()

    def parent(self, name):
        descriptor = self.inspector.parent(self.targets[name])
        if self.value:
            expected = self.value["parents"][name]
            if list(identity(os.fstat(descriptor))) != expected:
                raise RuntimeError("An update target parent was replaced")
        return descriptor

    def sibling(self, name, kind):
        return f".{self.targets[name].name}.loopz-{kind}-{self.value['token']}"

    def scan(self, name, entry):
        parent = self.parent(name)
        try:
            os.stat(entry, dir_fd=parent, follow_symlinks=False)
        except FileNotFoundError:
            return None
        return self.inspector.scan(parent, entry, component=name)

    def phase(self, phase):
        value = copy.deepcopy(self.value)
        value["phase"] = phase
        self.save(value)
        self.checkpoint("phase:" + phase)

    def workspace_name(self):
        return ".loopz-suite-work-" + self.value["token"]

    def workspace(self):
        name = self.workspace_name()
        descriptor = os.open(
            name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=self.directory
        )
        info = os.fstat(descriptor)
        if (
            list(identity(info)) != self.value["workspace"]
            or info.st_uid != self.owner
            or info.st_mode & 0o077
        ):
            os.close(descriptor)
            raise RuntimeError("The private update workspace changed")
        return descriptor

    def prepare(self, stage):
        if self.pending():
            raise RuntimeError("Recover the unfinished coordinated update first")
        for name in (".uninstall-state.json", ".suite-driver-state.json"):
            try:
                os.stat(name, dir_fd=self.directory, follow_symlinks=False)
            except FileNotFoundError:
                continue
            raise RuntimeError(
                "Recover the unfinished uninstall or driver recovery before updating"
            )
        originals = self.inspector.inventory()
        if any(value is None for value in originals.values()):
            raise ValueError("A coordinated update requires all four installed components")
        if any(os.fstat(self.parent(name)).st_dev != self.folder_identity[0] for name in NAMES):
            raise ValueError("Coordinated replacement requires one filesystem")
        value = {
            "schema": 1,
            "phase": "preparing",
            "token": uuid.uuid4().hex,
            "parents": {name: list(identity(os.fstat(self.parent(name)))) for name in NAMES},
            "originals": originals,
            "candidates": dict.fromkeys(NAMES),
            "workspace": None,
        }
        self.save(value, initial=True)
        self.checkpoint("journal:created")
        os.mkdir(self.workspace_name(), 0o700, dir_fd=self.directory)
        workspace = os.open(
            self.workspace_name(),
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
            dir_fd=self.directory,
        )
        try:
            value["workspace"] = list(identity(os.fstat(workspace)))
            os.fsync(self.directory)
            self.save(value)
            self.checkpoint("workspace:recorded")
            stage = Path(stage)
            # Input is a private, verified root snapshot; descriptor scanning also
            # rejects links, hard links and changed ownership in the copied tree.
            source = RemovalFiles({name: stage / name for name in NAMES}, owner=self.owner)
            try:
                staged = source.inventory()
            finally:
                source.close()
            for name in NAMES:
                if staged[name] is None:
                    raise ValueError("The private update source is incomplete")
                path = self.folder / self.workspace_name() / name
                if stat.S_ISDIR(staged[name]["mode"]):
                    shutil.copytree(stage / name, path, symlinks=True)
                else:
                    shutil.copy2(stage / name, path, follow_symlinks=False)
                candidate = self.inspector.scan(workspace, name, component=name)
                # Inode identities differ after a copy; content and metadata must not.
                if content(candidate) != content(staged[name]):
                    raise RuntimeError("Private replacement differs from the verified source")
                # Backend updates preserve the application's existing account owner.
                # Root-only staging remains private until the atomic promotion.
                original = originals[name]
                if self.owner == 0 and name in APPLICATIONS and original["owner"] >= 501:
                    entries = [path, *path.rglob("*")] if path.is_dir() else [path]
                    for entry in entries:
                        os.chown(entry, original["owner"], original["group"], follow_symlinks=False)
                    candidate = self.inspector.scan(workspace, name, component=name)
                self.flush_tree(path)
                value = copy.deepcopy(self.value)
                value["candidates"][name] = candidate
                self.save(value)
                self.checkpoint("candidate:recorded:" + name)
                parent = self.parent(name)
                rename_exclusive(workspace, parent, name, self.sibling(name, "new"))
                os.fsync(parent)
                os.fsync(workspace)
                self.checkpoint("candidate:published:" + name)
        finally:
            os.close(workspace)
        self.phase("prepared")

    @staticmethod
    def flush_tree(path):
        paths = [path, *path.rglob("*")] if path.is_dir() else [path]
        for entry in sorted(paths, key=lambda item: len(item.parts), reverse=True):
            descriptor = os.open(entry, os.O_RDONLY | os.O_NOFOLLOW)
            try:
                os.fsync(descriptor)
                if entry.is_file():
                    fcntl.fcntl(descriptor, fcntl.F_FULLFSYNC)
            finally:
                os.close(descriptor)

    def require_locations(self, *, restored=False):
        """Inspect every component before changing any, including recovery retries."""
        locations = {}
        for name, target in self.targets.items():
            old, new = self.value["originals"][name], self.value["candidates"][name]
            current = self.scan(name, target.name)
            backup = self.scan(name, self.sibling(name, "old"))
            candidate = self.scan(name, self.sibling(name, "new"))
            if (
                current is not None
                and current != old
                and (new is None or current != new)
                or backup is not None
                and backup != old
                or candidate is not None
                and (new is None or candidate != new)
                or int(current == old) + int(backup == old) != 1
                or current is not None
                and current == new
                and candidate is not None
                or restored
                and (current != old or backup is not None)
            ):
                raise RuntimeError("A coordinated replacement location changed: " + name)
            locations[name] = (current, backup, candidate)
        return locations

    def apply(self):
        if self.value["phase"] != "prepared":
            raise RuntimeError("Only a freshly prepared update may be promoted")
        locations = self.require_locations(restored=True)
        for name, (_, _, candidate) in locations.items():
            if candidate != self.value["candidates"][name]:
                raise RuntimeError("A prepared replacement is missing")
        # This durable decision precedes even the first original-to-backup move.
        self.phase("activating")
        for name, target in self.targets.items():
            parent = self.parent(name)
            if (
                self.scan(name, target.name) != self.value["originals"][name]
                or self.scan(name, self.sibling(name, "new")) != self.value["candidates"][name]
            ):
                raise RuntimeError("A component changed before promotion")
            rename_exclusive(parent, parent, target.name, self.sibling(name, "old"))
            os.fsync(parent)
            self.checkpoint("original:moved:" + name)
            rename_exclusive(parent, parent, self.sibling(name, "new"), target.name)
            os.fsync(parent)
            self.checkpoint("replacement:active:" + name)

    def rollback(self):
        if self.value["phase"] in ("committed", "restored"):
            raise RuntimeError("A committed update cannot cross back to the original installation")
        self.require_locations()
        self.phase("restoring")
        for name in reversed(tuple(self.targets)):
            target = self.targets[name]
            current, backup, _ = self.require_locations()[name]
            parent = self.parent(name)
            if current == self.value["candidates"][name] and current is not None:
                rename_exclusive(parent, parent, target.name, self.sibling(name, "new"))
                os.fsync(parent)
                self.checkpoint("replacement:retained:" + name)
            if backup is not None:
                rename_exclusive(parent, parent, self.sibling(name, "old"), target.name)
                os.fsync(parent)
                self.checkpoint("original:restored:" + name)
        self.require_locations(restored=True)
        self.phase("filesRestored")

    def commit(self):
        """Caller has verified new service generations, graph and hardware continuity."""
        if self.value["phase"] != "activating":
            raise RuntimeError("Only a verified activation may commit")
        locations = self.require_locations()
        for name, (current, backup, candidate) in locations.items():
            if (
                current != self.value["candidates"][name]
                or backup != self.value["originals"][name]
                or candidate is not None
            ):
                raise RuntimeError("The complete new installation is not active")
        self.phase("committed")

    def finish(self):
        """Caller certifies runtime recovery before finalizing filesRestored state.

        Committed cleanup is resumable. It never restores an old image after the
        durable commit, even if removing backups was interrupted part way through.
        """
        committed = self.value["phase"] == "committed"
        if not committed and self.value["phase"] not in ("filesRestored", "restored"):
            raise RuntimeError("Runtime verification must precede update cleanup")
        if self.value["phase"] == "filesRestored":
            self.require_locations(restored=True)
            # Once runtime restoration is certified, an interrupted cleanup may
            # resume even if a replacement directory is only partially removed.
            self.phase("restored")
        for name, target in self.targets.items():
            expected = self.value["candidates" if committed else "originals"][name]
            if self.scan(name, target.name) != expected:
                raise RuntimeError("The selected installation changed before finalization")
            # Inspect all cleanup roots before deleting any. A partially deleted
            # obsolete tree may resume, but a substituted inode may not.
            parent = self.parent(name)
            key, kind = ("originals", "old") if committed else ("candidates", "new")
            node = self.value[key][name]
            try:
                info = os.stat(self.sibling(name, kind), dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                if node is None or identity(info) != node_identity(node):
                    raise RuntimeError("An update cleanup location was replaced")
            other = "new" if committed else "old"
            try:
                os.stat(self.sibling(name, other), dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                raise RuntimeError("An unexpected replacement occupies a completed update slot")
        try:
            workspace = os.stat(self.workspace_name(), dir_fd=self.directory, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            if self.value["workspace"] is None:
                raise RuntimeError("An unrecorded update workspace requires inspection")
            descriptor = self.workspace()
            os.close(descriptor)
        for name in NAMES:
            parent = self.parent(name)
            key, kind = ("originals", "old") if committed else ("candidates", "new")
            node = self.value[key][name]
            if node is not None:
                remove_at(parent, self.sibling(name, kind), node_identity(node))
                os.fsync(parent)
                self.checkpoint("cleanup:" + name)
        if self.value["workspace"] is not None:
            remove_at(self.directory, self.workspace_name(), tuple(self.value["workspace"]))
            os.fsync(self.directory)
        self.require_directory()
        if (
            identity(os.stat(NAME, dir_fd=self.directory, follow_symlinks=False))
            != self.file_identity
        ):
            raise RuntimeError("Update state changed before finalization")
        os.unlink(NAME, dir_fd=self.directory)
        os.fsync(self.directory)
        self.checkpoint("journal:removed")


def content(node):
    """Compare a copied tree without assigning meaning to its new inode numbers."""
    result = {key: value for key, value in node.items() if key not in ("device", "inode")}
    if "entries" in result:
        result["entries"] = {name: content(child) for name, child in result["entries"].items()}
    return result
