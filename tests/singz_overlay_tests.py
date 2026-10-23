"""The SingZ overlay must reproduce exact bytes or reject the SDK revision."""

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CMAKE = sys.argv.pop(1)
OVERLAY = ROOT / "cmake/singz/graph_containment.cmake"
RUNNER = ROOT / "vendor/singz/zdsp/src/runtime/graph_runner.cpp"
EXPECTED = "91a1ca4bcc7c8fbaeac5cec11bcc5a65675bdeab12974fcd790c5ee93f55cfae"


class SingZOverlayTests(unittest.TestCase):
    expected = EXPECTED

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="loopz-singz-overlay-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.input = self.work / "sdk/zdsp/src/runtime/graph_runner.cpp"
        self.input.parent.mkdir(parents=True)
        self.input.write_bytes(RUNNER.read_bytes())
        self.overlay = self.work / "overlay.cmake"
        self.overlay.write_bytes(OVERLAY.read_bytes())
        self.script = self.work / "apply.cmake"
        self.script.write_text(
            'set(SINGZ_ROOT "${CMAKE_CURRENT_LIST_DIR}/sdk")\n'
            'include("${CMAKE_CURRENT_LIST_DIR}/overlay.cmake")\n'
            "loopz_singz_containment_source(result)\n"
        )
        self.output = self.work / "contained-runtime/graph_runner.cpp"

    def apply(self):
        return subprocess.run(
            [CMAKE, "-P", str(self.script)],
            cwd=self.work,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def test_exact_reproducible_source_without_modifying_archive(self):
        original = self.input.read_bytes()
        first = self.apply()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(hashlib.sha256(self.output.read_bytes()).hexdigest(), self.expected)
        timestamp = self.output.stat().st_mtime_ns
        second = self.apply()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.output.stat().st_mtime_ns, timestamp)
        self.assertEqual(self.input.read_bytes(), original)

    def test_different_sdk_is_rejected_before_generating_source(self):
        self.input.write_bytes(self.input.read_bytes() + b"\n")
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match this SDK", result.stderr)
        self.assertFalse(self.output.exists())

    def test_changed_overlay_is_rejected_before_generating_source(self):
        self.overlay.write_text(
            self.overlay.read_text().replace("uint32_t invalid = 0;", "uint32_t invalid = 1;")
        )
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("produced unexpected source", result.stderr)
        self.assertFalse(self.output.exists())


class SingZPlanarOverlayTests(SingZOverlayTests):
    expected = "b32d465b277ad4544856373510456143f5f9e5d1e39cd0b6573a06b28b5ab7ea"

    def setUp(self):
        super().setUp()
        self.input = self.input.with_name("builtin_nodes.cpp")
        self.input.write_bytes(
            (ROOT / "vendor/singz/zdsp/src/runtime/builtin_nodes.cpp").read_bytes()
        )
        self.overlay.write_bytes((ROOT / "cmake/singz/planar_builtins.cmake").read_bytes())
        self.script.write_text(
            'set(SINGZ_ROOT "${CMAKE_CURRENT_LIST_DIR}/sdk")\n'
            'include("${CMAKE_CURRENT_LIST_DIR}/overlay.cmake")\n'
            "loopz_singz_planar_source(result)\n"
        )
        self.output = self.work / "contained-runtime/builtin_nodes.cpp"

    def test_changed_overlay_is_rejected_before_generating_source(self):
        self.overlay.write_text(self.overlay.read_text().replace("0x7f800000U", "0x7f000000U"))
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("produced unexpected source", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
