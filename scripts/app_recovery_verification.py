"""Reuse signature checks only while complete code, resources and metadata match."""

import hashlib
import json
import os
from pathlib import Path
import stat

from desktop_install import bounded_files
from macos_file_attributes import extended_attributes
from user_app_transaction import verify_component


def component_seal(path):
    path = Path(path)
    hashes = bounded_files(path)
    if not hashes:
        raise ValueError("A signed component must have a complete nonempty inventory")
    entries = [path, *sorted(path.rglob("*"))] if path.is_dir() else [path]
    metadata = []
    total_attributes = 0
    for entry in entries:
        info = entry.lstat()
        if (
            not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode))
            or info.st_mode & 0o022
        ):
            raise ValueError("A signed component has unsafe entries or permissions")
        attributes = []
        for name, value in extended_attributes(entry):
            total_attributes += len(value)
            if total_attributes > 4 * 1024 * 1024:
                raise ValueError("A signed component exceeds its metadata bound")
            attributes.append((name.hex(), hashlib.sha256(value).hexdigest()))
        metadata.append(
            (
                "." if entry == path else str(entry.relative_to(path)),
                info.st_dev,
                info.st_ino,
                info.st_mode,
                info.st_uid,
                info.st_gid,
                info.st_nlink,
                getattr(info, "st_flags", 0),
                attributes,
            )
        )
    # Renaming changes ctime and path but does not change a signed image. The
    # complete file hashes, inode identities, modes, flags and xattrs stay bound.
    encoded = json.dumps({"files": hashes, "metadata": metadata}, sort_keys=True).encode()
    return hashlib.sha256(encoded).hexdigest()


class VerifiedAppCache:
    def __init__(self, verify=verify_component):
        self.verify = verify
        self.verified = set()

    def __call__(self, role, path):
        try:
            before = component_seal(path)
        except OSError:
            # An unreadable optional xattr disables the optimization, never the
            # existing full signature verification or journal inventory checks.
            self.verify(role, path)
            return
        key = role, before
        if key in self.verified:
            return
        self.verify(role, path)
        if component_seal(path) != before:
            raise RuntimeError("The signed component changed during verification")
        if len(self.verified) >= 16:
            self.verified.clear()
        self.verified.add(key)
