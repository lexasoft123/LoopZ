#!/usr/bin/env python3
"""Assemble an explicitly experimental ZIP; never weakens production signing gates."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.2.1"
DRIVER_SHA256 = "73b542a37aafb6f82c7400788de3e98140f0fb3b8652f83224094a15f2585ccf"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-package", required=True, type=Path)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--acceptance", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    for value in (args.commit, args.source_tree):
        if not re.fullmatch(r"[0-9a-f]{40}", value):
            parser.error("An exact reviewed Git commit and tree are required")
    if digest(args.base_package.read_bytes()) != args.expected_sha256.lower():
        raise SystemExit("Native candidate archive checksum differs")
    if args.output.exists():
        raise SystemExit("Use a fresh output directory")
    with zipfile.ZipFile(args.base_package) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise SystemExit("Duplicate ZIP paths")
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or ":" in name or "\\" in name:
                raise SystemExit("Unsafe ZIP path")
            if name.lower().endswith((".pfx", ".p12", ".key")):
                raise SystemExit("Private-key files are forbidden")
        files = {name: archive.read(name) for name in names if not name.endswith("/")}
    manifest = json.loads(files.pop("manifest.json").decode("utf-8-sig"))
    if manifest.get("developmentOnly") is not True:
        raise SystemExit("Development package identity is missing")
    for name, expected in manifest["hashes"].items():
        if name not in files or digest(files[name]) != expected.lower():
            raise SystemExit("Candidate payload checksum differs: " + name)
    if set(files) != set(manifest["hashes"]):
        raise SystemExit("Unverified payload entries")
    required = {
        "LoopZ.exe",
        "loopz-windows-control.exe",
        "loopz-driver-setup.exe",
        "LoopZDeviceBroker.exe",
        "LoopZ-Workbench-Public.cer",
        "driver/LoopZAudio.inf",
        "driver/LoopZAudio.sys",
        "driver/loopzaudio.cat",
        "interface/index.html",
        "interface/loopz.js",
        "interface/loopz.css",
        "THIRD-PARTY-NOTICES.txt",
    }
    if not required.issubset(files):
        raise SystemExit("Incomplete native candidate payload")
    if digest(files["driver/LoopZAudio.sys"]) != DRIVER_SHA256:
        raise SystemExit("Unexpected development driver")
    files = {name: data for name, data in files.items() if not name.startswith("test-tools/")}
    for name in ("Install.cmd", "Remove.cmd", "Experimental-Setup.ps1", "WINDOWS-SETUP.md"):
        files[name] = (ROOT / "packaging/windows/experimental" / name).read_bytes()
    files.pop("WINDOWS-PREVIEW.txt", None)
    files["RELEASE-NOTES.txt"] = (ROOT / "docs/release-notes" / f"v{VERSION}.md").read_bytes()
    acceptance = json.loads(args.acceptance.read_text(encoding="utf-8-sig"))
    files["ACCEPTANCE.json"] = (json.dumps(acceptance, indent=2) + "\n").encode()
    provenance = {
        "version": VERSION,
        "commit": args.commit,
        "sourceTree": args.source_tree,
        "developmentOnly": True,
        "productionSigned": False,
        "nativeCandidateSha256": args.expected_sha256.lower(),
        "driverSha256": DRIVER_SHA256,
    }
    files["PROVENANCE.json"] = (json.dumps(provenance, indent=2) + "\n").encode()
    manifest.update(
        releaseVersion=VERSION,
        commit=args.commit,
        sourceTree=args.source_tree,
        developmentOnly=True,
        productionSigned=False,
        hashes={name: digest(data) for name, data in sorted(files.items())},
    )
    files["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    args.output.mkdir(parents=True)
    target = args.output / f"LoopZ-{VERSION}-windows-x64-experimental.zip"
    with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(target) as archive:
        for name, expected in manifest["hashes"].items():
            if digest(archive.read(name)) != expected:
                raise SystemExit("Final ZIP verification failed: " + name)
    (args.output / "SHA256SUMS.txt").write_text(
        digest(target.read_bytes()) + "  " + target.name + "\n"
    )
    (args.output / "PROVENANCE.json").write_bytes(files["PROVENANCE.json"])
    (args.output / "WINDOWS-SETUP.md").write_bytes(files["WINDOWS-SETUP.md"])
    print(target)


if __name__ == "__main__":
    main()
