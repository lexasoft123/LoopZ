"""Private preparation and descriptor-scoped cleanup of application replacements."""

import os
from pathlib import Path
import shutil
import stat
import tempfile

from application_ownership import APPLICATIONS
from install_transaction import FileTransaction, blocked_signals, check_ancestors, files


def identity(info):
    return info.st_dev, info.st_ino


def remove_at(parent, name, expected):
    try:
        info = os.stat(name, dir_fd=parent, follow_symlinks=False)
    except FileNotFoundError:
        return
    if identity(info) != expected:
        raise RuntimeError("Refusing to remove a changed application replacement")
    if not stat.S_ISDIR(info.st_mode):
        os.unlink(name, dir_fd=parent)
        return
    child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
    try:
        if identity(os.fstat(child)) != expected:
            raise RuntimeError("Application directory changed during cleanup")
        for entry in os.listdir(child):
            nested = os.stat(entry, dir_fd=child, follow_symlinks=False)
            remove_at(child, entry, identity(nested))
        if identity(os.stat(name, dir_fd=parent, follow_symlinks=False)) != expected:
            raise RuntimeError("Application directory moved during cleanup")
        os.rmdir(name, dir_fd=parent)
    finally:
        os.close(child)


class DesktopFileTransaction(FileTransaction):
    """Never copy or chmod through an administrator-writable destination path.

    The root CLI supplies a root-owned snapshot in a private temporary folder.
    Fully prepared apps move atomically onto the same filesystem. Cleanup opens
    their recorded inodes and walks directory descriptors without following links.
    """

    def __init__(self, *arguments, owners=None):
        super().__init__(*arguments)
        self.owners = dict(owners or {})
        if not set(self.owners) <= APPLICATIONS & set(self.names) or any(
            not isinstance(pair, tuple)
            or len(pair) != 2
            or any(type(value) is not int or value < 0 for value in pair)
            or pair[0] < 501
            for pair in self.owners.values()
        ):
            raise ValueError("Only application bundles may use a regular account owner")
        if os.geteuid() != 0 and any(pair[0] != os.geteuid() for pair in self.owners.values()):
            raise ValueError("An ordinary update cannot assign another account's files")
        self.parent_fds = {}
        self.expected = {}
        self.prepared = None

    def parent(self, path):
        folder = path.parent
        if folder not in self.parent_fds:
            check_ancestors([folder])
            self.parent_fds[folder] = os.open(folder, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        return self.parent_fds[folder]

    def prepare(self):
        check_ancestors([self.stage, *self.targets.values()])
        info = self.stage.stat()
        if info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise ValueError("Application preparation requires a private owned package snapshot")
        try:
            self.prepared = Path(tempfile.mkdtemp(prefix="prepared-apps-", dir=self.stage.parent))
            for name in self.names:
                target = self.targets[name]
                parent = self.parent(target)
                if os.fstat(parent).st_dev != self.prepared.stat().st_dev:
                    raise RuntimeError("Atomic application deployment requires the same filesystem")
                original = self.stage / name
                prepared = self.prepared / name
                if original.is_dir():
                    shutil.copytree(original, prepared, symlinks=True)
                else:
                    shutil.copy2(original, prepared, follow_symlinks=False)
                files(prepared)
                entries = [prepared, *prepared.rglob("*")] if prepared.is_dir() else [prepared]
                for entry in entries:
                    info = entry.lstat()
                    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
                        raise ValueError("Only regular application files may be prepared")
                    if os.geteuid() == 0:
                        os.chown(entry, *self.owners.get(name, (0, 0)), follow_symlinks=False)
                    os.chmod(
                        entry,
                        0o755 if stat.S_ISDIR(info.st_mode) or info.st_mode & 0o111 else 0o644,
                    )
                if files(prepared) != files(original):
                    raise RuntimeError("Prepared application differs from the private snapshot")
                for entry in sorted(entries, key=lambda item: len(item.parts), reverse=True):
                    fd = os.open(entry, os.O_RDONLY | os.O_NOFOLLOW)
                    try:
                        os.fsync(fd)
                    finally:
                        os.close(fd)
                expected = identity(prepared.stat())
                candidate = target.parent / f".{target.name}.loopz-new-{self.token}"
                self.will_prepare(name, prepared)
                with blocked_signals():
                    os.rename(prepared, candidate.name, dst_dir_fd=parent)
                    self.candidates[name] = candidate
                    self.expected[candidate] = expected
                    os.fsync(parent)
        except BaseException:
            self.finish()
            raise

    def will_prepare(self, name, prepared):
        """A durable user transaction records the inode before its first move."""
        pass

    def _promote(self, name):
        target = self.targets[name]
        parent = self.parent(target)
        candidate = self.candidates[name]
        candidate_info = os.stat(candidate.name, dir_fd=parent, follow_symlinks=False)
        if identity(candidate_info) != self.expected[candidate] or stat.S_ISLNK(
            candidate_info.st_mode
        ):
            raise RuntimeError("Prepared application changed before promotion")
        backup = target.parent / f".{target.name}.loopz-old-{self.token}"
        try:
            original = os.stat(target.name, dir_fd=parent, follow_symlinks=False)
        except FileNotFoundError:
            original = None
        if original:
            if stat.S_ISLNK(original.st_mode):
                raise RuntimeError("Refusing to replace a linked application")
            os.rename(target.name, backup.name, src_dir_fd=parent, dst_dir_fd=parent)
            self.backups[name] = backup
            self.expected[backup] = identity(original)
        self.changed.append(name)
        os.rename(candidate.name, target.name, src_dir_fd=parent, dst_dir_fd=parent)
        self.expected[target] = self.expected[candidate]
        os.fsync(parent)

    def rollback(self):
        errors = []
        for name in reversed(self.changed):
            target = self.targets[name]
            parent = self.parent(target)
            try:
                with blocked_signals():
                    expected = self.expected.get(target, self.expected[self.candidates[name]])
                    remove_at(parent, target.name, expected)
                    if name in self.backups:
                        backup = self.backups[name]
                        if (
                            identity(os.stat(backup.name, dir_fd=parent, follow_symlinks=False))
                            != self.expected[backup]
                        ):
                            raise RuntimeError("Application backup changed before restoration")
                        os.rename(backup.name, target.name, src_dir_fd=parent, dst_dir_fd=parent)
                    os.fsync(parent)
            except Exception as error:
                errors.append(f"{name}: {error}")
        if errors:
            raise RuntimeError("Application rollback needs repair: " + "; ".join(errors))
        self.changed.clear()

    def finish(self):
        errors = []
        try:
            for path in [*self.candidates.values(), *self.backups.values()]:
                try:
                    remove_at(self.parent(path), path.name, self.expected[path])
                except Exception as error:
                    errors.append(str(error))
            if self.prepared and self.prepared.exists():
                shutil.rmtree(self.prepared)
        finally:
            self.close()
        if errors:
            raise RuntimeError("Application recovery files retained: " + "; ".join(errors))

    def close(self):
        """Release handles while preserving recovery files after an uncertain outcome."""
        for descriptor in self.parent_fds.values():
            os.close(descriptor)
        self.parent_fds.clear()
