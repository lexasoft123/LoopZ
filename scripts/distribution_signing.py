"""Read-only release-signature checks; notarization is a separate acceptance gate."""

import plistlib
import re

from desktop_install import TEAM
from reuse_signed_component import code_directories, run


def require_distribution_signature(path, identifier, requirement, *, engine=False, invoke=run):
    """Check every architecture, not only the architecture of the staging Mac.

    Strict code validation verifies the timestamp token. A local "Signed Time"
    is not a secure timestamp and must not pass this release-only policy.
    This check does not imply Gatekeeper acceptance or a notarization ticket.
    """
    directories = code_directories(path, requirement, invoke)
    timestamps = {}
    for architecture in directories:
        details = invoke(
            ["/usr/bin/codesign", "--display", "--arch", architecture, "--verbose=4", str(path)]
        ).stderr
        timestamp = re.findall(r"^Timestamp=(.+)$", details, re.M)
        authorities = re.findall(r"^Authority=(.+)$", details, re.M)
        if (
            re.findall(r"^Identifier=(.+)$", details, re.M) != [identifier]
            or re.findall(r"^TeamIdentifier=(.+)$", details, re.M) != [TEAM]
            or not authorities
            or not authorities[0].startswith("Developer ID Application:")
            or not re.search(r"^CodeDirectory .*flags=\S*\(runtime\)", details, re.M)
            or len(timestamp) != 1
            or timestamp[0].strip().lower() in ("", "none", "0")
        ):
            raise ValueError(
                f"{path.name} needs a timestamped Developer ID signature for {architecture}"
            )
        timestamps[architecture] = timestamp[0]
        # Entitlements can differ between Mach-O slices. Check them separately.
        encoded = invoke(
            [
                "/usr/bin/codesign",
                "--display",
                "--arch",
                architecture,
                "--entitlements",
                "-",
                "--xml",
                str(path),
            ]
        ).stdout
        entitlements = plistlib.loads(encoded.encode()) if encoded.strip() else {}
        expected = {"com.apple.security.device.audio-input": True} if engine else {}
        if entitlements != expected:
            raise ValueError(f"{path.name} has unexpected release entitlements for {architecture}")
    return {
        "codeDirectories": directories,
        "secureTimestamps": timestamps,
        "notarizationVerified": False,
    }
