"""Coordinated payload policy and private four-target file rollback; no system mutations."""

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import suite_update as suite
import driver_update_tests
from desktop_files import DesktopFileTransaction
from install_transaction import files


class SuiteTests(unittest.TestCase):
    def setUp(self):
        nested = driver_update_tests.PackagePolicyTests()
        nested.setUp()
        self.addCleanup(nested.doCleanups)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.package = self.root / "suite"
        shutil.copytree(nested.package, self.package / "update")
        self.broker = self.package / "broker" / suite.BROKER
        self.broker.parent.mkdir()
        self.broker.write_bytes(b"new broker")
        self.broker.chmod(0o755)
        self.manifest = {
            "schema": 1,
            "kind": "loopz-suite-update",
            "installed": False,
            "updateManifestSHA256": hashlib.sha256(
                (self.package / "update/manifest.json").read_bytes()
            ).hexdigest(),
            "capabilities": copy.deepcopy(suite.CAPABILITIES),
        }
        self.refresh()

    def refresh(self):
        value = files(self.package)
        value.pop("manifest.json", None)
        self.manifest["files"] = value
        (self.package / "manifest.json").write_text(json.dumps(self.manifest))

    def verify(self, expected=None, callback=lambda *args: None):
        return suite.verify_suite(self.package, callback, expected)

    def test_complete_nested_payload_verifies_five_fixed_identities(self):
        calls = []
        _, digest = self.verify(callback=lambda *args: calls.append(args))
        self.assertEqual(len(calls), 5)
        self.assertEqual(calls[-1][1:], (suite.BROKER_IDENTIFIER, suite.BROKER_REQUIREMENT, False))
        self.verify(digest)
        self.assertEqual(set(suite.TARGETS), set(suite.NAMES))
        self.assertEqual(
            str(suite.TARGETS[suite.BROKER]),
            "/Library/Application Support/LoopZ/loopz-routing-broker",
        )

    def test_tampered_payload_and_review_digest_fail_before_code_checks(self):
        callback = unittest.mock.Mock()
        with self.assertRaises(ValueError):
            self.verify("0" * 64, callback)
        callback.assert_not_called()
        self.broker.write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self.verify(callback=callback)
        callback.assert_not_called()

    def test_unknown_fields_bad_flags_and_untyped_rate_policy_fail_closed(self):
        mutations = [
            lambda m: m.update(extra=True),
            lambda m: m.update(schema=True),
            lambda m: m.update(schema=1.0),
            lambda m: m.update(installed=0),
            lambda m: m.update(updateManifestSHA256=None),
            lambda m: m["capabilities"].update(rateNegotiation=True),
            lambda m: m["capabilities"].update(outputPairRemoval=True),
            lambda m: m["capabilities"].update(sampleRatesHz=[float(x) for x in suite.RATES]),
            lambda m: m["capabilities"].update(sampleRatesHz=[44100, 48000]),
        ]
        original = copy.deepcopy(self.manifest)
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                self.manifest = copy.deepcopy(original)
                mutate(self.manifest)
                self.refresh()
                with self.assertRaises(ValueError):
                    self.verify()

    def test_duplicate_manifest_keys_are_not_accepted(self):
        path = self.package / "manifest.json"
        path.write_text(path.read_text().replace('"schema": 1', '"schema": 1, "schema": 1', 1))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.verify()

    def test_rehashed_extra_broker_file_and_nonexecutable_are_rejected(self):
        extra = self.broker.parent / "unreviewed"
        extra.write_bytes(b"extra")
        self.refresh()
        with self.assertRaises(ValueError):
            self.verify()
        extra.unlink()
        self.broker.chmod(0o644)
        self.refresh()
        with self.assertRaises(ValueError):
            self.verify()

    def test_links_and_changed_nested_manifest_are_rejected(self):
        alias = self.broker.parent / "linked"
        os.link(self.broker, alias)
        self.refresh()
        with self.assertRaises(ValueError):
            self.verify()
        alias.unlink()
        alias.symlink_to(self.broker)
        with self.assertRaises(ValueError):
            self.verify()
        alias.unlink()
        nested = self.package / "update/manifest.json"
        nested.write_bytes(nested.read_bytes() + b"\n")
        self.refresh()
        with self.assertRaises(ValueError):
            self.verify()

    def test_signature_failure_is_not_replaced_by_metadata_acceptance(self):
        def reject_broker(path, identifier, requirement, engine):
            if identifier == suite.BROKER_IDENTIFIER:
                raise ValueError("Untrusted broker signature")

        with self.assertRaisesRegex(ValueError, "Untrusted"):
            self.verify(callback=reject_broker)

    def test_assembly_and_four_target_rollback_use_only_private_paths(self):
        self.verify()
        assembled = suite.assemble_suite(self.package, self.root)
        self.assertEqual({path.name for path in assembled.iterdir()}, set(suite.NAMES))
        targets = {name: self.root / "installed" / name for name in suite.NAMES}
        for name, path in targets.items():
            path.parent.mkdir(exist_ok=True)
            if name == suite.BROKER:
                path.write_bytes(b"old broker")
            else:
                path.mkdir()
                (path / "old").write_bytes(name.encode())
        before = {name: files(path) for name, path in targets.items()}

        class FailAfterBroker(DesktopFileTransaction):
            def _promote(self, name):
                super()._promote(name)
                if name == suite.BROKER:
                    raise OSError("injected last-target failure")

        transaction = FailAfterBroker(assembled, targets, suite.NAMES)
        transaction.prepare()
        try:
            with self.assertRaisesRegex(OSError, "injected"):
                transaction.apply()
            transaction.rollback()
            self.assertEqual({name: files(path) for name, path in targets.items()}, before)
        finally:
            transaction.finish()
        self.assertFalse(
            any(path.name.startswith(".") for path in targets[suite.BROKER].parent.iterdir())
        )


