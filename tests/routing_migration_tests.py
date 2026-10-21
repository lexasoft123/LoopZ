#!/usr/bin/env python3
"""Fail-closed existing-slot transition tests; no process or system mutations."""

from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import routing_migration as migration


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.host = object.__new__(migration.LegacySlotHost)
        self.host.original_host = {
            "pid": 321,
            "uid": 202,
            "startSeconds": 123,
            "startMicroseconds": 456,
            "executable": "Apple isolated host",
            "device": 1,
            "inode": 3,
        }
        self.host.original_snapshot = {
            "defaultInput": "input",
            "defaultOutput": "output",
            "defaultSystemOutput": "system",
            "loopzVisible": True,
            "loopzAlive": True,
            "loopzRunning": False,
            "clockPeriodFrames": 128,
            "inputLatencyFrames": 1024,
            "routingDevices": [],
        }
        self.host.original_core_audio = "unchanged core audio"
        self.host.signalled = False
        self.host.rollback_host = None
        self.host.legacy_peer = Mock(
            return_value={
                "driverPID": 321,
                "driverAttached": True,
                "helperAttached": False,
            }
        )
        self.host.snapshot = Mock(return_value=self.host.original_snapshot)
        self.host.require_idle = Mock()
        self.host.service_identity = Mock(return_value=self.host.original_core_audio)
        self.guard = Mock()
        self.transaction = SimpleNamespace(
            backups={migration.DRIVER_KEY: Path("/temporary/.LoopZAudio.driver.loopz-old-test")}
        )
        inspect_patch = patch.object(
            migration.driver_host, "inspect", return_value=dict(self.host.original_host)
        )
        signal_patch = patch.object(migration.driver_host, "signal_checked")
        self.inspect = inspect_patch.start()
        self.signal = signal_patch.start()
        self.addCleanup(inspect_patch.stop)
        self.addCleanup(signal_patch.stop)

    def test_real_root_owned_checker_filesystem_check_is_system_python_compatible(self):
        expected = '{"driverPID":321,"driverAttached":true,"helperAttached":false}'
        with patch.object(
            migration, "command", return_value=SimpleNamespace(stdout=expected)
        ) as run:
            peer = migration.read_legacy_bridge_status(Path("/usr/bin/true"))
        self.assertEqual(peer["driverPID"], 321)
        run.assert_called_once_with(Path("/usr/bin/true"), "--bridge-status")

    def test_checker_symlinks_and_unprotected_files_are_rejected_without_execution(self):
        with tempfile.TemporaryDirectory(prefix="loopz-checker-path-") as temporary:
            folder = Path(temporary)
            linked = folder / "linked"
            linked.symlink_to("/usr/bin/true")
            writable = folder / "writable"
            writable.write_text("fixture")
            writable.chmod(0o777)
            with patch.object(migration, "command") as run:
                for path in [linked, writable, folder]:
                    with self.assertRaisesRegex(RuntimeError, "root-owned and protected"):
                        migration.read_legacy_bridge_status(path)
                run.assert_not_called()

    def test_signal_requires_authenticated_idle_unchanged_host_and_backup_image(self):
        self.host.transition_driver(self.transaction, self.guard)
        self.assertTrue(self.host.signalled)
        self.signal.assert_called_once_with(self.host.original_host, force=True)
        self.inspect.assert_called_once_with(
            321,
            self.transaction.backups[migration.DRIVER_KEY] / "Contents/MacOS/LoopZAudio",
            {migration.DRIVER.name, self.transaction.backups[migration.DRIVER_KEY].name},
        )
        self.host.require_idle.assert_called_once_with(self.host.original_snapshot)

    def test_wrong_peer_or_active_capture_never_signals(self):
        for peer in [
            {"driverPID": 999, "driverAttached": True},
            {"driverPID": 321, "driverAttached": True, "helperAttached": True},
            {"driverPID": 321, "driverAttached": False},
        ]:
            self.host.legacy_peer.return_value = peer
            with self.assertRaisesRegex(RuntimeError, "authenticate"):
                self.host.transition_driver(self.transaction, self.guard)
        self.signal.assert_not_called()

    def test_pid_reuse_mapped_image_failure_and_audio_change_never_signal(self):
        for field in self.host.original_host:
            self.inspect.return_value = {**self.host.original_host, field: "changed"}
            with self.assertRaisesRegex(RuntimeError, "generation"):
                self.host.transition_driver(self.transaction, self.guard)
        self.inspect.side_effect = RuntimeError("Another HAL component is mapped")
        with self.assertRaises(RuntimeError):
            self.host.transition_driver(self.transaction, self.guard)
        self.inspect.side_effect = None
        self.inspect.return_value = dict(self.host.original_host)
        self.host.service_identity.return_value = "different core audio"
        with self.assertRaisesRegex(RuntimeError, "Core Audio changed"):
            self.host.transition_driver(self.transaction, self.guard)
        self.signal.assert_not_called()

    def test_no_backup_or_busy_device_never_signals(self):
        with self.assertRaisesRegex(RuntimeError, "rollback bundle"):
            self.host.transition_driver(SimpleNamespace(backups={}), self.guard)
        self.host.require_idle.side_effect = RuntimeError("LoopZ is busy")
        with self.assertRaisesRegex(RuntimeError, "busy"):
            self.host.transition_driver(self.transaction, self.guard)
        self.signal.assert_not_called()

    def test_failed_continuity_guard_never_signals(self):
        self.guard.check.side_effect = RuntimeError("Output guard ended")
        with self.assertRaisesRegex(RuntimeError, "Output guard"):
            self.host.transition_driver(self.transaction, self.guard)
        self.signal.assert_not_called()
        self.assertFalse(self.host.signalled)

    def test_rollback_only_signals_a_verified_replacement_and_checks_legacy_attachment(self):
        self.host.signalled = True
        replacement = {**self.host.original_host, "pid": 654, "inode": 6}
        with patch.object(migration.driver_host, "discover", return_value=replacement):
            self.host.prepare_driver_rollback(self.transaction)
        self.host.restore_driver(self.transaction)
        self.signal.assert_called_once_with(replacement, force=True)
        self.inspect.assert_called_once_with(321, migration.BINARY)

    def test_missing_replacement_does_not_allow_a_name_based_signal(self):
        self.host.signalled = True
        with patch.object(migration.driver_host, "discover", side_effect=RuntimeError("no host")):
            self.host.prepare_driver_rollback(self.transaction)
        self.host.restore_driver(self.transaction)
        self.signal.assert_not_called()

    def test_disk_restore_without_runtime_restore_is_a_failure(self):
        self.host.snapshot.return_value = {**self.host.original_snapshot, "loopzAlive": False}
        with (
            patch.object(migration.time, "monotonic", side_effect=[0, 1, 26]),
            patch.object(migration.time, "sleep"),
        ):
            with self.assertRaisesRegex(RuntimeError, "restoration was not verified"):
                self.host.restore_driver(self.transaction)
        self.signal.assert_not_called()


if __name__ == "__main__":
    unittest.main()
