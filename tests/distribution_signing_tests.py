"""Release admission must reject a weak slice, local time or excess entitlements."""

from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from distribution_signing import require_distribution_signature


class DistributionSigningTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "Component"
        self.path.write_bytes(b"fixture")
        self.operations = []
        self.details = {
            arch: (
                "Identifier=com.lexasoft.loopz.editor\n"
                "TeamIdentifier=USJ7H3X44X\n"
                "Authority=Developer ID Application: Fixture (USJ7H3X44X)\n"
                "CodeDirectory v=20500 size=123 flags=0x10000(runtime) hashes=1\n"
                "Timestamp=Oct 20, 2026 at 12:00:00\n"
                f"CandidateCDHashFull sha256={digest * 64}\n"
            )
            for arch, digest in [("arm64", "a"), ("x86_64", "b")]
        }
        self.entitlements = {"arm64": {}, "x86_64": {}}

    def invoke(self, arguments):
        self.operations.append(arguments)
        stdout, stderr = "", ""
        if arguments[0] == "/usr/bin/lipo":
            stdout = "arm64 x86_64\n"
        elif "--verify" in arguments:
            self.assertIn("--all-architectures", arguments)
            self.assertIn("=fixed publisher requirement", arguments)
        else:
            architecture = arguments[arguments.index("--arch") + 1]
            if "--entitlements" in arguments:
                stdout = plistlib.dumps(self.entitlements[architecture]).decode()
            else:
                stderr = self.details[architecture]
        return subprocess.CompletedProcess(arguments, 0, stdout=stdout, stderr=stderr)

    def check(self, *, engine=False):
        return require_distribution_signature(
            self.path,
            "com.lexasoft.loopz.editor",
            "fixed publisher requirement",
            engine=engine,
            invoke=self.invoke,
        )

    def test_every_slice_is_verified_and_never_claims_notarization(self):
        result = self.check()
        self.assertEqual(set(result["secureTimestamps"]), {"arm64", "x86_64"})
        self.assertEqual(result["codeDirectories"], {"arm64": "a" * 64, "x86_64": "b" * 64})
        self.assertIs(result["notarizationVerified"], False)

    def test_local_signing_time_on_second_slice_is_not_a_timestamp(self):
        self.details["x86_64"] = self.details["x86_64"].replace("Timestamp=", "Signed Time=")
        with self.assertRaisesRegex(ValueError, "timestamped"):
            self.check()

    def test_wrong_publisher_identifier_runtime_or_empty_timestamp_is_rejected(self):
        original = self.details["x86_64"]
        for before, after in [
            ("USJ7H3X44X", "OTHERTEAM1"),
            ("Identifier=com.lexasoft.loopz.editor", "Identifier=other.application"),
            ("Developer ID Application:", "Apple Development:"),
            ("flags=0x10000(runtime)", "flags=0x0(none)"),
            ("Timestamp=Oct 20, 2026 at 12:00:00", "Timestamp=none"),
        ]:
            with self.subTest(after=after):
                self.details["x86_64"] = original.replace(before, after)
                with self.assertRaises(ValueError):
                    self.check()
        self.details["x86_64"] = original

    def test_hidden_debug_entitlement_on_other_slice_is_rejected(self):
        self.entitlements["x86_64"] = {"com.apple.security.get-task-allow": True}
        with self.assertRaisesRegex(ValueError, "entitlements"):
            self.check()

    def test_engine_requires_input_entitlement_on_both_slices(self):
        self.entitlements["arm64"] = {"com.apple.security.device.audio-input": True}
        with self.assertRaisesRegex(ValueError, "entitlements"):
            self.check(engine=True)
        self.entitlements["x86_64"] = dict(self.entitlements["arm64"])
        self.check(engine=True)
        with self.assertRaisesRegex(ValueError, "entitlements"):
            self.check()

    def test_strict_signature_failure_is_not_masked_by_display_metadata(self):
        def invalid(arguments):
            if "--verify" in arguments:
                raise subprocess.CalledProcessError(1, arguments)
            return self.invoke(arguments)

        with self.assertRaises(subprocess.CalledProcessError):
            require_distribution_signature(
                self.path,
                "com.lexasoft.loopz.editor",
                "fixed publisher requirement",
                invoke=invalid,
            )
        self.assertFalse(self.operations)


if __name__ == "__main__":
    unittest.main()
