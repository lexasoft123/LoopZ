#!/usr/bin/env python3
"""Adapt SingZ's disposable signing/notary credentials to the native release build."""
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    needed = [
        "APPLE_DEVELOPER_ID_CERTIFICATE_BASE64",
        "APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD",
        "APP_STORE_CONNECT_API_KEY_BASE64",
        "APP_STORE_CONNECT_API_KEY_ID",
        "APP_STORE_CONNECT_API_ISSUER_ID",
    ]
    if any(not os.environ.get(key) for key in needed):
        raise SystemExit(
            "Configure the five SingZ-compatible Apple signing secrets before releasing"
        )
    with tempfile.TemporaryDirectory(prefix="loopz-release-secrets-") as temporary:
        folder = Path(temporary)
        folder.chmod(0o700)
        certificate = folder / "certificate.p12"
        key = folder / "AuthKey.p8"
        for path, variable in [(certificate, needed[0]), (key, needed[2])]:
            path.touch(mode=0o600)
            path.write_bytes(base64.b64decode(os.environ[variable], validate=True))
        config = folder / "signing.json"
        config.write_text(json.dumps({"certificateFile": str(certificate)}))
        environment = os.environ.copy()
        environment["LOOPZ_MAC_P12_PASSWORD"] = os.environ[needed[1]]
        environment["SINGZ_ASC_KEY_PATH"] = str(key)
        output = ROOT / "artifacts/release-0.1.0"
        subprocess.run(
            [
                sys.executable,
                ROOT / "scripts/build_release.py",
                "--build",
                ROOT / "build/release",
                "--output",
                output,
                "--config",
                config,
            ],
            check=True,
            env=environment,
        )
        records = list(output.glob("LoopZ-0.1.0-*.json"))
        if len(records) != 1:
            raise SystemExit("Expected one release architecture per job")
        record = records[0]
        digest = json.loads(record.read_text())["sha256"]
        args = [
            sys.executable,
            ROOT / "scripts/notarize_image.py",
            "--record",
            record,
            "--sha256",
            digest,
        ]
        subprocess.run([*args, "--submit"], check=True, env=environment)
        for _ in range(60):
            subprocess.run(args, check=True, env=environment)
            receipt = json.loads(record.with_suffix(".notary.json").read_text())
            if receipt["status"] == "Accepted":
                return
            if receipt["status"] == "Invalid":
                raise SystemExit("Apple rejected this release; inspect the notarization log")
            time.sleep(10)
        raise SystemExit("Notarization is pending; resume from its recorded submission ID")


if __name__ == "__main__":
    main()
