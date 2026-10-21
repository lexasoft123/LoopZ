"""Same-code staging must preserve every architecture and resource seal."""

import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from reuse_signed_component import reuse_component
from install_transaction import files


class ReuseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.candidate = self.root / "staged.app"
        self.installed = self.root / "installed.app"
        for path, envelope in (
            (self.candidate, "new envelope"),
            (self.installed, "installed envelope"),
        ):
            (path / "Contents/MacOS").mkdir(parents=True)
            (path / "Contents/Info.plist").write_bytes(
                plistlib.dumps({"CFBundleExecutable": "Engine"})
            )
            (path / "Contents/MacOS/Engine").write_text(envelope)
            (path / "Contents/resource").write_text("same resource")
        self.before = files(self.installed)
        self.architectures = {"candidate": ["arm64", "x86_64"], "installed": ["arm64", "x86_64"]}
        self.digests = {
            "candidate": {"arm64": "a" * 64, "x86_64": "b" * 64},
            "installed": {"arm64": "a" * 64, "x86_64": "b" * 64},
        }
        self.reject = False
        self.seen = []

    def run_tool(self, args):
        self.seen.append(args)
        location = Path(args[-1])
        side = "installed" if self.installed in [location, *location.parents] else "candidate"
        if args[0] == "/usr/bin/lipo":
            return SimpleNamespace(stdout=" ".join(self.architectures[side]), stderr="")
        if "--verify" in args:
            if side == "installed" and self.reject:
                raise subprocess.CalledProcessError(1, args)
            return SimpleNamespace(stdout="", stderr="")
        arch = args[args.index("--arch") + 1]
        return SimpleNamespace(
            stdout="", stderr="CandidateCDHashFull sha256=" + self.digests[side][arch] + "\n"
        )

    def reuse(self):
        return reuse_component(self.candidate, self.installed, "fixed publisher", self.run_tool)

    def test_new_envelope_same_all_arch_code_reuses_complete_installed_bytes(self):
        self.assertTrue(self.reuse())
        self.assertEqual(files(self.candidate), self.before)
        self.assertEqual(files(self.installed), self.before)
        self.assertEqual(
            {args[args.index("--arch") + 1] for args in self.seen if "--arch" in args},
            {"arm64", "x86_64"},
        )

    def test_changed_non_native_architecture_is_not_reused(self):
        self.digests["candidate"]["x86_64"] = "c" * 64
        self.assertFalse(self.reuse())
        self.assertNotEqual(files(self.candidate), self.before)

    def test_architecture_removal_is_not_reused(self):
        self.architectures["candidate"] = ["arm64"]
        self.assertFalse(self.reuse())

    def test_resource_or_plist_change_is_not_reused(self):
        for name in ("resource", "Info.plist"):
            with self.subTest(name=name):
                file = self.candidate / "Contents" / name
                original = file.read_bytes()
                file.write_bytes(original + b"changed")
                self.assertFalse(self.reuse())
                file.write_bytes(original)

    def test_untrusted_installed_code_is_not_reused(self):
        self.reject = True
        self.assertFalse(self.reuse())
        self.assertNotEqual(files(self.candidate), self.before)

    def test_standalone_login_helper_can_retain_its_exact_envelope(self):
        import shutil

        shutil.rmtree(self.candidate)
        shutil.rmtree(self.installed)
        self.candidate.write_text("new helper envelope")
        self.installed.write_text("installed helper envelope")
        self.assertTrue(self.reuse())
        self.assertEqual(self.candidate.read_bytes(), self.installed.read_bytes())

    def test_symlinked_component_is_never_reused(self):
        import shutil

        shutil.rmtree(self.installed)
        self.installed.symlink_to(self.candidate, target_is_directory=True)
        self.assertFalse(self.reuse())


if __name__ == "__main__":
    unittest.main()
