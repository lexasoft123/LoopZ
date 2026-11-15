"""Fixed Engine locations; only explicit audio maintenance migrates installations."""

from pathlib import Path
import stat
import os
import plistlib
import pwd

LEGACY_ENGINE = Path("/Applications/LoopZ Engine.app")
NEW_ENGINE = Path("/Library/Application Support/LoopZ/LoopZ Engine.app")
ENGINE_TARGET = NEW_ENGINE
ENGINE_EXECUTABLE = str(ENGINE_TARGET / "Contents/MacOS/LoopZ Engine")


def read_engine_path(preferred=NEW_ENGINE, legacy=LEGACY_ENGINE):
    """Read a complete real bundle, otherwise keep the repairable legacy target."""
    try:
        for directory in (preferred, preferred / "Contents", preferred / "Contents/MacOS"):
            if not stat.S_ISDIR(directory.lstat().st_mode):
                return legacy
        mode = (preferred / "Contents/MacOS/LoopZ Engine").lstat().st_mode
        return preferred if stat.S_ISREG(mode) and mode & 0o111 else legacy
    except OSError:
        return legacy


def account_engine_job():
    """The console account owns the user job, including in root maintenance scripts."""
    try:
        uid = Path("/dev/console").stat().st_uid
        if uid < 501:
            uid = os.getuid()
    except FileNotFoundError:
        uid = os.getuid()
    return Path(pwd.getpwuid(uid).pw_dir) / "Library/Application Support/LoopZ/Session/engine.plist"


def installed_engine_path(preferred=NEW_ENGINE, legacy=LEGACY_ENGINE, job=None):
    """Honor this account's job before choosing an existing, repairable bundle."""
    selected_job = job
    if selected_job is None:
        selected_job = (
            account_engine_job()
            if (preferred, legacy) == (NEW_ENGINE, LEGACY_ENGINE)
            else preferred.parent / "Session/engine.plist"
        )
    try:
        descriptor = os.open(selected_job, os.O_RDONLY | os.O_NOFOLLOW)
    except FileNotFoundError:
        descriptor = None
    if descriptor is not None:
        with os.fdopen(descriptor, "rb") as source:
            info = os.fstat(source.fileno())
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_nlink != 1
                or not 0 < info.st_size <= 65536
            ):
                raise ValueError("Invalid account Engine job")
            stored = plistlib.loads(source.read(65537))
        arguments = stored.get("ProgramArguments") if isinstance(stored, dict) else None
        if not isinstance(arguments, list) or not arguments:
            raise ValueError("Invalid account Engine job arguments")
        for path in (preferred, legacy):
            if arguments[0] == str(path / "Contents/MacOS/LoopZ Engine"):
                return path
        raise ValueError("Unknown account Engine job path")
    for path in (preferred, legacy):
        try:
            if stat.S_ISDIR(path.lstat().st_mode):
                return path
        except FileNotFoundError:
            pass
    return preferred


def installed_engine_executable():
    return str(installed_engine_path() / "Contents/MacOS/LoopZ Engine")


class _LazyInstalledEnginePath(os.PathLike):
    """Resolve once on first path use, never while module inventories import.

    Keeping one resolution per inventory also prevents a transaction's target
    from changing halfway through a job update. Derived TARGETS dicts can copy
    this path-like value without probing another account's home directory.
    """

    def __init__(self):
        self._path = None

    def _resolve(self):
        if self._path is None:
            self._path = installed_engine_path()
        return self._path

    def __fspath__(self):
        return os.fspath(self._resolve())

    def __str__(self):
        return str(self._resolve())

    def __repr__(self):
        return "<installed Engine path; unresolved>" if self._path is None else repr(self._path)

    def __truediv__(self, other):
        return self._resolve() / other

    def __getattr__(self, name):
        return getattr(self._resolve(), name)

    def __eq__(self, other):
        return self._resolve() == other

    def __hash__(self):
        return hash(self._resolve())


def lazy_installed_engine_path():
    return _LazyInstalledEnginePath()
