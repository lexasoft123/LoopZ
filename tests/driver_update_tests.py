"""Existing-slot update policy, isolated-host guards and three-component rollback."""

import copy
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import driver_update as update
import desktop_install as desktop
import desktop_install_tests as desktop_tests
from install_transaction import FileTransaction, files


class PackagePolicyTests(unittest.TestCase):
    def setUp(self):
        fixture = desktop_tests.PackageTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.package = self.root / "update"
        shutil.copytree(fixture.stage, self.package / "desktop")
        self.driver = self.package / "driver" / update.DRIVER_NAME
        (self.driver / "Contents/MacOS").mkdir(parents=True)
        (self.driver / "Contents/MacOS/LoopZAudio").write_text("driver code")
        (self.driver / "Contents/Resources").mkdir()
        (self.driver / "Contents/Resources/routing-service.json").write_text(
            json.dumps(update.BROKER_PIN)
        )
        self.info = {
            "CFBundleIdentifier": "com.lexasoft.loopz.driver",
            "CFBundleExecutable": "LoopZAudio",
            "CFPlugInFactories": {"F2F8AC9D-AE84-4550-955C-E35A027F56B2": "LoopZAudio_Create"},
            "CFPlugInTypes": {
                "443ABAB8-E7B3-491A-B985-BEB9187030DB": ["F2F8AC9D-AE84-4550-955C-E35A027F56B2"]
            },
            "AudioServerPlugIn_MachServices": [update.SERVICE],
        }
        (self.driver / "Contents/Info.plist").write_bytes(plistlib.dumps(self.info))
        self.manifest = {
            "schema": 1,
            "kind": "loopz-desktop-driver-update",
            "installed": False,
            "desktopManifestSHA256": hashlib.sha256(
                (self.package / "desktop/manifest.json").read_bytes()
            ).hexdigest(),
        }
        self.refresh()

    def refresh(self):
        actual = files(self.package)
        actual.pop("manifest.json", None)
        self.manifest["files"] = actual
        (self.package / "manifest.json").write_text(json.dumps(self.manifest))

    def verify(self, digest=None):
        return update.verify_update(self.package, lambda *args: None, digest)

    def test_complete_payload_verifies_four_code_identities_and_assembles_only_targets(self):
        seen = []
        _, digest = update.verify_update(self.package, lambda *args: seen.append(args))
        self.assertEqual(len(seen), 4)
        self.verify(digest)
        assembled = update.assemble(self.package, self.root)
        self.assertEqual({p.name for p in assembled.iterdir()}, set(update.UPDATE_NAMES))

    def test_changed_manifest_and_tampered_driver_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "changed"):
            self.verify("0" * 64)
        (self.driver / "Contents/MacOS/LoopZAudio").write_text("changed")
        with self.assertRaisesRegex(ValueError, "complete manifest"):
            self.verify()

    def test_rehashed_wrong_factory_pin_extra_payload_and_symlinks_are_rejected(self):
        info = self.driver / "Contents/Info.plist"
        info.write_bytes(plistlib.dumps({**self.info, "CFBundleExecutable": "another"}))
        self.refresh()
        with self.assertRaisesRegex(ValueError, "slot identity"):
            self.verify()
        info.write_bytes(plistlib.dumps(self.info))
        pin = self.driver / "Contents/Resources/routing-service.json"
        pin.write_text("{}")
        self.refresh()
        with self.assertRaisesRegex(ValueError, "trust pin"):
            self.verify()
        pin.write_text(json.dumps(update.BROKER_PIN))
        extra = self.package / "driver/extra"
        extra.write_text("not a driver component")
        self.refresh()
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            self.verify()
        extra.unlink()
        (self.driver / "linked").symlink_to("Contents")
        with self.assertRaises(ValueError):
            self.verify()

    def test_missing_nested_digest_and_unknown_schema_fail_closed(self):
        for change in ({"desktopManifestSHA256": None}, {"schema": True}, {"installed": 0}):
            before = copy.deepcopy(self.manifest)
            self.manifest.update(change)
            self.refresh()
            with self.assertRaises(ValueError):
                self.verify()
            self.manifest = before


