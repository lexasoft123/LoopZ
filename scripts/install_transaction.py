"""File-only installation primitives, also exercised against temporary targets."""

import hashlib
import fcntl
from contextlib import contextmanager
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import uuid

NAMES = (
    "LoopZAudio.driver",
    "LoopZ Engine.app",
    "loopz-broker",
    "loopz-probe",
    "loopz-install-check",
    "com.lexasoft.loopz.bridge.plist",
)


def files(path):
    path = Path(path)
    if path.is_symlink():
        raise ValueError(f"Symlinks are not accepted: {path}")
    if not path.exists():
        return {}
    entries = [path] if path.is_file() else sorted(path.rglob("*"))
    result = {}
    for entry in entries:
        if entry.is_symlink() or not (entry.is_file() or entry.is_dir()):
            raise ValueError(f"Unsupported installation entry: {entry}")
        if entry.is_file():
            key = str(entry.relative_to(path)) if path.is_dir() else "."
            result[key] = hashlib.sha256(entry.read_bytes()).hexdigest()
    return result


def verify_manifest(stage, manifest):
    allowed = set(NAMES) | {"com.lexasoft.loopz.engine.plist"}
    expected = manifest.get("sha256", {})
    if not expected or manifest.get("protocol") != 1:
        raise ValueError("Missing manifest hashes or incompatible transport protocol")
    actual = files(stage)
    actual.pop("manifest.json", None)
    for name, digest in expected.items():
        relative = PurePosixPath(name)
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or not relative.parts
            or relative.parts[0] not in allowed
        ):
            raise ValueError("Manifest contains an unsupported installation path")
        if actual.get(name) != digest:
            raise ValueError(f"Prepared artifact changed: {name}")
    if actual != expected:
        raise ValueError("Prepared files differ from the complete manifest")
    required = [
        "LoopZAudio.driver/Contents/MacOS/LoopZAudio",
        "LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
        *NAMES[2:],
    ]
    if any(name not in expected for name in required):
        raise ValueError("Manifest is missing an installation component")


def compatible_driver(stage, installed):
    # Comparing the complete sealed bundle also catches entitlement/plist and
    # resource changes, not only changes to the Mach-O executable.
    return bool(files(installed)) and files(stage) == files(installed)


def validate_after(before, after, service_before, service_after):
    if not after.get("loopzVisible") or not after.get("loopzAlive"):
        raise RuntimeError(
            "LoopZ did not activate without a restart; installation cannot be certified"
        )
    for key in ("defaultInput", "defaultOutput", "defaultSystemOutput"):
        if before[key] != after[key]:
            raise RuntimeError(f"Sound device selection changed during installation: {key}")
    if not service_before or service_before != service_after:
        raise RuntimeError("Core Audio service identity changed during installation")


class FileTransaction:
    """Prepare all sibling replacements first; retain originals until verified."""

    def __init__(self, stage, targets, names, replace=os.replace):
        self.stage, self.targets, self.names = Path(stage), targets, names
        self.replace = replace
        self.token = uuid.uuid4().hex
        self.candidates, self.backups, self.changed = {}, {}, []

    def prepare(self):
        check_ancestors(self.targets.values())
        try:
            for name in self.names:
                target = self.targets[name]
                target.parent.mkdir(parents=True, exist_ok=True)
                candidate = target.parent / f".{target.name}.loopz-new-{self.token}"
                self.candidates[name] = candidate
                source = self.stage / name
                if source.is_dir():
                    shutil.copytree(source, candidate, symlinks=True)
                else:
                    shutil.copy2(source, candidate, follow_symlinks=False)
                files(candidate)  # reject unexpected links, including copy races
                for entry in (
                    [candidate, *candidate.rglob("*")] if candidate.is_dir() else [candidate]
                ):
                    executable = entry.is_dir() or bool(entry.stat().st_mode & 0o111)
                    if os.geteuid() == 0:
                        os.chown(entry, 0, 0)
                    os.chmod(entry, 0o755 if executable else 0o644)
        except BaseException:
            self.finish()
            raise

    def apply(self):
        check_ancestors(self.targets.values())
        for name in self.names:
            with blocked_signals():
                self._promote(name)

    def _promote(self, name):
        target = self.targets[name]
        if target.is_symlink():
            raise ValueError(f"Refusing to replace a symlink: {target}")
        backup = target.parent / f".{target.name}.loopz-old-{self.token}"
        if target.exists():
            self.replace(target, backup)
            self.backups[name] = backup
        # Track before promoting, so failure of the second rename restores
        # the first rename's backup too.
        self.changed.append(name)
        self.replace(self.candidates[name], target)

    def rollback(self):
        errors = []
        for name in reversed(self.changed):
            target = self.targets[name]
            try:
                remove(target)
                if name in self.backups:
                    self.replace(self.backups[name], target)
            except Exception as error:
                errors.append(f"{name}: {error}")
        if errors:
            raise RuntimeError("Rollback needs repair; backups retained: " + "; ".join(errors))
        self.changed.clear()

    def finish(self):
        for path in [*self.candidates.values(), *self.backups.values()]:
            remove(path)


def remove(path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def check_ancestors(targets):
    for target in targets:
        for path in [target, *target.parents]:
            if path.is_symlink():
                raise ValueError(f"Symlink in installation destination: {path}")


@contextmanager
def exclusive_lock(path, *, readable=False):
    check_ancestors([path])
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if info.st_uid != os.geteuid() or info.st_mode & 0o022 or info.st_nlink != 1:
            raise ValueError("Installer lock has unsafe ownership or permissions")
        if readable:
            if os.geteuid() != 0:
                raise ValueError("Only the system installer may publish a shared update lock")
            os.fchmod(fd, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another LoopZ installation is already running") from error
        yield
    finally:
        os.close(fd)


@contextmanager
def blocked_signals():
    previous = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
    try:
        yield
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)
