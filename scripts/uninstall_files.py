"""Descriptor-scoped, reversible removal of an explicit component inventory."""

import hashlib
import ctypes
import os
from pathlib import Path
import stat
import uuid

from desktop_files import identity, remove_at
from application_ownership import snapshot_owner
from install_transaction import blocked_signals, check_ancestors


def scan_identity(info):
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_uid,
        info.st_gid,
        info.st_mtime_ns,
        info.st_size,
        info.st_nlink,
    )


def rename_exclusive(source, destination, name, destination_name=None):
    """macOS atomic no-clobber publication, including directory rollback."""
    library = ctypes.CDLL(None, use_errno=True)
    operation = library.renameatx_np
    operation.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    operation.restype = ctypes.c_int
    encoded = os.fsencode(name)
    target = encoded if destination_name is None else os.fsencode(destination_name)
    if operation(source, encoded, destination, target, 0x4):  # RENAME_EXCL
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), name)


def open_directory(path):
    """Open every ancestor without following a concurrently substituted link."""
    if not Path(path).is_absolute() or ".." in Path(path).parts:
        raise ValueError("Removal directory paths must be absolute and normalized")
    descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for component in Path(path).parts[1:]:
            child = os.open(
                component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor
            )
            os.close(descriptor)
            descriptor = child
        result, descriptor = descriptor, None
        return result
    finally:
        if descriptor is not None:
            os.close(descriptor)