class UpdateTransactionTests(unittest.TestCase):
    def setUp(self):
        self.fixture = desktop_tests.TransactionTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        f = self.fixture
        self.targets = {**f.targets, update.DRIVER_NAME: f.root / "installed" / update.DRIVER_NAME}
        self.targets[update.DRIVER_NAME].write_text("old driver")
        (f.stage / update.DRIVER_NAME).write_text("new driver")
        self.original = {name: files(path) for name, path in self.targets.items()}
        self.transition = Mock()

    def deploy(self, **options):
        f = self.fixture
        return desktop.deploy(
            f.stage,
            self.targets,
            f.host,
            f.root,
            names=update.UPDATE_NAMES,
            driver_transition=self.transition,
            **options,
        )

    def test_three_components_promote_before_reload_and_validate_before_withdrawal(self):
        def activate(transaction, guard):
            self.assertEqual(set(transaction.backups), {"LoopZ Engine.app", update.DRIVER_NAME})
            self.assertTrue(
                all(
                    files(path) == files(self.fixture.stage / name)
                    for name, path in self.targets.items()
                )
            )

        self.transition.activate.side_effect = activate
        result = self.deploy()
        self.assertEqual(result["status"], "passed")
        self.assertFalse(result["driverPreserved"])
        self.assertTrue(result["brokerPreserved"])
        self.transition.validate.assert_called_once()
        self.transition.restore.assert_not_called()

    def test_failed_new_driver_check_restores_all_files_before_runtime_rollback(self):
        self.transition.validate.side_effect = RuntimeError("new driver failed")
        self.transition.restore.side_effect = lambda work: self.assertEqual(
            self.original, {name: files(path) for name, path in self.targets.items()}
        )
        result = self.deploy()
        self.assertTrue(result["rolledBack"] and result["filesRestored"])
        self.transition.prepare_rollback.assert_called_once()
        self.transition.restore.assert_called_once()

    def test_failed_file_rollback_does_not_signal_a_runtime_replacement(self):
        def transaction(*args):
            result = FileTransaction(*args)
            result.rollback = Mock(side_effect=RuntimeError("disk restore failed"))
            return result

        self.transition.validate.side_effect = RuntimeError("new driver failed")
        result = self.deploy(transaction_factory=transaction)
        self.assertFalse(result["rolledBack"])
        self.assertTrue(result["retainedRecoveryFiles"])
        self.transition.restore.assert_not_called()

    def test_third_component_promotion_failure_restores_files_without_reloading(self):
        def transaction(*args):
            def replace(source, target):
                if target == self.targets[update.DRIVER_NAME] and ".loopz-new-" in source.name:
                    raise OSError("driver promotion failed")
                source.rename(target)

            return FileTransaction(*args, replace=replace)

        result = self.deploy(transaction_factory=transaction)
        self.assertTrue(result["rolledBack"] and result["filesRestored"])
        self.assertEqual(self.original, {name: files(path) for name, path in self.targets.items()})
        self.transition.activate.assert_not_called()


