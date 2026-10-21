"""Admit maintenance only when the account has no pending app replacement."""

from pathlib import Path
import pwd

RECORD = ".app-update-state.json"


def updates_directory(uid):
    return Path(pwd.getpwuid(uid).pw_dir) / "Library/Application Support/LoopZ/Updates"


def app_update_pending(uid):
    folder = updates_directory(uid)
    for name in (RECORD, ".app-recovery-state.json"):
        record = folder / name
        if record.exists() or record.is_symlink():
            return True
    jobs = folder / "RecoveryJobs"
    if jobs.is_symlink():
        return True
    return jobs.exists() and any(jobs.glob("*.permit"))


def require_no_pending_app_update(uid):
    if app_update_pending(uid):
        raise RuntimeError(
            "Recover the interrupted application update first: "
            "run manage_app_update.py --recover as your account, without sudo"
        )
