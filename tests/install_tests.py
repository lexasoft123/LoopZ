#!/usr/bin/env python3
"""Failure injection for privileged replacement; no system files are touched."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from install_transaction import (
    NAMES,
    FileTransaction,
    compatible_driver,
    files,
    validate_after,
    verify_manifest,
    check_ancestors,
    exclusive_lock,
)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="loopz-install-test-")
        self.root = Path(self.temporary.name).resolve()
        self.stage = self.root / "stage"
        self.stage.mkdir()
        self.targets = {}
        for name in ["app", "broker", "driver"]:
            (self.stage / name).write_text("new-" + name)
            target = self.root / "installed" / name
            target.parent.mkdir(exist_ok=True)
            target.write_text("old-" + name)
            self.targets[name] = target

    def tearDown(self):
        self.temporary.cleanup()

    def test_committed_update_preserves_resident_driver_inode(self):
        driver = self.targets["driver"]
        inode = driver.stat().st_ino
        tx = FileTransaction(self.stage, self.targets, ["app", "broker"])
        tx.prepare()
        tx.apply()
        self.assertEqual(self.targets["app"].read_text(), "new-app")
        self.assertEqual(tx.backups["app"].read_text(), "old-app")
        tx.finish()
        self.assertEqual(driver.read_text(), "old-driver")
        self.assertEqual(driver.stat().st_ino, inode)
        self.assertEqual(set(p.name for p in driver.parent.iterdir()), {"app", "broker", "driver"})

    def test_failure_between_backup_and_promotion_restores_all_originals(self):
        calls = 0

        def fail_promotion(source, target):
            nonlocal calls
            calls += 1
            if calls == 4:
                raise OSError("injected promotion failure")
            os.replace(source, target)

        tx = FileTransaction(self.stage, self.targets, ["app", "broker"], replace=fail_promotion)
        tx.prepare()
        with self.assertRaises(OSError):
            tx.apply()
        tx.rollback()
        tx.finish()
        for name in self.targets:
            self.assertEqual(self.targets[name].read_text(), "old-" + name)

    def test_failed_fresh_install_removes_new_files(self):
        self.targets["app"].unlink()
        tx = FileTransaction(self.stage, self.targets, ["app", "broker"])
        tx.prepare()
        tx.apply()
        tx.rollback()
        tx.finish()
        self.assertFalse(self.targets["app"].exists())
        self.assertEqual(self.targets["broker"].read_text(), "old-broker")

    def test_restore_failure_retains_original_backup(self):
        tx = FileTransaction(self.stage, self.targets, ["app"])
        tx.prepare()
        tx.apply()
        tx.replace = lambda *args: (_ for _ in ()).throw(OSError("injected restore failure"))
        with self.assertRaisesRegex(RuntimeError, "backups retained"):
            tx.rollback()
        self.assertEqual(tx.backups["app"].read_text(), "old-app")

    def test_symlink_package_cannot_replace_an_installed_file(self):
        (self.stage / "broker").unlink()
        (self.stage / "broker").symlink_to(self.targets["driver"])
        tx = FileTransaction(self.stage, self.targets, ["app", "broker"])
        with self.assertRaises(ValueError):
            tx.prepare()
        self.assertEqual(self.targets["broker"].read_text(), "old-broker")
        self.assertFalse(list(self.targets["app"].parent.glob(".*loopz-new-*")))

    def test_complete_bundle_compatibility_includes_resources(self):
        left, right = self.root / "driver-a", self.root / "driver-b"
        for path in [left, right]:
            path.mkdir()
            (path / "binary").write_text("identical")
            (path / "plist").write_text("same")
        self.assertTrue(compatible_driver(left, right))
        (right / "plist").write_text("changed protocol")
        self.assertFalse(compatible_driver(left, right))

    def test_manifest_rejects_path_escape_extra_files_and_incompatible_protocol(self):
        package = self.root / "package"
        package.mkdir()
        for name in [
            *NAMES[2:],
            "LoopZAudio.driver/Contents/MacOS/LoopZAudio",
            "LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
        ]:
            path = package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
        manifest = {"protocol": 1, "sha256": files(package)}
        verify_manifest(package, manifest)
        bad = {"protocol": 1, "sha256": {**manifest["sha256"], "../escape": "x"}}
        with self.assertRaises(ValueError):
            verify_manifest(package, bad)
        with self.assertRaises(ValueError):
            verify_manifest(package, {**manifest, "protocol": 2})
        (package / "extra").write_text("unverified")
        with self.assertRaises(ValueError):
            verify_manifest(package, manifest)

    def test_absent_activation_changed_defaults_or_service_identity_fail(self):
        good = {
            "loopzVisible": True,
            "loopzAlive": True,
            "defaultInput": "mic",
            "defaultOutput": "speakers",
            "defaultSystemOutput": "speakers",
        }
        validate_after(good, good, "pid-1-start-A", "pid-1-start-A")
        for key, value in [
            ("loopzVisible", False),
            ("loopzAlive", False),
            ("defaultOutput", "other"),
        ]:
            with self.assertRaises(RuntimeError):
                validate_after(good, {**good, key: value}, "1", "1")
        with self.assertRaises(RuntimeError):
            validate_after(good, good, "pid-1-start-A", "pid-1-start-B")

    def test_missing_target_with_symlinked_parent_is_rejected(self):
        outside = self.root / "outside"
        outside.mkdir()
        link = self.root / "link"
        link.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            check_ancestors([link / "new-file"])
        tx = FileTransaction(self.stage, {"app": link / "app"}, ["app"])
        with self.assertRaises(ValueError):
            tx.prepare()
        self.assertFalse(list(outside.iterdir()))

    def test_exclusive_lock_rejects_second_installer_and_symlink(self):
        lock = self.root / ".lock"
        with exclusive_lock(lock):
            with self.assertRaisesRegex(RuntimeError, "already running"):
                with exclusive_lock(lock):
                    pass
        with exclusive_lock(lock):
            pass
        link = self.root / "lock-link"
        link.symlink_to(lock)
        with self.assertRaises(ValueError):
            with exclusive_lock(link):
                pass

    def test_post_activation_failure_rolls_back_files_and_broker(self):
        self.exercise_rollback(False)

    def test_broker_stop_failure_still_restores_original_files(self):
        self.exercise_rollback(True)

    def exercise_rollback(self, fail_stop):
        import install_local

        targets = {name: self.root / "rollback-system" / name for name in NAMES}
        for name, target in targets.items():
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("old-" + name)
            (self.stage / name).write_text("new-" + name)
        good = {
            "loopzVisible": True,
            "loopzAlive": True,
            "loopzRunning": False,
            "defaultInput": "mic",
            "defaultOutput": "speaker",
            "defaultSystemOutput": "speaker",
        }
        report = self.root / "guard.json"

        class Guard:
            returncode = 0

            def poll(self):
                return None

            def wait(self, timeout):
                return 0

        report.write_text(json.dumps({"state": "running"}))

        class Result:
            stdout = '{"protocol":1,"driverAttached":true}'
            returncode = 0

        with (
            patch.object(install_local, "TARGETS", targets),
            patch.object(install_local, "snapshot", return_value=good),
            patch.object(
                install_local, "service_identity", side_effect=["original", "restarted", "original"]
            ),
            patch.object(install_local, "command", return_value=Result()),
            patch.object(install_local, "daemon_loaded", return_value=True),
            patch.object(install_local.subprocess, "Popen", return_value=Guard()),
            patch.object(install_local, "stop_broker") as stop,
            patch.object(install_local, "start_broker") as start,
        ):
            if fail_stop:
                stop.side_effect = [None, RuntimeError("injected broker stop failure")]
            with self.assertRaises(RuntimeError):
                install_local.update(self.stage, self.root, list(NAMES[1:]), None)
            self.assertEqual(stop.call_count, 2)
            self.assertEqual(start.call_count, 2)
        for name, target in targets.items():
            self.assertEqual(target.read_text(), "old-" + name)
        self.assertFalse(list(targets["loopz-broker"].parent.glob(".*loopz-old-*")))

    def test_driver_inventory_requires_a_mapping_and_checks_system_hal(self):
        import driver_host

        binary = self.root / "LoopZAudio"
        binary.write_bytes(b"image")
        info = binary.stat()
        name = "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver/Contents/MacOS/LoopZAudio"

        def record(fd, path):
            return f"f{fd}\0D{info.st_dev:x}\0i{info.st_ino}\0n{path}\0".encode()

        valid = b"p123\0" + record("txt", name)
        self.assertEqual(
            driver_host.mapped_image(valid, 123, binary, {"LoopZAudio.driver"})["inode"],
            info.st_ino,
        )
        with self.assertRaises(RuntimeError):
            driver_host.mapped_image(
                b"p123\0" + record("7r", name), 123, binary, {"LoopZAudio.driver"}
            )
        with self.assertRaises(RuntimeError):
            driver_host.mapped_image(
                valid
                + record(
                    "txt", "/System/Library/Audio/Plug-Ins/HAL/Other.driver/Contents/MacOS/Other"
                ),
                123,
                binary,
                {"LoopZAudio.driver"},
            )
        with self.assertRaises(RuntimeError):
            driver_host.mapped_image(valid, 124, binary, {"LoopZAudio.driver"})
        with self.assertRaises(RuntimeError):
            driver_host.mapped_image(b"p123\0", 123, binary, {"LoopZAudio.driver"})

    def test_apple_identity_uses_a_requirement_expression(self):
        import driver_host

        host = {
            "pid": 123,
            "uid": 202,
            "startSeconds": 1,
            "startMicroseconds": 100,
            "executable": "Apple helper",
        }
        with (
            patch.object(driver_host, "process_identity", return_value=host),
            patch.object(driver_host, "command") as command,
        ):
            self.assertEqual(driver_host.identity(123), host)
            command.assert_called_once_with(
                "/usr/bin/codesign",
                "--verify",
                "--strict",
                "-R",
                "=" + driver_host.IDENTITY,
                "Apple helper",
            )

    def test_driver_signal_rejects_a_reused_pid_generation(self):
        import driver_host

        host = {
            "pid": 123,
            "uid": 202,
            "startSeconds": 1,
            "startMicroseconds": 100,
            "executable": "Apple helper",
        }
        with (
            patch.object(
                driver_host, "process_identity", return_value={**host, "startMicroseconds": 101}
            ),
            patch.object(driver_host.os, "kill") as kill,
        ):
            with self.assertRaises(RuntimeError):
                driver_host.signal_checked(host)
            kill.assert_not_called()

    def test_driver_reload_rejects_default_and_dependent_routes(self):
        import driver_host

        state = {
            "loopzVisible": True,
            "loopzAlive": True,
            "loopzRunning": False,
            "loopzBlockingRoutes": [],
            "defaultInput": "mic",
            "defaultOutput": "speakers",
            "defaultSystemOutput": "speakers",
        }
        driver_host.require_idle(state)
        for changed in [
            {"loopzRunning": True},
            {"defaultInput": "com.lexasoft.loopz.audio.feasibility.v1"},
            {"loopzBlockingRoutes": ["aggregate"]},
        ]:
            with self.assertRaises(RuntimeError):
                driver_host.require_idle({**state, **changed})
        del state["loopzBlockingRoutes"]
        with self.assertRaises(RuntimeError):
            driver_host.require_idle(state)

    def test_isolated_update_interruption_after_promotion_restores_files(self):
        import update_driver_local as updater
        import driver_host

        targets = {name: self.root / "driver-rollback" / name for name in NAMES}
        for name, target in targets.items():
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("old-" + name)
            (self.stage / name).write_text("new-" + name)
        state = {
            "loopzVisible": True,
            "loopzAlive": True,
            "loopzRunning": False,
            "loopzBlockingRoutes": [],
            "defaultInput": "mic",
            "defaultOutput": "speakers",
            "defaultSystemOutput": "speakers",
            "driverTiming": {},
            "clockPeriodFrames": 1024,
        }
        host = {
            "pid": 123,
            "uid": 202,
            "startSeconds": 1,
            "startMicroseconds": 100,
            "executable": "Apple helper",
            "device": 1,
            "inode": 2,
            "HALBundles": ["LoopZAudio.driver"],
        }
        (self.root / "guard.json").write_text('{"state":"running"}')

        class Guard:
            returncode = 0

            def poll(self):
                return None

            def wait(self, timeout):
                return 0

        class Result:
            stdout = '{"helperAttached":false}'

        with (
            patch.object(updater.installer, "TARGETS", targets),
            patch.object(updater, "snapshot", return_value=state),
            patch.object(driver_host, "discover", return_value=host),
            patch.object(driver_host, "inspect", return_value=host),
            patch.object(updater.installer, "service_identity", return_value="unchanged"),
            patch.object(updater.installer, "command", return_value=Result()),
            patch.object(updater.subprocess, "Popen", return_value=Guard()),
            patch.object(updater.installer, "stop_broker"),
            patch.object(
                updater.installer,
                "start_broker",
                side_effect=[KeyboardInterrupt("injected after promotion"), None],
            ),
            patch.object(driver_host, "signal_checked") as kill,
        ):
            result = updater.experiment(self.stage, self.root)
        self.assertTrue(result["rolledBack"])
        kill.assert_not_called()
        for name, target in targets.items():
            self.assertEqual(target.read_text(), "old-" + name)


if __name__ == "__main__":
    unittest.main()
