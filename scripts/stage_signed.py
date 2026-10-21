#!/usr/bin/env python3
"""Stage LoopZ using SingZ's disposable Developer ID keychain workflow."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


class SigningError(RuntimeError):
    pass


def security(*arguments):
    # security takes passwords in argv. Never print its arguments, output or
    # CalledProcessError: those can include a certificate/keychain password.
    try:
        result = subprocess.run(
            ["/usr/bin/security", *arguments],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise SigningError(f"security {arguments[0]} could not complete") from None
    if result.returncode:
        raise SigningError(f"security {arguments[0]} failed (exit {result.returncode})")
    return result.stdout


def certificate_password(store):
    value = os.environ.get("LOOPZ_MAC_P12_PASSWORD") or os.environ.get("SINGZ_MAC_P12_PASSWORD")
    if value:
        return value
    if not store or not store.is_file():
        raise SigningError("Set a password store or LOOPZ_MAC_P12_PASSWORD")
    environment = os.environ.copy()
    age_key = Path.home() / ".config/sops/age/keys.txt"
    if not environment.get("SOPS_AGE_KEY_FILE") and age_key.is_file():
        environment["SOPS_AGE_KEY_FILE"] = str(age_key)
    try:
        result = subprocess.run(
            [
                "sops",
                "--config",
                "/dev/null",
                "--decrypt",
                "--output-type",
                "json",
                str(store),
            ],
            capture_output=True,
            text=True,
            env=environment,
            timeout=30,
        )
        values = json.loads(result.stdout) if result.returncode == 0 else {}
        value = values.get("mac_p12_password") if isinstance(values, dict) else None
    except (OSError, subprocess.TimeoutExpired, ValueError):
        value = None
    if not isinstance(value, str) or not value:
        raise SigningError("Cannot read mac_p12_password from the encrypted store")
    return value


@contextmanager
def signing_environment(certificate, password):
    with tempfile.TemporaryDirectory(prefix="loopz-signing-") as directory:
        keychain = str(Path(directory) / "signing.keychain-db")
        keychain_password = secrets.token_urlsafe(32)
        try:
            security("create-keychain", "-p", keychain_password, keychain)
            security("set-keychain-settings", keychain)
            security("unlock-keychain", "-p", keychain_password, keychain)
            security(
                "import",
                str(certificate),
                "-k",
                keychain,
                "-T",
                "/usr/bin/codesign",
                "-T",
                "/usr/bin/productbuild",
                "-P",
                password,
            )
            # SingZ uses the keychain password here, not the .p12 password.
            # This affects only the newly imported, disposable signing key.
            security(
                "set-key-partition-list",
                "-S",
                "apple-tool:,apple:",
                "-s",
                "-k",
                keychain_password,
                keychain,
            )
            # Like SingZ, put this keychain first for identity resolution.
            # --keychain alone can still resolve a duplicate identity's key
            # through the user search list and reach a locked login keychain.
            search_list = shlex.split(security("list-keychains", "-d", "user"))
            security("list-keychains", "-d", "user", "-s", keychain, *search_list)
            identities = security("find-identity", "-v", "-p", "codesigning", keychain)
            all_identities = set(re.findall(r'\b([A-Fa-f0-9]{40}) "([^"]+)"', identities))
            if len(all_identities) != 1 or not next(iter(all_identities))[1].startswith(
                "Developer ID Application: "
            ):
                raise SigningError(
                    "Certificate must contain exactly one Developer ID Application identity"
                )
            fingerprint, _ = next(iter(all_identities))
            environment = os.environ.copy()
            environment.pop("LOOPZ_MAC_P12_PASSWORD", None)
            environment.pop("SINGZ_MAC_P12_PASSWORD", None)
            environment["LOOPZ_ENGINE_SIGNING_IDENTITY"] = fingerprint
            environment["LOOPZ_ENGINE_SIGNING_KEYCHAIN"] = keychain
            yield environment
        finally:
            # create-keychain can add its path to the user's search list;
            # delete-keychain removes that entry without rewriting other entries.
            if Path(keychain).exists():
                security("delete-keychain", keychain)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / ".local/macos-signing.json")
    parser.add_argument("--certificate", type=Path)
    parser.add_argument("--password-store", type=Path)
    parser.add_argument(
        "--routing", action="store_true", help="Stage the new document routing runtime"
    )
    parser.add_argument("staging_arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    config = json.loads(args.config.read_text()) if args.config.is_file() else {}
    certificate = args.certificate or config.get("certificateFile")
    store = args.password_store or config.get("passwordStore")
    if not certificate or not Path(certificate).expanduser().is_file():
        raise SigningError(
            "Select an existing Developer ID .p12 with --certificate or local config"
        )
    certificate = Path(certificate).expanduser().resolve()
    store = Path(store).expanduser().resolve() if store else None
    arguments = args.staging_arguments
    if arguments[:1] == ["--"]:
        arguments = arguments[1:]
    # A signed staging invocation must not accidentally select the ad-hoc path.
    if any(
        arg.split("=", 1)[0] in {"--engine-signing-identity", "--engine-signing-keychain"}
        for arg in arguments
    ):
        raise SigningError("The disposable keychain owns the engine signing identity and keychain")
    with signing_environment(certificate, certificate_password(store)) as environment:
        return subprocess.call(
            [
                sys.executable,
                str(
                    ROOT
                    / "scripts"
                    / ("prepare_routing_install.py" if args.routing else "prepare_install.py")
                ),
                *arguments,
            ],
            env=environment,
        )


if __name__ == "__main__":

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Signing interrupted; no installation was performed", file=sys.stderr)
        sys.exit(130)
    except (SigningError, OSError, ValueError) as error:
        print(f"Signed staging failed: {error}", file=sys.stderr)
        sys.exit(1)
