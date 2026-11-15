"""Fixed Engine locations; migration is not enabled by this path refactor."""

from pathlib import Path
import stat

LEGACY_ENGINE = Path("/Applications/LoopZ Engine.app")
NEW_ENGINE = Path("/Library/Application Support/LoopZ/LoopZ Engine.app")
ENGINE_TARGET = LEGACY_ENGINE
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
