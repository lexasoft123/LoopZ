#!/usr/bin/env python3
"""Prepare or submit an immutable signed update to Apple's notary service.

Submission is opt-in. Status checks use the recorded submission ID and never
resubmit after a timeout. This does not install anything or staple/mutate the
reviewed package. Credentials come from the existing SingZ environment wrapper.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tempfile
import uuid
import zipfile

from desktop_install import IDENTIFIERS, bounded_files, requirement
from distribution_signing import require_distribution_signature
from driver_update import DRIVER_NAME, DRIVER_REQUIREMENT, verify_update
from install_desktop import verify_code
from stage_signed import ROOT


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(arguments, timeout=60):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        # Authentication tools can echo configuration in diagnostics. Persist
        # only a fixed error; never print their command/environment or stderr.
        raise RuntimeError("Release verification or Apple notary operation failed")
    return result


def write_receipt(path, value):
    descriptor, temporary = tempfile.mkstemp(prefix=".notary-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def verify_archive(archive, root_name, expected):
    """The upload must contain exactly the reviewed payload, without metadata extras."""
    actual = {}
    with zipfile.ZipFile(archive) as bundle:
        if len(bundle.infolist()) > 2048:
            raise ValueError("Release archive has too many entries")
        for item in bundle.infolist():
            path = PurePosixPath(item.filename)
            if (
                path.is_absolute()
                or ".." in path.parts
                or not path.parts
                or path.parts[0] != root_name
                or str(path) != item.filename.rstrip("/")
            ):
                raise ValueError("Release archive contains an unexpected path")
            if item.is_dir():
                continue
            relative = str(PurePosixPath(*path.parts[1:]))
            if (
                relative not in expected
                or relative in actual
                or not 0 <= item.file_size <= 64 * 1024 * 1024
                or stat.S_ISLNK(item.external_attr >> 16)
            ):
                raise ValueError("Release archive contains unexpected, linked or duplicate data")
            actual[relative] = hashlib.sha256(bundle.read(item)).hexdigest()
    if actual != expected:
        raise ValueError("Release archive differs from the reviewed payload")
    return len(actual)


def api_auth(environment):
    key = Path(environment.get("SINGZ_ASC_KEY_PATH", ""))
    key_id = environment.get("APP_STORE_CONNECT_API_KEY_ID", "")
    issuer = environment.get("APP_STORE_CONNECT_API_ISSUER_ID", "")
    if not key.is_absolute() or not re.fullmatch(r"[A-Z0-9]{10,32}", key_id):
        raise ValueError("Use the existing SingZ Apple credential wrapper")
    try:
        if str(uuid.UUID(issuer)) != issuer.lower():
            raise ValueError("Invalid issuer")
        info = key.lstat()
    except (ValueError, OSError):
        raise ValueError("Notarization credentials are unavailable") from None
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_mode & 0o077
        or info.st_nlink != 1
        or not 1 <= info.st_size <= 65536
    ):
        raise ValueError("Notarization requires a private regular API key file")
    return ["--key", str(key), "--key-id", key_id, "--issuer", issuer]


def prepare(package, expected_manifest):
    package = package.resolve()
    _, digest = verify_update(package, verify_code, expected_manifest)
    signatures = {}
    for name, identifier in IDENTIFIERS.items():
        signatures["desktop/" + name] = require_distribution_signature(
            package / "desktop" / name,
            identifier,
            requirement(identifier),
            engine=name == "LoopZ Engine.app",
        )
    signatures["driver/" + DRIVER_NAME] = require_distribution_signature(
        package / "driver" / DRIVER_NAME, "com.lexasoft.loopz.driver", DRIVER_REQUIREMENT
    )
    before = bounded_files(package)
    work = Path(tempfile.mkdtemp(prefix="notary-release-", dir=ROOT / "artifacts"))
    archive = work / "LoopZ.zip"
    command(
        [
            "/usr/bin/ditto",
            "-c",
            "-k",
            "--keepParent",
            "--norsrc",
            "--noextattr",
            "--noqtn",
            "--noacl",
            str(package),
            str(archive),
        ]
    )
    if bounded_files(package) != before:
        raise RuntimeError("The release package changed while archiving")
    count = verify_archive(archive, package.name, before)
    receipt = dict(
        schema=1,
        status="prepared",
        package=str(package),
        manifestSHA256=digest,
        archive=str(archive),
        archiveSHA256=sha(archive),
        archiveFileCount=count,
        signatures=signatures,
        installed=False,
        notarizationVerified=False,
        createdAtUTC=datetime.now(timezone.utc).isoformat(),
    )
    path = work / "receipt.json"
    write_receipt(path, receipt)
    return path, receipt


def read_receipt(path):
    value = json.loads(path.read_text())
    if value.get("schema") != 1 or not re.fullmatch(
        r"[a-f0-9]{64}", value.get("archiveSHA256", "")
    ):
        raise ValueError("Invalid notary receipt")
    archive = Path(value.get("archive", ""))
    if archive != path.parent / "LoopZ.zip" or archive.is_symlink() or not archive.is_file():
        raise ValueError("Notary archive is missing or redirected")
    if sha(archive) != value["archiveSHA256"]:
        raise ValueError("Notary archive changed after preparation")
    return value


def verify_submission(value, approved_archive_sha):
    # Approval names exact bytes, not a mutable receipt or a package directory.
    if (
        not re.fullmatch(r"[a-f0-9]{64}", approved_archive_sha or "")
        or approved_archive_sha != value["archiveSHA256"]
    ):
        raise ValueError("Submission requires the reviewed archive SHA-256")
    package = Path(value["package"])
    verify_update(package, verify_code, value["manifestSHA256"])
    verify_archive(Path(value["archive"]), package.name, bounded_files(package))


def submit(path, value, authentication, invoke=command):
    if value.get("status") != "prepared" or value.get("submissionID"):
        raise ValueError("An attempted submission must be checked; it cannot be resubmitted")
    value["status"] = "uploading"
    write_receipt(path, value)
    # No --wait: a specific ID becomes the authority for subsequent checks.
    response = invoke(
        [
            "/usr/bin/xcrun",
            "notarytool",
            "submit",
            value["archive"],
            "--no-wait",
            "--output-format",
            "json",
            *authentication,
        ],
        timeout=90,
    )
    result = json.loads(response.stdout)
    submission = str(uuid.UUID(result["id"]))
    value.update(status="submitted", submissionID=submission)
    write_receipt(path, value)


def check(path, value, authentication, invoke=command):
    submission = str(uuid.UUID(value.get("submissionID", "")))
    response = invoke(
        [
            "/usr/bin/xcrun",
            "notarytool",
            "info",
            submission,
            "--output-format",
            "json",
            *authentication,
        ]
    )
    result = json.loads(response.stdout)
    if str(uuid.UUID(result["id"])) != submission:
        raise ValueError("Apple returned a different submission ID")
    status = result.get("status")
    if status not in ("In Progress", "Accepted", "Invalid", "Rejected"):
        raise ValueError("Unrecognized Apple notarization status")
    value.update(
        status=status,
        notarizationVerified=status == "Accepted",
        checkedAtUTC=datetime.now(timezone.utc).isoformat(),
    )
    write_receipt(path, value)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    sources = parser.add_mutually_exclusive_group(required=True)
    sources.add_argument("--package", type=Path)
    sources.add_argument("--receipt", type=Path)
    parser.add_argument("--manifest-sha")
    parser.add_argument("--archive-sha", help="Exact reviewed archive SHA-256; required to submit")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--submit", action="store_true")
    actions.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.submit and not args.receipt:
        raise ValueError("Prepare and review the archive before submitting its receipt")
    if args.archive_sha and not args.submit:
        raise ValueError("Archive approval applies only to submission")
    if args.package:
        if args.check or not re.fullmatch(r"[a-f0-9]{64}", args.manifest_sha or ""):
            raise ValueError("Preparation requires the reviewed manifest SHA-256")
        path, value = prepare(args.package, args.manifest_sha)
    else:
        if args.manifest_sha or not (args.submit or args.check):
            raise ValueError("An existing receipt requires submission or a status check")
        path = args.receipt.resolve()
        value = read_receipt(path)
    if args.submit:
        verify_submission(value, args.archive_sha)
        submit(path, value, api_auth(os.environ))
    elif args.check:
        check(path, value, api_auth(os.environ))
    print(
        json.dumps(
            dict(
                receipt=str(path),
                status=value["status"],
                submissionID=value.get("submissionID"),
                notarizationVerified=value["notarizationVerified"],
                installed=False,
            )
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError):
        raise SystemExit(
            "Release notarization step failed; no installation was performed"
        ) from None
