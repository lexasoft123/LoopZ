"""Strict data-only kernel-generation records; no process or filesystem access."""

from pathlib import Path


def generation_key(process):
    return ".".join(
        str(process[key]) for key in ("pid", "uid", "startSeconds", "startMicroseconds")
    )


def validate_process(process, *, owner, executable=None):
    if not isinstance(process, dict) or set(process) != {
        "pid",
        "uid",
        "startSeconds",
        "startMicroseconds",
        "executable",
    }:
        raise ValueError("Incomplete recorded process generation")
    if any(
        type(process[key]) is not int for key in ("pid", "uid", "startSeconds", "startMicroseconds")
    ):
        raise ValueError("Invalid recorded process identity types")
    if (
        not 1 < process["pid"] < 0x80000000
        or process["uid"] != owner
        or process["startSeconds"] <= 0
        or not 0 <= process["startMicroseconds"] < 1_000_000
    ):
        raise ValueError("Invalid recorded process generation")
    path = process["executable"]
    if (
        not isinstance(path, str)
        or len(path) > 4096
        or "\0" in path
        or not Path(path).is_absolute()
        or ".." in Path(path).parts
        or executable is not None
        and path != str(executable)
    ):
        raise ValueError("Recorded process executable differs from policy")
