"""Retain installed signing envelopes only when all signed code/resources match."""

from pathlib import Path
import os
import plistlib
import re
import shutil
import subprocess
import tempfile
from xml.parsers.expat import ExpatError

from desktop_install import bounded_files


def run(arguments):
    return subprocess.run(arguments, capture_output=True, text=True, check=True)


def executable(path):
    if path.is_file():
        return path, "."
    try:
        info = plistlib.loads((path / "Contents/Info.plist").read_bytes())
    except (plistlib.InvalidFileException, ExpatError) as error:
        raise ValueError("Invalid signed component metadata") from error
    if not isinstance(info, dict):
        raise ValueError("Invalid signed component metadata")
    name = info.get("CFBundleExecutable")
    if not isinstance(name, str) or Path(name).name != name or name in ("", ".", ".."):
        raise ValueError("Invalid signed component executable")
    key = "Contents/MacOS/" + name
    return path / key, key


def code_directories(path, code_requirement, invoke=run):
    invoke(
        [
            "/usr/bin/codesign",
            "--verify",
            "--strict",
            "--all-architectures",
            "-R",
            "=" + code_requirement,
            str(path),
        ]
    )
    binary, _ = executable(path)
    architectures = invoke(["/usr/bin/lipo", "-archs", str(binary)]).stdout.split()
    if (
        not architectures
        or len(architectures) > 8
        or len(set(architectures)) != len(architectures)
        or any(not re.fullmatch(r"[a-z0-9_]{1,16}", item) for item in architectures)
    ):
        raise ValueError("The component architecture list is invalid")
    result = {}
    for architecture in architectures:
        info = invoke(
            ["/usr/bin/codesign", "--display", "--arch", architecture, "--verbose=4", str(path)]
        ).stderr
        digest = re.findall(r"^CandidateCDHashFull sha256=([a-f0-9]{64})$", info, re.M)
        if len(digest) != 1:
            raise ValueError("A full SHA-256 CodeDirectory is required for each architecture")
        result[architecture] = digest[0]
    return result


def reuse_component(candidate, installed, code_requirement, invoke=run):
    """A differing CMS envelope alone must not turn a GUI update into an Engine update.

    Check every architecture and every non-executable file, not just the native
    architecture or a truncated cdhash. Both components must satisfy the fixed
    publisher requirement. Only the private staged candidate is ever replaced.
    """
    candidate, installed = Path(candidate), Path(installed)
    if not installed.exists():
        return False
    replacement_started = False
    try:
        expected = bounded_files(installed)
        built = bounded_files(candidate)
        _, built_key = executable(candidate)
        _, installed_key = executable(installed)
        if built_key != installed_key or {
            key: value for key, value in built.items() if key != built_key
        } != {key: value for key, value in expected.items() if key != installed_key}:
            return False
        if code_directories(candidate, code_requirement, invoke) != code_directories(
            installed, code_requirement, invoke
        ):
            return False
        with tempfile.TemporaryDirectory(
            prefix=".reuse-signed-", dir=candidate.parent
        ) as temporary:
            copied = Path(temporary) / candidate.name
            if installed.is_dir():
                shutil.copytree(installed, copied, symlinks=True)
            else:
                shutil.copy2(installed, copied, follow_symlinks=False)
            if (
                bounded_files(installed) != expected
                or bounded_files(copied) != expected
                or bounded_files(candidate) != built
            ):
                raise RuntimeError("A signed component changed during staging")
            invoke(
                [
                    "/usr/bin/codesign",
                    "--verify",
                    "--strict",
                    "--all-architectures",
                    "-R",
                    "=" + code_requirement,
                    str(copied),
                ]
            )
            replacement_started = True
            if candidate.is_dir():
                shutil.rmtree(candidate)
            else:
                candidate.unlink()
            os.rename(copied, candidate)
        return True
    except (OSError, ValueError, subprocess.SubprocessError):
        if replacement_started:
            raise
        # Missing/untrusted/incompatible installed components are not reused.
        # Keep the freshly signed candidate and its normal update path.
        return False