class HostTransitionTests(unittest.TestCase):
    def setUp(self):
        self.host = Mock()
        self.host.service_identity.return_value = "same core audio"
        self.host.snapshot.return_value = {}
        self.original = {
            "pid": 123,
            "uid": 202,
            "startSeconds": 55,
            "startMicroseconds": 44,
            "executable": "Apple isolated host",
            "device": 1,
            "inode": 2,
            "HALBundles": [update.DRIVER_NAME],
        }
        self.transition = update.DriverTransition(self.host)
        self.guard = Mock()
        self.transaction = SimpleNamespace(
            backups={update.DRIVER_NAME: Path("/temporary/.LoopZAudio.driver.loopz-old-test")}
        )
        self.discover = patch.object(
            update.driver_host, "discover", return_value=self.original
        ).start()
        self.inspect = patch.object(
            update.driver_host, "inspect", return_value=self.original
        ).start()
        self.signal = patch.object(update.driver_host, "signal_checked").start()
        self.addCleanup(patch.stopall)
        self.transition.begin({}, "same core audio")

    def test_only_verified_original_can_be_signalled(self):
        self.transition.activate(self.transaction, self.guard)
        self.signal.assert_called_once_with(self.original, force=True)
        self.assertTrue(self.transition.signalled)

    def test_generation_busy_catalog_changed_audio_or_no_backup_never_signals(self):
        self.inspect.return_value = {**self.original, "startMicroseconds": 45}
        with self.assertRaisesRegex(RuntimeError, "generation"):
            self.transition.activate(self.transaction, self.guard)
        self.inspect.return_value = self.original
        self.host.require_idle.side_effect = RuntimeError("active graph")
        with self.assertRaisesRegex(RuntimeError, "active graph"):
            self.transition.activate(self.transaction, self.guard)
        self.host.require_idle.side_effect = None
        self.host.service_identity.return_value = "different core audio"
        with self.assertRaisesRegex(RuntimeError, "Core Audio"):
            self.transition.activate(self.transaction, self.guard)
        with self.assertRaisesRegex(RuntimeError, "backup"):
            self.transition.activate(SimpleNamespace(backups={}), self.guard)
        self.signal.assert_not_called()

    def test_unverified_replacement_is_never_signalled_during_restore(self):
        self.transition.signalled = True
        self.discover.side_effect = RuntimeError("mixed or unavailable mapped image")
        self.transition.prepare_rollback()
        self.discover.side_effect = None
        self.host.validate_activation.return_value = {"captureOpened": False}
        with tempfile.TemporaryDirectory() as temporary:
            result = self.transition.restore(Path(temporary))
        self.assertFalse(result["reloadRequired"])
        self.signal.assert_not_called()
        self.host.cleanup_validation.assert_called_once()

    def test_os_reloaded_driver_before_our_signal_still_requires_verified_runtime_rollback(self):
        replacement = {**self.original, "pid": 456, "inode": 77}
        self.discover.return_value = replacement
        self.transition.prepare_rollback()
        self.assertFalse(self.transition.signalled)
        self.discover.side_effect = [RuntimeError("only new image is mapped"), self.original]
        with tempfile.TemporaryDirectory() as temporary:
            result = self.transition.restore(Path(temporary))
        self.assertTrue(result["reloadRequired"])
        self.signal.assert_called_once_with(replacement, force=True)
        self.host.validate_activation.assert_called_once()

    def test_os_already_restored_old_image_does_not_receive_another_signal(self):
        self.transition.replacement = {**self.original, "pid": 456, "inode": 77}
        with tempfile.TemporaryDirectory() as temporary:
            result = self.transition.restore(Path(temporary))
        self.assertFalse(result["reloadRequired"])
        self.signal.assert_not_called()

    def test_retaining_both_images_in_one_process_does_not_prove_runtime_rollback(self):
        replacement = {**self.original, "inode": 77}
        self.transition.replacement = replacement
        fresh = {**self.original, "pid": 789, "startSeconds": 66}
        self.discover.side_effect = [self.original, fresh]
        with tempfile.TemporaryDirectory() as temporary:
            result = self.transition.restore(Path(temporary))
        self.assertTrue(result["reloadRequired"])
        self.signal.assert_called_once_with(replacement, force=True)
        self.assertEqual(result["host"], fresh)

    def test_superseded_host_signal_does_not_prevent_verifying_os_recovery(self):
        replacement = {**self.original, "pid": 456, "inode": 77}
        self.transition.replacement = replacement
        self.signal.side_effect = RuntimeError("Host generation changed before signal")
        self.discover.side_effect = [RuntimeError("reload pending"), self.original]
        with tempfile.TemporaryDirectory() as temporary:
            result = self.transition.restore(Path(temporary))
        self.assertEqual(result["host"], self.original)
        self.assertIn("generation", result["supersededSignalError"])

    def test_changed_core_audio_prevents_rollback_signal(self):
        self.transition.signalled = True
        self.transition.replacement = {**self.original, "pid": 456}
        self.host.service_identity.return_value = "another core audio"
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "Core Audio changed"):
                self.transition.restore(Path(temporary))
        self.signal.assert_not_called()

    def test_replacement_requires_successful_capture_free_report_and_stable_host(self):
        replacement = {**self.original, "pid": 456, "inode": 77}
        self.discover.return_value = replacement
        report = {
            "passed": True,
            "inputOpened": False,
            "physicalDevicesOpened": False,
            "permissionRequested": False,
        }
        self.host.user_command.return_value = SimpleNamespace(stdout=json.dumps(report))
        activation = {"publicDevice": {"uid": "com.lexasoft.loopz.device.temporary"}}
        self.assertEqual(self.transition.validate(activation)["replacementHost"], replacement)
        self.host.user_command.return_value.stdout = json.dumps({**report, "inputOpened": True})
        with self.assertRaisesRegex(RuntimeError, "capture-free"):
            self.transition.validate(activation)
        self.host.user_command.return_value.stdout = json.dumps(report)
        self.discover.side_effect = [replacement, {**replacement, "startMicroseconds": 66}]
        with self.assertRaisesRegex(RuntimeError, "changed during"):
            self.transition.validate(activation)


class IdlePolicyTests(unittest.TestCase):
    def test_incomplete_legacy_or_dependent_graph_cannot_pass_the_update_gate(self):
        host = object.__new__(update.DriverUpdateHost)
        clean = {
            "defaultInput": "physical-input",
            "defaultOutput": "physical-output",
            "defaultSystemOutput": "system-output",
            "routingDevices": [],
            "routingBlockingRoutes": [],
            "loopzBlockingRoutes": [],
            "loopzVisible": False,
            "loopzRunning": False,
            "routingPlugIn": {"registered": False},
        }
        with (
            patch.object(update.DesktopHost, "require_idle"),
            patch.object(Path, "exists", return_value=False),
        ):
            host.require_idle(clean)
            for change in (
                {"defaultOutput": "com.lexasoft.loopz.device.cached"},
                {"routingDevices": [{}]},
                {"routingBlockingRoutes": [{}]},
                {"loopzVisible": True},
                {"routingPlugIn": {"registered": True}},
            ):
                with self.assertRaises(RuntimeError):
                    host.require_idle({**clean, **change})
            incomplete = dict(clean)
            del incomplete["loopzBlockingRoutes"]
            with self.assertRaises(RuntimeError):
                host.require_idle(incomplete)


class CapabilityTests(unittest.TestCase):
    def test_required_features_are_strict_but_additive_features_are_accepted(self):
        value = {
            "version": 1,
            "backgroundSession": 1,
            "routingDocument": True,
            "editorService": True,
            "publicTimingProbe": 1,
            "continuousRoutingProbe": 1,
        }
        self.assertEqual(desktop.validate_runtime_capabilities(value), value)
        for change in (
            {"version": True},
            {"backgroundSession": True},
            {"routingDocument": 1},
            {"editorService": False},
            {"version": 2},
        ):
            with self.assertRaises(RuntimeError):
                desktop.validate_runtime_capabilities({**value, **change})


if __name__ == "__main__":
    unittest.main()
