"""Descriptor-scoped protected JSON records for fixed update recovery namespaces.

Callers hold the shared installation lock. Subclasses validate data-only schemas;
paths and commands are never taken from a stored record. Only these fixed
record names are supported, and closing an object preserves unfinished work.
"""

import copy
import fcntl
import json
import os
from pathlib import Path
import stat
import uuid

from desktop_files import identity
from suite_update import unique_object
from uninstall_files import open_directory, rename_exclusive, scan_identity

LIMIT = 32 * 1024 * 1024
RECORD_NAMES = {
    ".suite-update-state.json",
    ".suite-driver-state.json",
    ".runtime-context.json",
    ".app-update-state.json",
    ".app-recovery-state.json",
    ".app-recovery-ready.json",
}


class ProtectedRecord:
    def __init__(self, folder, record_name, *, owner=None):
        self.folder = Path(folder)
        if record_name not in RECORD_NAMES:
            raise ValueError("Unsupported update recovery record namespace")
        self.record_name = record_name
        self.owner = os.geteuid() if owner is None else owner
        self.directory = open_directory(self.folder)
        info = os.fstat(self.directory)
        if info.st_uid != self.owner or info.st_mode & 0o022:
            os.close(self.directory)
            raise ValueError("The update journal directory must be protected and owned")
        self.folder_identity = identity(info)
        self.value = None
        self.file_identity = None

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
            info = os.stat(self.record_name, dir_fd=self.directory, follow_symlinks=False)
        except FileNotFoundError:
            return False
        self.require_record(info)
        return True

    def require_record(self, info):
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != self.owner
            or info.st_mode & 0o077
            or info.st_nlink != 1
            or info.st_size > LIMIT
        ):
            raise ValueError("Update state must be a protected bounded regular file")

    def require_directory(self):
        current = open_directory(self.folder)
        try:
            if identity(os.fstat(current)) != self.folder_identity:
                raise RuntimeError("The update journal directory was replaced")
        finally:
            os.close(current)

    def save(self, value, *, initial=False):
        self.require_directory()
        self.validate(value)
        encoded = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
        if len(encoded) > LIMIT:
            raise ValueError("Update state exceeds its payload limit")
        temporary = f".suite-update-{uuid.uuid4().hex}.tmp"
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
                rename_exclusive(self.directory, self.directory, temporary, self.record_name)
            else:
                if identity(
                    os.stat(self.record_name, dir_fd=self.directory, follow_symlinks=False)
                ) != (self.file_identity):
                    raise RuntimeError("Update state changed before its next durable write")
                os.rename(
                    temporary,
                    self.record_name,
                    src_dir_fd=self.directory,
                    dst_dir_fd=self.directory,
                )
            self.file_identity = identity(os.fstat(descriptor))
            self.value = copy.deepcopy(value)
            os.fsync(self.directory)
            fcntl.fcntl(descriptor, fcntl.F_FULLFSYNC)
        finally:
            os.close(descriptor)
            try:
                os.unlink(temporary, dir_fd=self.directory)
            except FileNotFoundError:
                pass

    def load(self):
        self.require_directory()
        if not self.pending():
            raise RuntimeError("No interrupted coordinated update was found")
        descriptor = os.open(
            self.record_name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.directory
        )
        try:
            before = os.fstat(descriptor)
            self.require_record(before)
            with os.fdopen(descriptor, "rb", closefd=False) as source:
                encoded = source.read(LIMIT + 1)
            if len(encoded) > LIMIT or scan_identity(os.fstat(descriptor)) != scan_identity(before):
                raise RuntimeError("Update state changed while reading")
            if identity(
                os.stat(self.record_name, dir_fd=self.directory, follow_symlinks=False)
            ) != identity(before):
                raise RuntimeError("Update state was replaced while reading")
            value = json.loads(encoded, object_pairs_hook=unique_object)
            self.validate(value)
            self.value, self.file_identity = value, identity(before)
            return copy.deepcopy(value)
        finally:
            os.close(descriptor)

    def remove_record(self):
        self.require_directory()
        if (
            identity(os.stat(self.record_name, dir_fd=self.directory, follow_symlinks=False))
            != self.file_identity
        ):
            raise RuntimeError("Recovery record changed before finalization")
        os.unlink(self.record_name, dir_fd=self.directory)
        os.fsync(self.directory)
