"""Fixed private diagnostic namespace and process-identity schema."""

from pathlib import Path
import re
from process_record import validate_process


def validate_artifacts(artifacts, owner):
    if (
        not isinstance(artifacts, dict)
        or set(artifacts) != {"work", "guards", "probe"}
        or not isinstance(artifacts["guards"], list)
        or len(artifacts["guards"]) > 8
    ):
        raise ValueError("Invalid diagnostic artifact context")
    work = artifacts["work"]
    if work is None:
        if artifacts["guards"] or artifacts["probe"] is not None:
            raise ValueError("Diagnostics require a recorded private workspace")
        return
    if (
        not isinstance(work, dict)
        or set(work) != {"path", "device", "inode"}
        or not isinstance(work["path"], str)
        or Path(work["path"]).parent != Path("/private/tmp")
        or not re.fullmatch(r"loopz-suite-run-[A-Za-z0-9_-]{6,64}", Path(work["path"]).name)
        or any(type(work[key]) is not int or work[key] < 0 for key in ("device", "inode"))
        or not work["inode"]
    ):
        raise ValueError("Diagnostic workspace is outside its fixed namespace")
    folders = set()
    for guard in artifacts["guards"]:
        if (
            not isinstance(guard, dict)
            or set(guard) != {"folder", "process"}
            or not isinstance(guard["folder"], str)
            or not re.fullmatch(r"guard-[a-f0-9]{32}", guard["folder"])
        ):
            raise ValueError("Invalid recorded continuity guard")
        if guard["folder"] in folders:
            raise ValueError("Duplicate recorded continuity guard")
        folders.add(guard["folder"])
        if guard["process"] is not None:
            validate_process(
                guard["process"],
                owner=owner,
                executable=Path(work["path"]) / guard["folder"] / "loopz-suite-check",
            )
    probe = artifacts["probe"]
    if probe is not None and (
        not isinstance(probe, dict)
        or set(probe) != {"label", "folder"}
        or not isinstance(probe["label"], str)
        or not re.fullmatch(
            r"com\.lexasoft\.loopz\.suite\.validation\.[a-f0-9]{32}", probe["label"]
        )
        or not isinstance(probe["folder"], str)
        or not re.fullmatch(r"validation-[a-f0-9]{32}", probe["folder"])
    ):
        raise ValueError("Invalid recorded validation job")
