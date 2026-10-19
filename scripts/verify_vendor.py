#!/usr/bin/env python3
"""Verify the reviewed source snapshot without accessing a sibling checkout."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
lock = json.loads((root / "vendor/singz.lock.json").read_text())
base = root / "vendor/singz"
actual = {
    str(p.relative_to(base)): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in base.rglob("*")
    if p.is_file()
}
if actual != lock["sha256"]:
    raise SystemExit("SingZ snapshot differs from its lock manifest")
print(f"Verified {len(actual)} files at {lock['revision']}")
