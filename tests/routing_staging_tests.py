#!/usr/bin/env python3
"""Routing package trust and publication checks without private keys or installation."""

import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import prepare_routing_install as staging


class RoutingStagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="loopz-routing-staging-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.output = self.root / "package"
        self.document = self.root / "user-routes.json"
        self.signed = []
        for name in staging.COMPONENTS:
            path = self.build / name
            if path.suffix in (".app", ".driver"):
                (path / "Contents").mkdir(parents=True)
                (path / "Contents/Info.plist").write_bytes(
                    plistlib.dumps(
                        {
                            "AudioServerPlugIn_MachServices": [staging.SERVICE],
                        }
                    )
                )
            else:
                path.write_text("test build " + name)

    def sign(self, path, identifier, identity, keychain, engine=False):
        self.assertEqual(engine, path.name == "LoopZ Engine.app")
        if path.name == "LoopZRoutingAudio.driver":
            pin = json.loads((path / "Contents/Resources/routing-service.json").read_text())
            self.assertEqual(
                pin["brokerRequirement"],
                staging.stable_requirement(
                    staging.COMPONENTS["loopz-routing-broker"], "TESTTEAM01"
                ),
            )
        self.signed.append(path.name)
        return {
            "identifier": identifier,
            "team": "TESTTEAM01",
            "requirement": staging.stable_requirement(identifier, "TESTTEAM01"),
        }

    def native_check(self, tool, binary):
        pin = json.loads((binary.parents[1] / "Resources/routing-service.json").read_text())
        return subprocess.CompletedProcess(
            [],
            0,
            json.dumps(
                {
                    "brokerRequirement": pin["brokerRequirement"],
                    "service": staging.SERVICE,
                    "serverUID": 0,
                    "privileged": True,
                }
            ),
            "",
        )

    def stage(self):
        return staging.stage(self.build, self.output, self.document, "test-identity")

    def test_seals_pin_before_driver_signature_and_publishes_verified_package(self):
        with (
            patch.object(staging, "sign", self.sign),
            patch.object(staging, "command", self.native_check),
        ):
            result = self.stage()
        self.assertEqual(self.signed[-1], "LoopZRoutingAudio.driver")
        self.assertEqual(set(self.signed), set(staging.COMPONENTS))
        pin = (self.output / "routing-service.json").read_bytes()
        self.assertEqual(
            pin,
            (
                self.output / "LoopZRoutingAudio.driver/Contents/Resources/routing-service.json"
            ).read_bytes(),
        )
        daemon = plistlib.loads((self.output / "com.lexasoft.loopz.routing.plist").read_bytes())
        agent = plistlib.loads((self.output / "com.lexasoft.loopz.engine.plist").read_bytes())
        self.assertEqual(daemon["MachServices"], {staging.SERVICE: True})
        self.assertEqual(
            daemon["ProgramArguments"][-1], result["signatures"]["LoopZ Engine.app"]["requirement"]
        )
        self.assertEqual(agent["ProgramArguments"][-2:], ["--routing-document", str(self.document)])
        self.assertTrue(agent["KeepAlive"])
        self.assertFalse(result["installed"])
        self.assertFalse(result["loginRegistered"])
        self.assertFalse(self.document.exists())
        self.assertFalse((self.build / "LoopZRoutingAudio.driver/Contents/Resources").exists())
        self.assertEqual(
            set(result["files"]),
            {
                str(path.relative_to(self.output))
                for path in self.output.rglob("*")
                if path.is_file() and path.name != "manifest.json"
            },
        )

    def test_wrong_driver_service_prevents_driver_signing_and_publication(self):
        path = self.build / "LoopZRoutingAudio.driver/Contents/Info.plist"
        path.write_bytes(plistlib.dumps({"AudioServerPlugIn_MachServices": ["wrong"]}))
        with patch.object(staging, "sign", self.sign):
            with self.assertRaisesRegex(RuntimeError, "exact broker Mach service"):
                self.stage()
        self.assertNotIn("LoopZRoutingAudio.driver", self.signed)
        self.assertFalse(self.output.exists())

    def test_mixed_team_or_native_trust_disagreement_cannot_publish(self):
        def mixed(*args, **kwargs):
            result = self.sign(*args, **kwargs)
            if args[0].name == "loopz-config":
                result["team"] = "OTHERTEAM1"
            return result

        with patch.object(staging, "sign", mixed):
            with self.assertRaisesRegex(RuntimeError, "different signing teams"):
                self.stage()
        self.assertFalse(self.output.exists())
        with (
            patch.object(staging, "sign", self.sign),
            patch.object(
                staging, "command", return_value=subprocess.CompletedProcess([], 0, "{}", "")
            ),
        ):
            with self.assertRaisesRegex(RuntimeError, "disagrees"):
                self.stage()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".loopz-routing-stage-*")))

    def test_existing_output_adhoc_and_source_links_are_rejected(self):
        self.output.mkdir()
        (self.output / "keep").write_text("previous package")
        with self.assertRaisesRegex(RuntimeError, "already exists"):
            self.stage()
        self.assertEqual((self.output / "keep").read_text(), "previous package")
        with self.assertRaisesRegex(RuntimeError, "stable Developer ID"):
            staging.stage(self.build, self.root / "other", self.document, "-")
        source = self.build / "loopz-config"
        source.unlink()
        source.symlink_to(self.build / "loopz-probe")
        with self.assertRaisesRegex(RuntimeError, "linked build component"):
            staging.stage(self.build, self.root / "other", self.document, "test-identity")
        self.assertFalse((self.root / "other").exists())


if __name__ == "__main__":
    unittest.main()