class RemovalFiles:
    """Quarantine original inodes; purge only after runtime verification.

    Targets are supplied by the fixed product policy, never by a root CLI path
    argument. Tests supply temporary targets. Closing preserves any quarantine
    that still needs rollback or cleanup rather than implicitly deleting it.
    """

    def __init__(self, targets, owner=None, *, user_components=()):
        self.targets = {name: Path(path) for name, path in targets.items()}
        paths = list(self.targets.values())
        if (
            not paths
            or len(paths) != len(set(paths))
            or any(
                not p.is_absolute()
                or ".." in p.parts
                or any(p in other.parents for other in paths if p != other)
                for p in paths
            )
        ):
            raise ValueError("Removal requires distinct absolute, non-overlapping targets")
        self.owner = os.geteuid() if owner is None else owner
        self.user_components = frozenset(user_components)
        if not self.user_components <= set(self.targets):
            raise ValueError("Unknown user application component")
        self.parents, self.quarantines, self.originals, self.moved = {}, {}, {}, []
        self.committed = False
        self.commit_uncertain = False
        self.journal = None
        self.token = uuid.uuid4().hex

    def close(self):
        for _, descriptor in self.quarantines.values():
            os.close(descriptor)
        for descriptor in self.parents.values():
            os.close(descriptor)
        self.quarantines.clear()
        self.parents.clear()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def parent(self, target):
        check_ancestors([target])
        parent = target.parent
        if parent not in self.parents:
            self.parents[parent] = open_directory(parent)
        descriptor = self.parents[parent]
        if identity(os.fstat(descriptor)) != identity(parent.lstat()):
            raise RuntimeError("A removal parent directory moved")
        return descriptor

    def scan(self, parent, name, *, component=None):
        entries, total = 0, 0
        root = os.stat(name, dir_fd=parent, follow_symlinks=False)
        expected_owner = snapshot_owner(
            component, {"owner": root.st_uid}, self.owner, self.user_components
        )

        def walk(directory, entry, depth=0):
            nonlocal entries, total
            entries += 1
            if entries > 4096 or depth > 64:
                raise ValueError("Removal inventory exceeds its entry or depth limit")
            info = os.stat(entry, dir_fd=directory, follow_symlinks=False)
            if info.st_uid != expected_owner or info.st_mode & 0o022:
                raise ValueError("Removal components must be owned and protected")
            is_directory = stat.S_ISDIR(info.st_mode)
            if not is_directory and (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1):
                raise ValueError("Removal inventory rejects links and special files")
            value = {
                "device": info.st_dev,
                "inode": info.st_ino,
                "mode": info.st_mode,
                "owner": info.st_uid,
                "group": info.st_gid,
                "modifiedNs": info.st_mtime_ns,
            }
            flags = (
                os.O_RDONLY | os.O_NOFOLLOW | (os.O_DIRECTORY if is_directory else os.O_NONBLOCK)
            )
            descriptor = os.open(entry, flags, dir_fd=directory)
            try:
                if scan_identity(os.fstat(descriptor)) != scan_identity(info):
                    raise RuntimeError("A removal component changed while opening")
                if is_directory:
                    value["entries"] = {
                        child: walk(descriptor, child, depth + 1)
                        for child in sorted(os.listdir(descriptor))
                    }
                else:
                    if info.st_size > 64 * 1024 * 1024:
                        raise ValueError("Removal component exceeds its file limit")
                    total += info.st_size
                    if total > 256 * 1024 * 1024:
                        raise ValueError("Removal component exceeds its payload limit")
                    digest = hashlib.sha256()
                    remaining = info.st_size
                    while remaining:
                        chunk = os.read(descriptor, min(remaining, 65536))
                        if not chunk:
                            raise RuntimeError("A removal file changed while reading")
                        digest.update(chunk)
                        remaining -= len(chunk)
                    value.update(size=info.st_size, sha256=digest.hexdigest())
                after = os.fstat(descriptor)
                if scan_identity(after) != scan_identity(info):
                    raise RuntimeError("A removal component changed during inventory")
            finally:
                os.close(descriptor)
            return value

        return walk(parent, name)

    def inventory(self):
        result = {}
        for name, target in self.targets.items():
            check_ancestors([target])
            try:
                parent = self.parent(target)
                os.stat(target.name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                result[name] = None
                continue
            result[name] = self.scan(parent, target.name, component=name)
        return result

    def prepare(self):
        if self.originals:
            raise RuntimeError("Removal inventory is already prepared")
        self.originals = self.inventory()
        return self.originals

    def apply(self):
        if not self.originals or self.committed or self.moved:
            raise RuntimeError("Removal must be freshly prepared")
        if self.inventory() != self.originals:
            raise RuntimeError("Removal inventory changed after review")
        for name, target in self.targets.items():
            expected = self.originals[name]
            if expected is None:
                continue
            parent = self.parent(target)
            if target.parent not in self.quarantines:
                folder = target.parent / f".loopz-remove-{self.token}"
                os.mkdir(folder.name, 0o700, dir_fd=parent)
                descriptor = os.open(
                    folder.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent
                )
                info = os.fstat(descriptor)
                if info.st_uid != os.geteuid() or info.st_mode & 0o077:
                    os.close(descriptor)
                    raise RuntimeError("Removal quarantine is not private and owned")
                self.quarantines[target.parent] = (folder, descriptor)
                os.fsync(descriptor)
                os.fsync(parent)
                if self.journal:
                    self.journal.record_quarantine(self, target.parent, descriptor)
            _, quarantine = self.quarantines[target.parent]
            with blocked_signals():
                if self.scan(parent, target.name, component=name) != expected:
                    raise RuntimeError("A removal component changed before quarantine")
                self.moved.append(name)
                try:
                    rename_exclusive(parent, quarantine, target.name)
                except BaseException:
                    self.moved.pop()
                    raise
                os.fsync(parent)
                os.fsync(quarantine)

    def quarantined(self, name):
        target = self.targets[name]
        return target.parent / f".loopz-remove-{self.token}" / target.name

    def rollback(self):
        if self.committed:
            raise RuntimeError("Purged removal cannot be rolled back")
        failures = []
        for name in reversed(self.moved.copy()):
            target = self.targets[name]
            try:
                parent = self.parent(target)
                _, quarantine = self.quarantines[target.parent]
                if self.scan(quarantine, target.name, component=name) != self.originals[name]:
                    raise RuntimeError("The quarantined component changed")
                rename_exclusive(quarantine, parent, target.name)
                self.moved.remove(name)
                os.fsync(parent)
                os.fsync(quarantine)
            except Exception as error:
                failures.append(f"{name}: {error}")
        self.clean_empty_quarantines()
        if failures:
            raise RuntimeError("Removal rollback needs repair: " + "; ".join(failures))

    def clean_empty_quarantines(self):
        for parent_path, (folder, descriptor) in list(self.quarantines.items()):
            parent = self.parent(folder)
            if identity(os.fstat(descriptor)) != identity(folder.lstat()):
                raise RuntimeError("The removal quarantine moved; retained for repair")
            if not os.listdir(descriptor):
                os.rmdir(folder.name, dir_fd=parent)
                os.fsync(parent)
                os.close(descriptor)
                del self.quarantines[parent_path]

    def purge(self, *, allow_partial=False):
        if not self.moved or self.committed:
            raise RuntimeError("Removal has no verified quarantine to purge")
        self.committed = True
        if self.journal:
            self.commit_uncertain = True
            self.journal.begin_purge()
            self.commit_uncertain = False
        failures = []
        for name in self.moved.copy():
            target = self.targets[name]
            _, quarantine = self.quarantines[target.parent]
            try:
                expected = self.originals[name]
                observed = self.scan(quarantine, target.name, component=name)
                if not (
                    remaining_snapshot(observed, expected)
                    if allow_partial
                    else observed == expected
                ):
                    raise RuntimeError("A quarantined component changed before purge")
                remove_at(quarantine, target.name, (expected["device"], expected["inode"]))
                self.moved.remove(name)
                os.fsync(quarantine)
            except Exception as error:
                failures.append(f"{self.quarantined(name)}: {error}")
                try:
                    os.stat(target.name, dir_fd=quarantine, follow_symlinks=False)
                except FileNotFoundError:
                    if name in self.moved:
                        self.moved.remove(name)
                except OSError:
                    pass  # Keep the recorded location if absence is not established.
        try:
            self.clean_empty_quarantines()
        except Exception as error:
            failures.append(str(error))
        return failures


def remaining_snapshot(observed, original):
    """A committed purge may remove entries, but may not introduce or edit any."""
    if "entries" not in original:
        return observed == original
    if "entries" not in observed:
        return False
    if any(
        observed.get(key) != value
        for key, value in original.items()
        if key not in ("entries", "modifiedNs")
    ):
        return False
    return all(
        name in original["entries"] and remaining_snapshot(value, original["entries"][name])
        for name, value in observed["entries"].items()
    )
