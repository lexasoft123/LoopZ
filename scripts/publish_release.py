#!/usr/bin/env python3
"""Publish notarized artifacts and committed release notes, following SingZ's tag flow."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build_release import ROOT, VERSION


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/release-0.2.3")
    parser.add_argument("--prerelease", action="store_true")
    args = parser.parse_args()
    if args.tag != "v" + VERSION:
        raise SystemExit("Tag must match the release version")
    notes = ROOT / "docs/release-notes" / (args.tag + ".md")
    title = notes.read_text().splitlines()[0]
    body = args.output / "release-notes.md"
    body.write_text("\n".join(notes.read_text().splitlines()[2:]) + "\n")
    artifacts = []
    preview = args.prerelease
    for record_path in args.output.glob(f"LoopZ-{VERSION}-*.json"):
        if record_path.name.endswith((".notary.json", ".manifest.json")):
            continue
        record = json.loads(record_path.read_text())
        tagged_commit = subprocess.check_output(
            ["git", "rev-parse", args.tag + "^{commit}"], text=True
        ).strip()
        if record["commit"] != tagged_commit:
            raise SystemExit("Artifact was built from another commit")
        preview = preview or record.get("firstInstallVerified") is not True
        artifact = Path(record["artifact"])
        if (
            record["tag"] != args.tag
            or record.get("sourceDirty") is not False
            or record.get("notarized") is not True
            or hashlib.sha256(artifact.read_bytes()).hexdigest() != record["sha256"]
        ):
            raise SystemExit("Only exact notarized release artifacts can be published")
        public = dict(record, artifact=artifact.name)
        public_path = record_path.with_suffix(".manifest.json")
        public_path.write_text(json.dumps(public, indent=2) + "\n")
        artifacts += [str(artifact), str(public_path)]
    if not artifacts:
        raise SystemExit("No notarized release artifacts are ready")
    checksum = args.output / "SHA256SUMS.txt"
    checksum.write_text(
        "".join(
            f"{hashlib.sha256(Path(path).read_bytes()).hexdigest()}  {Path(path).name}\n"
            for path in artifacts
            if path.endswith(".dmg")
        )
    )
    command = [
        "gh",
        "release",
        "create",
        args.tag,
        "--repo",
        "lexasoft123/LoopZ",
        "--verify-tag",
        "--title",
        title,
        "--notes-file",
        str(body),
        *artifacts,
        str(checksum),
    ]
    if preview:
        command += ["--prerelease", "--latest=false"]
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
