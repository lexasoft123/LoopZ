#!/usr/bin/env python3
"""Submit an exact signed DMG once; resume status checks without resubmitting."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def write(path, value):
    fd, temporary = tempfile.mkstemp(prefix=".notary-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def invoke(arguments):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise RuntimeError(
            "Apple notarization operation failed; retry status using the saved receipt"
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--submit", action="store_true")
    args = parser.parse_args()
    record = json.loads(args.record.read_text())
    image = Path(record["artifact"])
    if (
        not re.fullmatch(r"[a-f0-9]{64}", args.sha256)
        or image.is_symlink()
        or not image.is_file()
        or record["sha256"] != args.sha256
        or hashlib.sha256(image.read_bytes()).hexdigest() != args.sha256
    ):
        raise ValueError("DMG differs from the reviewed release artifact")
    receipt = args.record.with_suffix(".notary.json")
    state = (
        json.loads(receipt.read_text())
        if receipt.exists()
        else {"sha256": args.sha256, "status": "prepared"}
    )
    if state["sha256"] != args.sha256:
        raise ValueError("Notarization receipt names another artifact")
    auth = [
        "--key",
        os.environ["SINGZ_ASC_KEY_PATH"],
        "--key-id",
        os.environ["APP_STORE_CONNECT_API_KEY_ID"],
        "--issuer",
        os.environ["APP_STORE_CONNECT_API_ISSUER_ID"],
    ]
    command = ["/usr/bin/xcrun", "notarytool"]
    if args.submit:
        if state.get("submissionID") or state["status"] != "prepared":
            raise ValueError("This image was already submitted; check its saved receipt")
        invoke(
            [
                "/usr/bin/codesign",
                "--verify",
                "--strict",
                "-R",
                '=anchor apple generic and certificate leaf[subject.OU] = "USJ7H3X44X" and certificate leaf[field.1.2.840.113635.100.6.1.13] exists',
                str(image),
            ]
        )
        state["status"] = "submitting"
        write(receipt, state)
        result = json.loads(
            invoke([*command, "submit", str(image), *auth, "--output-format", "json"]).stdout
        )
        state.update(submissionID=result["id"], status="submitted")
    else:
        if not state.get("submissionID"):
            raise ValueError("No submission ID; do not resubmit an unknown outcome")
        result = json.loads(
            invoke(
                [*command, "info", state["submissionID"], *auth, "--output-format", "json"]
            ).stdout
        )
        state["status"] = result["status"]
    write(receipt, state)
    if state["status"] == "Accepted":
        invoke(["/usr/bin/xcrun", "stapler", "staple", str(image)])
        invoke(["/usr/bin/xcrun", "stapler", "validate", str(image)])
        invoke(
            [
                "/usr/sbin/spctl",
                "--assess",
                "--type",
                "open",
                "--context",
                "context:primary-signature",
                str(image),
            ]
        )
        record["notarized"] = True
        record["submissionSHA256"] = args.sha256
        record["sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
        record["submissionID"] = state["submissionID"]
        write(args.record, record)
    print(
        json.dumps(
            {
                "receipt": str(receipt),
                "status": state["status"],
                "submissionID": state.get("submissionID"),
            }
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, RuntimeError, subprocess.SubprocessError):
        raise SystemExit(
            "Notarization did not finish. See the saved receipt; do not submit it again."
        ) from None
