"""Experimental ZIP integrity and path-boundary checks; never installs anything."""

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/package_windows_experimental.py"
spec = importlib.util.spec_from_file_location("experimental_package", SCRIPT)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class ExperimentalPackageTests(unittest.TestCase):
    def assemble(self, change=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("Install.cmd", "Remove.cmd", "Experimental-Setup.ps1", "WINDOWS-SETUP.md"):
                path = root / "packaging/windows/experimental" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture")
            notes = root / "docs/release-notes/v0.2.1.md"
            notes.parent.mkdir(parents=True)
            notes.write_text("Experimental fixture")
            driver = b"development driver fixture"
            files = {"driver/LoopZAudio.sys": driver, "LoopZ.exe": b"app"}
            for name in (
                "loopz-windows-control.exe",
                "loopz-driver-setup.exe",
                "LoopZDeviceBroker.exe",
                "LoopZ-Workbench-Public.cer",
                "driver/LoopZAudio.inf",
                "driver/loopzaudio.cat",
                "interface/index.html",
                "interface/loopz.js",
                "interface/loopz.css",
                "THIRD-PARTY-NOTICES.txt",
            ):
                files[name] = b"fixture"
            manifest = {
                "developmentOnly": True,
                "hashes": {name: package.digest(data) for name, data in files.items()},
            }
            if change:
                change(files, manifest)
            candidate = root / "candidate.zip"
            with zipfile.ZipFile(candidate, "w") as archive:
                for name, data in files.items():
                    archive.writestr(name, data)
                archive.writestr("manifest.json", json.dumps(manifest))
            acceptance = root / "acceptance.json"
            acceptance.write_text("{}")
            output = root / "out"
            arguments = [
                str(SCRIPT),
                "--base-package",
                str(candidate),
                "--expected-sha256",
                package.digest(candidate.read_bytes()),
                "--commit",
                "a" * 40,
                "--source-tree",
                "b" * 40,
                "--acceptance",
                str(acceptance),
                "--output",
                str(output),
            ]
            with (
                patch.object(sys, "argv", arguments),
                patch.object(package, "ROOT", root),
                patch.object(package, "DRIVER_SHA256", package.digest(driver)),
            ):
                package.main()
            result = output / "LoopZ-0.2.1-windows-x64-experimental.zip"
            with zipfile.ZipFile(result) as archive:
                final = json.loads(archive.read("manifest.json"))
                self.assertTrue(final["developmentOnly"])
                self.assertFalse(final["productionSigned"])
                self.assertEqual(final["commit"], "a" * 40)
                for name, expected in final["hashes"].items():
                    self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), expected)

    def test_verified_package_contains_consent_setup_and_provenance(self):
        self.assemble()

    def test_missing_native_component_rejected(self):
        def remove(files, manifest):
            files.pop("loopz-driver-setup.exe")
            manifest["hashes"].pop("loopz-driver-setup.exe")

        with self.assertRaises(SystemExit):
            self.assemble(remove)

    def test_changed_file_rejected(self):
        with self.assertRaises(SystemExit):
            self.assemble(lambda files, manifest: files.update({"LoopZ.exe": b"changed"}))

    def test_unverified_file_rejected(self):
        with self.assertRaises(SystemExit):
            self.assemble(lambda files, manifest: files.update({"extra.exe": b"extra"}))

    def test_parent_path_rejected(self):
        with self.assertRaises(SystemExit):
            self.assemble(lambda files, manifest: files.update({"../outside": b"extra"}))

    def test_private_key_rejected(self):
        with self.assertRaises(SystemExit):
            self.assemble(lambda files, manifest: files.update({"secret.pfx": b"private"}))

    def test_missing_development_identity_rejected(self):
        with self.assertRaises(SystemExit):
            self.assemble(lambda files, manifest: manifest.update(developmentOnly=False))


if __name__ == "__main__":
    unittest.main()
