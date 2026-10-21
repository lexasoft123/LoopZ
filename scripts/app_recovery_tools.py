"""Freeze only the recovery Python module closure; never copy local configuration."""

import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile

from install_transaction import check_ancestors
from uninstall_files import rename_exclusive, open_directory

SEED = "watch_app_update.py"
BOOTSTRAP = "bootstrap_app_recovery.py"
LIMIT = 16 * 1024 * 1024


def module_closure(source):
    source = Path(source)
    pending = {SEED, BOOTSTRAP}
    result = {}
    while pending:
        name = pending.pop()
        if name in result:
            continue
        path = source / name
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > 1024 * 1024:
            raise ValueError("Recovery modules must be bounded regular files without links")
        encoded = path.read_bytes()
        result[name] = encoded
        if len(result) > 128 or sum(map(len, result.values())) > LIMIT:
            raise ValueError("The recovery module closure exceeds its bound")
        for node in ast.walk(ast.parse(encoded, filename=name)):
            imports = []
            if isinstance(node, ast.Import):
                imports = [entry.name for entry in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                imports = [node.module]
            for module in imports:
                if (
                    re.fullmatch(r"[a-z_][a-z0-9_]*", module)
                    and (source / (module + ".py")).exists()
                ):
                    pending.add(module + ".py")
    return result


def manifest_for(modules):
    value = {name: hashlib.sha256(encoded).hexdigest() for name, encoded in modules.items()}
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def require_private_directory(path, owner):
    check_ancestors([path])
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o077:
        raise ValueError("Recovery files require a private account-owned directory")


def verify_tools(folder, expected, owner):
    check_ancestors([folder])
    info = folder.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o077:
        raise ValueError("Recovery tool directory changed ownership or privacy")
    manifest_path = folder / "manifest.json"
    manifest_info = manifest_path.lstat()
    if (
        not stat.S_ISREG(manifest_info.st_mode)
        or manifest_info.st_uid != owner
        or manifest_info.st_mode & 0o077
        or manifest_info.st_nlink != 1
        or manifest_info.st_size > 32768
    ):
        raise ValueError("Unsafe recovery manifest metadata")
    descriptor = os.open(manifest_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            encoded = stream.read(32769)
    finally:
        os.close(descriptor)
    if len(encoded) > 32768 or hashlib.sha256(encoded).hexdigest() != expected:
        raise ValueError("Recovery tool manifest changed after preparation")
    manifest = json.loads(encoded)
    if (
        not isinstance(manifest, dict)
        or not 2 <= len(manifest) <= 128
        or not {SEED, BOOTSTRAP} <= set(manifest)
        or set(item.name for item in folder.iterdir()) != set(manifest) | {"manifest.json"}
    ):
        raise ValueError("Incomplete recovery tool inventory")
    total = 0
    for name, expected_hash in {**manifest, "manifest.json": expected}.items():
        if name != "manifest.json" and not re.fullmatch(r"[a-z_][a-z0-9_]*\.py", name):
            raise ValueError("Invalid recovery module name")
        path = folder / name
        info = path.lstat()
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != owner
            or info.st_mode & 0o077
            or info.st_nlink != 1
            or info.st_size > 1024 * 1024
        ):
            raise ValueError("Unsafe recovery module metadata")
        content = path.read_bytes()
        total += len(content)
        if total > LIMIT or hashlib.sha256(content).hexdigest() != expected_hash:
            raise ValueError("Recovery module content changed")
    return manifest


def stage_tools(updates, *, source=Path(__file__).parent):
    owner = os.getuid()
    if owner < 501 or os.geteuid() != owner:
        raise ValueError("Recovery tools must be staged without administrator authority")
    cache = Path(updates) / "RecoveryTools"
    require_private_directory(cache, owner)
    modules = module_closure(source)
    encoded = manifest_for(modules)
    digest = hashlib.sha256(encoded).hexdigest()
    destination = cache / digest
    if destination.exists() or destination.is_symlink():
        verify_tools(destination, digest, owner)
        return digest
    temporary = Path(tempfile.mkdtemp(prefix=".stage-", dir=cache))
    parent = open_directory(cache)
    try:
        for name, content in {**modules, "manifest.json": encoded}.items():
            path = temporary / name
            with path.open("xb") as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
        verify_tools(temporary, digest, owner)
        rename_exclusive(parent, parent, temporary.name, digest)
        os.fsync(parent)
    finally:
        os.close(parent)
        if temporary.exists():
            shutil.rmtree(temporary)
    return digest
