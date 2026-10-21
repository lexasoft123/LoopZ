#!/usr/bin/env python3
"""Routing package/activation failure injection; all targets are temporary."""

import copy
import hashlib
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
import routing_install as install
from install_transaction import FileTransaction, files


class Guard:
    def __init__(self):
        self.closed = False
        self.fail_ready = self.fail_stop = self.fail_close = False

    def ready(self):
        if self.fail_ready:
            raise RuntimeError("guard did not start")

    def check(self):
        pass

    def stop(self):
        if self.fail_stop:
            raise RuntimeError("guard continuity failed")
        return {"passed": True, "stoppedByRequest": True}

    def close(self):
        self.closed = True
        if self.fail_close:
            raise RuntimeError("guard close failed")


class Host:
    def __init__(self):
        self.state = {
            "defaultInput": "input",
            "defaultOutput": "output",
            "defaultSystemOutput": "system",
            "routingDevices": [],
        }
        self.identity = "unchanged Core Audio generation"
        self.loaded = True
        self.calls = []
        self.guard = Guard()
        self.fail_activation = self.fail_cleanup = self.change_identity = False

    def snapshot(self):
        return copy.deepcopy(self.state)

    def require_idle(self, snapshot):
        if snapshot["routingDevices"]:
            raise RuntimeError("not idle")

    def service_identity(self):
        return self.identity

    def protected_state(self):
        return {"legacy": "unchanged inode and hash"}

    def daemon_loaded(self):
        return self.loaded

    def stop_broker(self):
        self.calls.append("stop")
        self.loaded = False

    def start_broker(self):
        self.calls.append("start")
        self.loaded = True

    def start_guard(self, work, before):
        return self.guard

    def validate_activation(self, work):
        self.calls.append("activate")
        self.state["routingDevices"] = [{"uid": "validation-device"}]
        if self.change_identity:
            self.identity = "unexpected restarted service"
        if self.fail_activation:
            raise RuntimeError("driver activation timeout")
        return {"nativeDeviceVerified": True}

    def cleanup_validation(self):
        self.calls.append("withdraw")
        if self.fail_cleanup:
            raise RuntimeError("device could not be withdrawn")
        self.state["routingDevices"] = []

    def activation_diagnostics(self):
        return {"audioCatalog": self.snapshot(), "brokerJobPresent": self.loaded}

    def transition_driver(self, transaction, guard):
        pass

    def prepare_driver_rollback(self, transaction):
        pass

    def restore_driver(self, transaction):
        pass


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="loopz-routing-install-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.stage = self.root / "stage"
        self.stage.mkdir()
        self.targets = {name: self.root / "installed" / name for name in install.NAMES}
        for name, target in self.targets.items():
            (self.stage / name).write_text("new " + name)
            target.parent.mkdir(exist_ok=True)
            target.write_text("old " + name)
        # Initial publication adds the new driver; the existing Engine is kept
        # as a rollback original until native/continuity verification passes.
        self.targets["LoopZRoutingAudio.driver"].unlink()
        self.original = {name: files(path) for name, path in self.targets.items()}
        self.host = Host()

    def run_install(self):
        return install.activate(self.stage, self.targets, list(install.NAMES), self.host, self.root)

    def assert_originals(self):
        self.assertEqual(self.original, {name: files(path) for name, path in self.targets.items()})

    def test_success_requires_publication_withdrawal_and_continuity(self):
        result = self.run_install()
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["hardwareContinuity"]["passed"])
        self.assertEqual(self.host.state["routingDevices"], [])
        self.assertEqual(self.host.calls, ["stop", "start", "activate", "withdraw"])
        self.assertTrue(self.host.guard.closed)
        self.assertFalse(list((self.root / "installed").glob(".*loopz-*")))

    def test_activation_timeout_restores_files_broker_and_runtime(self):
        self.host.fail_activation = True
        result = self.run_install()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["rolledBack"])
        self.assertTrue(result["residualDriverImagePossible"])
        self.assertEqual(
            result["activationDiagnostics"]["audioCatalog"]["routingDevices"],
            [{"uid": "validation-device"}],
        )
        self.assertTrue(self.host.loaded)
        self.assert_originals()
        self.assertTrue(self.host.guard.closed)

    def test_diagnostic_failure_does_not_prevent_rollback(self):
        self.host.fail_activation = True
        with patch.object(self.host, "activation_diagnostics", side_effect=OSError("read failed")):
            result = self.run_install()
        self.assertEqual(result["error"], "driver activation timeout")
        self.assertEqual(result["diagnosticError"], "read failed")
        self.assertTrue(result["rolledBack"])
        self.assert_originals()

    def test_driver_transition_failure_still_restores_files_and_resident_driver(self):
        with (
            patch.object(self.host, "transition_driver", side_effect=RuntimeError("transition")),
            patch.object(self.host, "prepare_driver_rollback") as prepare,
            patch.object(self.host, "restore_driver") as restore,
        ):
            result = self.run_install()
        self.assertEqual(result["error"], "transition")
        self.assertTrue(result["rolledBack"])
        prepare.assert_called_once()
        restore.assert_called_once()
        self.assert_originals()

    def test_unverified_resident_rollback_cannot_report_success(self):
        self.host.fail_activation = True
        with patch.object(
            self.host, "restore_driver", side_effect=RuntimeError("host unavailable")
        ):
            result = self.run_install()
        self.assertFalse(result["rolledBack"])
        self.assertTrue(result["repairRequired"])
        self.assertIn("restore resident driver: host unavailable", result["rollbackErrors"])
        self.assert_originals()

    def test_partial_file_promotion_restores_all_originals(self):
        original = FileTransaction._promote
        count = 0

        def promote(transaction, name):
            nonlocal count
            original(transaction, name)
            count += 1
            if count == 2:
                raise OSError("injected partial promotion")

        with patch.object(FileTransaction, "_promote", promote):
            result = self.run_install()
        self.assertTrue(result["rolledBack"])
        self.assert_originals()

    def test_guard_failure_before_mutation_leaves_installation_untouched(self):
        self.host.guard.fail_ready = True
        result = self.run_install()
        self.assertEqual(result["status"], "failed")
        self.assertEqual(self.host.calls, ["withdraw"])
        self.assert_originals()

    def test_continuity_failure_cannot_commit(self):
        self.host.guard.fail_stop = True
        result = self.run_install()
        self.assertEqual(result["status"], "failed")
        self.assertIn("continuityError", result)
        self.assert_originals()

    def test_runtime_change_or_failed_withdrawal_requires_repair(self):
        for field in ("change_identity", "fail_cleanup"):
            with self.subTest(field=field):
                self.host = Host()
                setattr(self.host, field, True)
                result = self.run_install()
                self.assertEqual(result["status"], "failed")
                self.assertTrue(result["repairRequired"])
                self.assertFalse(result["rolledBack"])
                self.assert_originals()

    def test_failed_rollback_retains_recoverable_backups(self):
        self.host.fail_activation = True
        with patch.object(FileTransaction, "rollback", side_effect=OSError("restore denied")):
            result = self.run_install()
        self.assertTrue(result["repairRequired"])
        self.assertTrue(result["backupsRetained"])
        self.assertTrue(all(Path(path).exists() for path in result["backupsRetained"]))

    def test_compatible_driver_keeps_its_inode(self):
        driver = self.targets["LoopZRoutingAudio.driver"]
        driver.write_bytes((self.stage / driver.name).read_bytes())
        inode = driver.stat().st_ino
        names = install.preflight(self.stage, self.targets, True)
        self.assertNotIn(driver.name, names)
        result = install.activate(self.stage, self.targets, names, self.host, self.root)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(driver.stat().st_ino, inode)
        driver.write_text("incompatible driver")
        with self.assertRaisesRegex(ValueError, "isolated-host reload"):
            install.preflight(self.stage, self.targets, True)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="loopz-routing-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        build = self.root / "build"
        build.mkdir()
        self.stage = self.root / "package"
        for name in staging.COMPONENTS:
            path = build / name
            if path.suffix in (".app", ".driver"):
                (path / "Contents").mkdir(parents=True)
                (path / "Contents/Info.plist").write_bytes(
                    plistlib.dumps({"AudioServerPlugIn_MachServices": [staging.SERVICE]})
                )
            else:
                path.write_text("fixture " + name)

        def sign(path, identifier, *args, **kwargs):
            return {
                "identifier": identifier,
                "team": install.TEAM,
                "requirement": staging.stable_requirement(identifier, install.TEAM),
            }

        def check(tool, binary):
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

        with patch.object(staging, "sign", sign), patch.object(staging, "command", check):
            staging.stage(build, self.stage, self.root / "user.json", "test")
        self.verified = []

    def verify(self, *args):
        self.verified.append(args)

    def rewrite_manifest(self, mutation):
        path = self.stage / "manifest.json"
        document = json.loads(path.read_text())
        mutation(document)
        path.write_text(json.dumps(document))

    def rehash(self):
        actual = files(self.stage)
        actual.pop("manifest.json")
        self.rewrite_manifest(lambda value: value.update(files=actual))

    def test_complete_known_publisher_package_verifies(self):
        _, digest = install.verify_package(self.stage, self.verify)
        self.assertEqual(len(self.verified), 7)
        install.verify_package(self.stage, self.verify, digest)
        with self.assertRaisesRegex(ValueError, "changed after review"):
            install.verify_package(self.stage, self.verify, "0" * 64)

    def test_migration_identity_requires_explicit_mode_and_exact_legacy_factory(self):
        path = self.stage / "LoopZRoutingAudio.driver/Contents/Info.plist"
        info = plistlib.loads(
            (staging.ROOT / "packaging/routing-migration-driver.plist").read_bytes()
        )
        path.write_bytes(plistlib.dumps(info))
        self.rewrite_manifest(lambda value: value.update(kind="loopz-routing-migration"))
        self.rewrite_manifest(
            lambda value: value["signatures"]["LoopZRoutingAudio.driver"].update(
                identifier="com.lexasoft.loopz.driver",
                requirement=staging.stable_requirement("com.lexasoft.loopz.driver", install.TEAM),
            )
        )
        self.rehash()
        with self.assertRaisesRegex(ValueError, "routing-runtime package"):
            install.verify_package(self.stage, self.verify)
        install.verify_package(self.stage, self.verify, legacy_slot=True)
        info["CFPlugInFactories"] = {"another-factory": "LoopZAudio_Create"}
        path.write_bytes(plistlib.dumps(info))
        self.rehash()
        with self.assertRaisesRegex(ValueError, "HAL slot identity"):
            install.verify_package(self.stage, self.verify, legacy_slot=True)

    def test_rehashed_arbitrary_root_job_is_rejected(self):
        path = self.stage / install.DAEMON
        value = plistlib.loads(path.read_bytes())
        value["ProgramArguments"] = ["/bin/sh", "-c", "untrusted"]
        path.write_bytes(plistlib.dumps(value))
        self.rehash()
        with self.assertRaisesRegex(ValueError, "root job"):
            install.verify_package(self.stage, self.verify)

    def test_payload_tampering_wrong_publisher_and_paths_are_rejected(self):
        self.rewrite_manifest(
            lambda value: value["signatures"]["loopz-config"].update(team="OTHERTEAM1")
        )
        with self.assertRaisesRegex(ValueError, "signing identity"):
            install.verify_package(self.stage, self.verify)
        self.rewrite_manifest(
            lambda value: value["signatures"]["loopz-config"].update(team=install.TEAM)
        )
        (self.stage / "loopz-config").write_text("tampered")
        with self.assertRaisesRegex(ValueError, "complete manifest"):
            install.verify_package(self.stage, self.verify)
        self.rehash()
        self.rewrite_manifest(lambda value: value["files"].update({"../escape": "0" * 64}))
        with self.assertRaisesRegex(ValueError, "installation path"):
            install.verify_package(self.stage, self.verify)

    def test_linked_payload_cannot_reach_code_verifier(self):
        path = self.stage / "loopz-config"
        path.unlink()
        path.symlink_to(self.stage / "loopz-probe")
        with self.assertRaisesRegex(ValueError, "Unsupported installation entry"):
            install.verify_package(self.stage, self.verify)
        self.assertEqual(self.verified, [])


if __name__ == "__main__":
    unittest.main()
