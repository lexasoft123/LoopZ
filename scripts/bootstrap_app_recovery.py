"""Verify a frozen toolset before importing any local recovery implementation."""

import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
import sys


def bootstrap(arguments):
    if len(arguments) != 2:
        raise ValueError("Recovery requires a fixed toolset and transaction identity")
    expected, token = arguments
    if not re.fullmatch(r"[a-f0-9]{64}", expected) or not re.fullmatch(r"[a-f0-9]{32}", token):
        raise ValueError("Invalid recovery bootstrap identities")
    folder = Path(__file__).absolute().parent
    owner = os.getuid()
    if owner < 501 or owner != os.geteuid() or folder.name != expected:
        raise ValueError("Recovery bootstrap requires its account-owned frozen toolset")
    namespace = (
        Path(pwd.getpwuid(owner).pw_dir)
        / "Library/Application Support/LoopZ/Updates/RecoveryTools"
        / expected
    )
    if folder != namespace:
        raise ValueError("Recovery tools must use the fixed account cache namespace")
    for parent in [folder, *folder.parents]:
        if parent.is_symlink():
            raise ValueError("Recovery tools cannot use a linked ancestor")
    info = folder.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o077:
        raise ValueError("Recovery tool directory is not private")

    def read(name):
        descriptor = os.open(folder / name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(descriptor)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != owner
                or info.st_mode & 0o077
                or info.st_nlink != 1
                or info.st_size > 1024 * 1024
            ):
                raise ValueError("Recovery module metadata is unsafe")
            with os.fdopen(descriptor, "rb", closefd=False) as stream:
                return stream.read(1024 * 1024 + 1)
        finally:
            os.close(descriptor)

    encoded = read("manifest.json")
    if len(encoded) > 32768 or hashlib.sha256(encoded).hexdigest() != expected:
        raise ValueError("Recovery manifest differs from the launched tool identity")
    manifest = json.loads(encoded)
    if (
        not isinstance(manifest, dict)
        or not 2 <= len(manifest) <= 128
        or not {"bootstrap_app_recovery.py", "watch_app_update.py"} <= set(manifest)
        or set(item.name for item in folder.iterdir()) != set(manifest) | {"manifest.json"}
    ):
        raise ValueError("Recovery tool inventory changed")
    total = 0
    for name, digest in manifest.items():
        if not re.fullmatch(r"[a-z_][a-z0-9_]*\.py", name):
            raise ValueError("Invalid recovery module path")
        content = read(name)
        total += len(content)
        if total > 16 * 1024 * 1024 or hashlib.sha256(content).hexdigest() != digest:
            raise ValueError("Recovery module content changed")
    from watch_app_update import main

    return main(expected, token)


if __name__ == "__main__":
    try:
        raise SystemExit(bootstrap(sys.argv[1:]))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(
            json.dumps(
                {"status": "failed", "mode": "automatic-app-recovery", "error": str(error)}
            )
        )
        raise SystemExit(1) from None
