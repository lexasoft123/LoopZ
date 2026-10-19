#!/usr/bin/env python3
"""Format LoopZ sources, or check their formatting without changing files."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CLANG_FORMAT_VERSION = "22.1.8"
NATIVE_EXTENSIONS = {".c", ".cpp", ".h", ".hpp", ".mm"}


def clang_format_path():
    candidates = [
        os.environ.get("CLANG_FORMAT"),
        shutil.which("clang-format"),
        "/opt/homebrew/opt/llvm/bin/clang-format",
        "/usr/local/opt/llvm/bin/clang-format",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            version = subprocess.check_output([candidate, "--version"], text=True)
            if re.search(rf"\bversion {re.escape(CLANG_FORMAT_VERSION)}\b", version):
                return candidate
    raise RuntimeError(
        f"Install clang-format {CLANG_FORMAT_VERSION}, or set CLANG_FORMAT to its executable."
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Report changes without writing files")
    options = parser.parse_args()

    native_files = sorted(
        str(path.relative_to(ROOT))
        for folder in ("native", "tests")
        for path in (ROOT / folder).rglob("*")
        if path.suffix in NATIVE_EXTENSIONS
    )
    native_flags = ["--dry-run", "--Werror"] if options.check else ["-i"]
    subprocess.run([clang_format_path(), *native_flags, *native_files], cwd=ROOT, check=True)

    local_python = ROOT / "build/format-tools/bin/python"
    python = str(local_python) if local_python.is_file() else sys.executable
    python_flags = ["--check"] if options.check else []
    subprocess.run([python, "-m", "black", *python_flags, "scripts", "tests"], cwd=ROOT, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"Formatting failed: {error}", file=sys.stderr)
        sys.exit(1)
