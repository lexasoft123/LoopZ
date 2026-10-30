#!/usr/bin/env python3
"""Verify the pinned zCore submodule without fetching or modifying it."""

from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
subprocess.run(
    [
        sys.executable,
        str(root / "scripts/verify-zcore-submodule.py"),
        "--root",
        str(root),
        "--path",
        "vendor/zcore",
        "--print-revision",
    ],
    check=True,
)