class CapabilityTests(unittest.TestCase):
    def values(self):
        return {
            "engine": {
                "version": 1,
                "backgroundSession": 1,
                "routingDocument": True,
                "editorService": True,
                "rateNegotiation": 1,
                "outputPairRemoval": 1,
                "sampleRatesHz": list(suite.RATES),
            },
            "broker": {
                "version": 1,
                "rateNegotiation": 1,
                "audioMappingProtocol": 2,
                "sampleRatesHz": list(suite.RATES),
            },
        }

    def inspect(self, values):
        return suite.inspect_suite(
            Path("/private/fixed-package"),
            lambda path: values["broker" if path.name == suite.BROKER else "engine"],
        )

    def test_both_binary_capabilities_are_required(self):
        self.inspect(self.values())
        for component, key in [
            ("engine", "rateNegotiation"),
            ("broker", "rateNegotiation"),
            ("engine", "outputPairRemoval"),
            ("broker", "audioMappingProtocol"),
        ]:
            for value in [None, True, 0]:
                values = self.values()
                values[component][key] = value
                with self.subTest(component=component, key=key, value=value):
                    with self.assertRaises((RuntimeError, ValueError)):
                        self.inspect(values)
        for component in ["engine", "broker"]:
            values = self.values()
            values[component]["sampleRatesHz"] = [44100, 48000]
            with self.assertRaises(ValueError):
                self.inspect(values)

    def test_staged_payload_is_never_executed_as_root(self):
        with (
            patch.object(suite.os, "geteuid", return_value=0),
            patch.object(suite.subprocess, "run") as run,
        ):
            with self.assertRaises(RuntimeError):
                suite.runtime_capabilities(Path("/private/payload"))
            run.assert_not_called()

    def test_capability_command_is_fixed_bounded_and_capture_free(self):
        result = subprocess.CompletedProcess([], 0, json.dumps(self.values()["broker"]), "")
        with (
            patch.object(suite.os, "geteuid", return_value=501),
            patch.object(suite.subprocess, "run", return_value=result) as run,
        ):
            suite.runtime_capabilities(Path("/private/payload"))
            self.assertEqual(run.call_args.args[0], ["/private/payload", "--runtime-capabilities"])
            self.assertEqual(run.call_args.kwargs["timeout"], 10)
            self.assertNotIn("shell", run.call_args.kwargs)
            for name in (
                "LOOPZ_MAC_P12_PASSWORD",
                "SINGZ_MAC_P12_PASSWORD",
                "LOOPZ_ENGINE_SIGNING_IDENTITY",
                "LOOPZ_ENGINE_SIGNING_KEYCHAIN",
            ):
                self.assertNotIn(name, run.call_args.kwargs["env"])
            result.stdout = "x" * (64 * 1024 + 1)
            with self.assertRaises(ValueError):
                suite.runtime_capabilities(Path("/private/payload"))

    def test_capability_child_does_not_inherit_exported_signing_credentials(self):
        secret = {
            name: "test-only-value"
            for name in (
                "LOOPZ_MAC_P12_PASSWORD",
                "SINGZ_MAC_P12_PASSWORD",
                "LOOPZ_ENGINE_SIGNING_IDENTITY",
                "LOOPZ_ENGINE_SIGNING_KEYCHAIN",
            )
        }
        result = subprocess.CompletedProcess([], 0, "{}", "")
        with (
            patch.dict(suite.os.environ, secret),
            patch.object(suite.os, "geteuid", return_value=501),
            patch.object(suite.subprocess, "run", return_value=result) as run,
        ):
            suite.runtime_capabilities(Path("/private/payload"))
            self.assertFalse(set(secret) & set(run.call_args.kwargs["env"]))


if __name__ == "__main__":
    unittest.main()
