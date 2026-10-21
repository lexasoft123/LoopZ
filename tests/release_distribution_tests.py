"""Release publication refuses unnotarized or mismatched artifacts before network use."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import publish_release
import notarize_image
import build_release
from types import SimpleNamespace


class DistributionTests(unittest.TestCase):
    def test_packaging_refuses_a_newer_macos_minimum_before_signing(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            build = folder / "build"
            build.mkdir()
            (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
            output = folder / "release"
            with patch.object(
                build_release, "run", return_value=SimpleNamespace(stdout="minos 26.0\n")
            ):
                with self.assertRaisesRegex(ValueError, "target macOS 14.2"):
                    build_release.build(build, output, {})
            self.assertFalse(output.exists())

    def test_publish_refuses_unnotarized_image_before_github(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            image = output / "LoopZ.dmg"
            image.write_bytes(b"not a signed release")
            record = {
                "artifact": str(image),
                "commit": "a" * 40,
                "tag": "v0.1.0",
                "notarized": False,
            }
            (output / "LoopZ-0.1.0-arm64.json").write_text(json.dumps(record))
            with (
                patch.object(sys, "argv", ["publish", "--tag", "v0.1.0", "--output", str(output)]),
                patch.object(subprocess, "check_output", return_value="a" * 40),
                patch.object(subprocess, "run") as run,
            ):
                with self.assertRaisesRegex(SystemExit, "notarized"):
                    publish_release.main()
                run.assert_not_called()

    def test_publish_refuses_artifact_from_another_commit(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "LoopZ-0.1.0-arm64.json").write_text(json.dumps({"commit": "a" * 40}))
            with (
                patch.object(sys, "argv", ["publish", "--tag", "v0.1.0", "--output", str(output)]),
                patch.object(subprocess, "check_output", return_value="b" * 40),
                patch.object(subprocess, "run") as run,
            ):
                with self.assertRaisesRegex(SystemExit, "another commit"):
                    publish_release.main()
                run.assert_not_called()

    def test_notary_refuses_changed_image_without_submitting(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            image = output / "LoopZ.dmg"
            image.write_bytes(b"changed")
            record = output / "release.json"
            record.write_text(json.dumps({"artifact": str(image), "sha256": "a" * 64}))
            with (
                patch.object(
                    sys,
                    "argv",
                    ["notary", "--record", str(record), "--sha256", "a" * 64, "--submit"],
                ),
                patch.object(notarize_image, "invoke") as invoke,
            ):
                with self.assertRaisesRegex(ValueError, "differs"):
                    notarize_image.main()
                invoke.assert_not_called()


if __name__ == "__main__":
    unittest.main()
