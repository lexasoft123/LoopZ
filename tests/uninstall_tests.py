"""Removal rehearsals use temporary components only; no system job or audio IO."""

import os
import copy
import hashlib
import json
import io
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from uninstall_files import RemovalFiles
import uninstall_files
import uninstall_policy as policy
import uninstall_driver
import uninstall_loopz
import uninstall_host
from types import SimpleNamespace


class RemovalFixture:
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.app = self.root / "Applications/LoopZ.app"
        (self.app / "Contents").mkdir(parents=True)
        (self.app / "Contents/binary").write_bytes(b"original program")
        self.broker = self.root / "Support/broker"
        self.broker.parent.mkdir()
        self.broker.write_bytes(b"original broker")
        self.unknown = self.broker.parent / "user-added-file"
        self.unknown.write_bytes(b"keep")
        self.targets = {
            "app": self.app,
            "broker": self.broker,
            "missing": self.root / "absent/tool",
        }
        self.transaction = RemovalFiles(self.targets)
        self.addCleanup(self.transaction.close)

    def assert_originals(self):
        self.assertEqual((self.app / "Contents/binary").read_bytes(), b"original program")
        self.assertEqual(self.broker.read_bytes(), b"original broker")
        self.assertEqual(self.unknown.read_bytes(), b"keep")


class RemovalFileTests(RemovalFixture, unittest.TestCase):
    def test_inventory_is_read_only_and_stable(self):
        before = self.transaction.prepare()
        self.assertEqual(before, self.transaction.inventory())
        self.assertIsNone(before["missing"])
        self.assert_originals()
        self.assertFalse(list(self.root.rglob(".loopz-remove-*")))

    def test_quarantine_and_rollback_restore_original_inodes(self):
        before = self.transaction.prepare()
        self.transaction.apply()
        self.assertFalse(self.app.exists() or self.broker.exists())
        self.assertEqual(self.transaction.quarantined("app").stat().st_ino, before["app"]["inode"])
        self.transaction.rollback()
        self.assert_originals()
        self.assertEqual(before, self.transaction.inventory())
        self.assertFalse(list(self.root.rglob(".loopz-remove-*")))

    def test_purge_removes_only_reviewed_components(self):
        self.transaction.prepare()
        self.transaction.apply()
        self.assertEqual(self.transaction.purge(), [])
        self.assertTrue(self.transaction.committed)
        self.assertFalse(self.app.exists() or self.broker.exists())
        self.assertEqual(self.unknown.read_bytes(), b"keep")
        self.assertFalse(list(self.root.rglob(".loopz-remove-*")))
        with self.assertRaisesRegex(RuntimeError, "cannot be rolled back"):
            self.transaction.rollback()

    def test_rollback_never_overwrites_a_recreated_target(self):
        self.transaction.prepare()
        self.transaction.apply()
        self.broker.write_bytes(b"new arrival")
        with self.assertRaisesRegex(RuntimeError, "needs repair"):
            self.transaction.rollback()
        self.assertEqual(self.broker.read_bytes(), b"new arrival")
        self.assertEqual(self.transaction.quarantined("broker").read_bytes(), b"original broker")
        self.assertTrue(self.app.exists())

    def test_collision_at_atomic_publication_is_preserved(self):
        self.transaction.prepare()
        self.transaction.apply()
        real = uninstall_files.rename_exclusive

        def collide(source, destination, name):
            if name == "broker":
                self.broker.write_bytes(b"racing writer")
            return real(source, destination, name)

        with patch.object(uninstall_files, "rename_exclusive", collide):
            with self.assertRaisesRegex(RuntimeError, "needs repair"):
                self.transaction.rollback()
        self.assertEqual(self.broker.read_bytes(), b"racing writer")
        self.assertEqual(self.transaction.quarantined("broker").read_bytes(), b"original broker")

    def test_changed_review_fails_before_any_move(self):
        self.transaction.prepare()
        self.broker.write_bytes(b"replaced")
        with self.assertRaisesRegex(RuntimeError, "changed after review"):
            self.transaction.apply()
        self.assertTrue(self.app.exists())
        self.assertEqual(self.transaction.moved, [])

    def test_mid_transaction_failure_retains_rollback(self):
        self.transaction.prepare()
        real = uninstall_files.rename_exclusive

        def fail_broker(source, destination, name):
            if name == "broker":
                raise OSError("injected rename failure")
            real(source, destination, name)

        with patch.object(uninstall_files, "rename_exclusive", fail_broker):
            with self.assertRaisesRegex(OSError, "injected"):
                self.transaction.apply()
        self.transaction.rollback()
        self.assert_originals()
        self.assertFalse(list(self.root.rglob(".loopz-remove-*")))

    def test_symlinks_and_hardlinks_are_rejected_without_following(self):
        original = self.app / "Contents/binary"
        original.unlink()
        original.symlink_to(self.unknown)
        with self.assertRaises(ValueError):
            self.transaction.prepare()
        original.unlink()
        os.link(self.unknown, original)
        with self.assertRaises(ValueError):
            self.transaction.prepare()
        self.assertEqual(self.unknown.read_bytes(), b"keep")

    def test_target_and_parent_symlinks_are_rejected(self):
        self.broker.unlink()
        self.broker.symlink_to(self.unknown)
        with self.assertRaises(ValueError):
            self.transaction.prepare()
        self.broker.unlink()
        self.broker.parent.rename(self.root / "actual-support")
        self.broker.parent.symlink_to(self.root / "actual-support")
        with self.assertRaises(ValueError):
            self.transaction.prepare()

    def test_fifo_substitution_during_open_never_blocks(self):
        real_open = uninstall_files.os.open
        substituted = False

        def replace_before_open(path, flags, *args, **options):
            nonlocal substituted
            if path == "broker" and not substituted and "dir_fd" in options:
                substituted = True
                self.broker.unlink()
                os.mkfifo(self.broker)
            return real_open(path, flags, *args, **options)

        with patch.object(uninstall_files.os, "open", replace_before_open):
            with self.assertRaisesRegex(RuntimeError, "changed while opening"):
                self.transaction.prepare()
        self.assertTrue(substituted and self.app.exists())
        self.assertEqual(self.unknown.read_bytes(), b"keep")

    def test_special_files_and_writable_components_are_rejected(self):
        self.broker.chmod(0o666)
        with self.assertRaises(ValueError):
            self.transaction.prepare()
        self.broker.unlink()
        os.mkfifo(self.broker)
        with self.assertRaises(ValueError):
            self.transaction.prepare()

    def test_changed_quarantine_is_never_purged(self):
        self.transaction.prepare()
        self.transaction.apply()
        saved = self.transaction.quarantined("broker")
        saved.write_bytes(b"changed quarantine")
        errors = self.transaction.purge()
        self.assertEqual(len(errors), 1)
        self.assertTrue(saved.exists())
        self.assertFalse(self.app.exists())
        self.assertTrue(self.transaction.committed)

    def test_close_never_discards_an_uncommitted_quarantine(self):
        self.transaction.prepare()
        self.transaction.apply()
        saved = self.transaction.quarantined("broker")
        self.transaction.close()
        self.assertEqual(saved.read_bytes(), b"original broker")

    def test_nonoverlapping_absolute_paths_required(self):
        for targets in (
            {},
            {"relative": Path("relative")},
            {"traversal": self.root / "a/../b"},
            {"app": self.app, "child": self.app / "Contents"},
            {"a": self.app, "b": self.app},
        ):
            with self.assertRaises(ValueError):
                RemovalFiles(targets)

    def test_deep_and_oversized_components_fail_without_removal(self):
        nested = self.app
        for _ in range(66):
            nested /= "d"
            nested.mkdir()
        with self.assertRaisesRegex(ValueError, "depth limit"):
            self.transaction.prepare()
        with self.broker.open("wb") as output:
            output.truncate(64 * 1024 * 1024 + 1)
        with RemovalFiles({"large": self.broker}) as removal:
            with self.assertRaisesRegex(ValueError, "file limit"):
                removal.prepare()
        self.assertTrue(self.app.exists() and self.broker.exists())


class Guard:
    def __init__(self, host):
        self.host = host
        self.closed = False

    def ready(self):
        self.host.fail("ready")

    def check(self):
        self.host.fail("guard")

    def stop(self):
        self.host.fail("continuity")
        return {"passed": "false" not in self.host.failures}

    def close(self):
        self.closed = True
        self.host.fail("close")


class Transition:
    def __init__(self, host):
        self.host = host
        self.restored = False

    def deactivate(self, removal):
        self.host.fail("driver")
        if "interrupt" in self.host.failures:
            raise KeyboardInterrupt()

    def restore(self):
        self.restored = True
        self.host.fail("restore driver")


class Host:
    def __init__(self, document):
        self.document = document
        self.catalog = {
            "defaultInput": "input",
            "defaultOutput": "output",
            "defaultSystemOutput": "system",
            "routingDevices": [],
            "routingBlockingRoutes": [],
            "loopzBlockingRoutes": [],
            "loopzVisible": False,
            "loopzRunning": False,
        }
        self.loaded = True
        self.failures = set()
        self.guard = Guard(self)
        self.transition = Transition(self)
        self.tools_prepared = False

    def fail(self, phase):
        if phase in self.failures:
            raise RuntimeError("injected " + phase)

    def verify_components(self, _):
        self.fail("signature")

    def state(self):
        return {
            "catalog": self.snapshot(),
            "coreAudio": "same server generation",
            "documents": {"routes": hashlib.sha256(self.document.read_bytes()).hexdigest()},
            "login": {"registered": False},
            "jobs": {"broker": "generation" if self.loaded else None},
        }

    def snapshot(self):
        return copy.deepcopy(self.catalog)

    def require_stopped(self, state):
        self.fail("running")

    def prepare_tools(self, _):
        self.fail("tools")
        self.tools_prepared = True

    def driver_transition(self, _):
        self.fail("isolation")
        return self.transition

    def start_guard(self, *_):
        return self.guard

    def stop_services(self, _):
        self.loaded = False
        self.fail("stop")

    def restore_services(self, _):
        self.loaded = True
        self.fail("restore service")

    def wait_removed(self):
        self.fail("withdraw")
        if "defaults" in self.failures:
            self.catalog["defaultOutput"] = "external user change"
        if "document" in self.failures:
            self.document.write_text("external edit")


class RemovalTransactionTests(RemovalFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.document = self.root / "saved-routes.json"
        self.document.write_text("preserve saved routing")
        self.host = Host(self.document)
        with RemovalFiles(self.targets) as review:
            self.expected = policy.digest(policy.preflight(review, self.host))

    def remove(self):
        return policy.remove_product(self.transaction, self.host, self.root, self.expected)

    def test_success_requires_runtime_and_continuity_before_purge(self):
        result = self.remove()
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["committed"] and result["userDocumentsPreserved"])
        self.assertFalse(self.host.loaded or self.app.exists() or self.broker.exists())
        self.assertEqual(self.document.read_text(), "preserve saved routing")
        self.assertTrue(self.host.guard.closed)
        self.assertEqual(self.unknown.read_bytes(), b"keep")

    def test_stale_plan_rejects_before_tools_or_services(self):
        self.broker.write_text("changed after review")
        with self.assertRaisesRegex(RuntimeError, "plan changed"):
            self.remove()
        self.assertFalse(self.host.tools_prepared)
        self.assertTrue(self.host.loaded and self.app.exists())

    def test_failure_phases_restore_originals_and_services(self):
        for phase in (
            "tools",
            "isolation",
            "ready",
            "stop",
            "driver",
            "withdraw",
            "guard",
            "continuity",
            "false",
            "interrupt",
        ):
            with self.subTest(phase=phase):
                with RemovalFiles(self.targets) as removal:
                    self.host.failures = {phase}
                    result = policy.remove_product(removal, self.host, self.root, self.expected)
                self.assertEqual(result["status"], "failed")
                self.assertFalse(result["committed"])
                self.assertTrue(result["rolledBack"], result)
                self.assertTrue(self.host.loaded)
                self.assert_originals()

    def test_external_default_change_is_reported_not_overwritten(self):
        self.host.failures = {"defaults"}
        result = self.remove()
        self.assertFalse(result["rolledBack"])
        self.assertEqual(self.host.catalog["defaultOutput"], "external user change")
        self.assertTrue(result["repairRequired"])
        self.assert_originals()

    def test_external_document_edit_is_never_rolled_back(self):
        self.host.failures = {"document"}
        result = self.remove()
        self.assertFalse(result["rolledBack"])
        self.assertEqual(self.document.read_text(), "external edit")
        self.assert_originals()

    def test_cleanup_failure_stays_committed_and_preserves_quarantine(self):
        real = uninstall_files.remove_at

        def fail_broker(parent, name, expected):
            if name == "broker":
                raise OSError("injected purge failure")
            return real(parent, name, expected)

        with patch.object(uninstall_files, "remove_at", fail_broker):
            result = self.remove()
        self.assertEqual(result["status"], "cleanupRequired")
        self.assertTrue(result["committed"])
        self.assertNotIn("rolledBack", result)
        self.assertIn("broker", result["retainedQuarantines"])
        self.assertFalse(self.host.loaded)
        self.assertFalse(self.app.exists() or self.broker.exists())
        self.assertEqual(self.transaction.quarantined("broker").read_bytes(), b"original broker")

    def test_failed_service_restore_requires_repair(self):
        self.host.failures = {"withdraw", "restore service"}
        result = self.remove()
        self.assertFalse(result["rolledBack"])
        self.assertTrue(result["repairRequired"])
        self.assert_originals()

    def test_guard_cleanup_failure_never_claims_success(self):
        self.host.failures = {"close"}
        result = self.remove()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["repairRequired"])
        self.assert_originals()

    def test_active_and_incomplete_catalogs_fail_before_any_change(self):
        for change in (
            {"defaultInput": "com.lexasoft.loopz.device.test"},
            {"routingDevices": [{"uid": "test"}]},
            {"routingBlockingRoutes": ["aggregate"]},
            {"loopzBlockingRoutes": None},
            {"loopzVisible": True},
            {"loopzRunning": True},
        ):
            with self.subTest(change=change), RemovalFiles(self.targets) as removal:
                original = copy.deepcopy(self.host.catalog)
                self.host.catalog.update(change)
                with self.assertRaises(RuntimeError):
                    policy.preflight(removal, self.host)
                self.host.catalog = original
                self.assert_originals()

    def test_default_command_is_read_only(self):
        output = io.StringIO()
        factory = lambda targets, owner, **kwargs: RemovalFiles(targets)
        with (
            patch.object(uninstall_loopz, "pending_recovery", return_value=None),
            patch.object(uninstall_loopz, "TARGETS", self.targets),
            patch.object(uninstall_loopz, "UninstallHost", return_value=self.host),
            patch.object(uninstall_loopz, "RemovalFiles", side_effect=factory),
            redirect_stdout(output),
        ):
            self.assertEqual(uninstall_loopz.main([]), 0)
        report = json.loads(output.getvalue())
        self.assertTrue(report["readOnly"])
        self.assertEqual(report["status"], "readyForAdministratorPreflight")
        self.assertFalse(self.host.tools_prepared)
        self.assertTrue(self.host.loaded)
        self.assert_originals()

    def test_checker_switches_only_after_signature_validation(self):
        host = uninstall_host.UninstallHost.__new__(uninstall_host.UninstallHost)
        host.original_checker = self.broker
        host.checker = self.broker
        work = self.root / "private-work"
        work.mkdir(mode=0o700)
        with patch.object(
            uninstall_host, "verify_code", side_effect=RuntimeError("invalid signature")
        ):
            with self.assertRaisesRegex(RuntimeError, "invalid signature"):
                host.prepare_tools(work)
        self.assertEqual(host.checker, self.broker)
        self.assertEqual(self.broker.read_bytes(), b"original broker")


class CommandTests(unittest.TestCase):
    def test_orphan_driver_recovery_record_blocks_uninstall(self):
        with patch.object(
            Path,
            "lstat",
            side_effect=[
                FileNotFoundError(),
                SimpleNamespace(st_mode=0o100600, st_uid=0, st_nlink=1),
            ],
        ):
            status = uninstall_loopz.pending_update()
        self.assertEqual(status["status"], "updateRecoveryRequired")
        self.assertTrue(status["journal"].endswith(".suite-driver-state.json"))

    def test_pending_update_blocks_every_uninstall_mode_before_host_lookup(self):
        for arguments in ([], ["--recover"], ["--remove", "--expect-plan-sha", "0" * 64]):
            with (
                self.subTest(arguments=arguments),
                patch.object(uninstall_loopz.os, "geteuid", return_value=0),
                patch.object(
                    uninstall_loopz,
                    "pending_update",
                    return_value={
                        "status": "updateRecoveryRequired",
                        "readOnly": True,
                    },
                ),
                patch.object(uninstall_loopz, "pending_recovery") as removal,
                patch.object(uninstall_loopz, "UninstallHost") as host,
                redirect_stdout(io.StringIO()) as output,
            ):
                self.assertEqual(uninstall_loopz.main(arguments), 1)
                removal.assert_not_called()
                host.assert_not_called()
                self.assertEqual(json.loads(output.getvalue())["status"], "updateRecoveryRequired")

    def test_pending_update_requires_a_protected_root_owned_regular_record(self):
        for mode, owner, links in (
            (0o100600, 0, 1),
            (0o100644, 0, 1),
            (0o120600, 0, 1),
            (0o100600, 501, 1),
            (0o100600, 0, 2),
        ):
            with (
                self.subTest(mode=mode, owner=owner, links=links),
                patch.object(
                    Path,
                    "lstat",
                    return_value=SimpleNamespace(st_mode=mode, st_uid=owner, st_nlink=links),
                ),
            ):
                if mode == 0o100600 and owner == 0 and links == 1:
                    self.assertEqual(
                        uninstall_loopz.pending_update()["status"], "updateRecoveryRequired"
                    )
                else:
                    with self.assertRaises(ValueError):
                        uninstall_loopz.pending_update()

    def test_update_record_is_rechecked_after_waiting_for_the_shared_lock(self):
        from contextlib import nullcontext

        for recover in (False, True):
            with (
                self.subTest(recover=recover),
                patch.object(uninstall_loopz.os, "geteuid", return_value=0),
                patch.object(
                    uninstall_loopz,
                    "pending_update",
                    side_effect=[None, {"status": "updateRecoveryRequired"}],
                ),
                patch.object(
                    uninstall_loopz,
                    "pending_recovery",
                    return_value={"status": "recoveryRequired"} if recover else None,
                ),
                patch.object(uninstall_loopz, "exclusive_lock", return_value=nullcontext()),
                patch.object(uninstall_loopz, "UninstallHost"),
                patch.object(uninstall_loopz, "RemovalFiles"),
                patch.object(uninstall_loopz, "RemovalJournal") as journal,
                redirect_stdout(io.StringIO()) as output,
            ):
                arguments = (
                    ["--recover"] if recover else ["--remove", "--expect-plan-sha", "0" * 64]
                )
                self.assertEqual(uninstall_loopz.main(arguments), 1)
                journal.assert_not_called()
                self.assertIn("before lock acquisition", output.getvalue())

    def test_pending_recovery_blocks_lookup_of_missing_apps_and_tools(self):
        for arguments in ([], ["--remove", "--expect-plan-sha", "0" * 64]):
            with (
                self.subTest(arguments=arguments),
                patch.object(uninstall_loopz.os, "geteuid", return_value=0),
                patch.object(
                    uninstall_loopz,
                    "pending_recovery",
                    return_value={"status": "recoveryRequired", "readOnly": True},
                ),
                patch.object(uninstall_loopz, "UninstallHost") as host,
                redirect_stdout(io.StringIO()) as output,
            ):
                self.assertEqual(uninstall_loopz.main(arguments), 1)
                host.assert_not_called()
                self.assertEqual(json.loads(output.getvalue())["status"], "recoveryRequired")

    def test_remove_requires_root_and_a_review_digest_before_host_construction(self):
        for uid, arguments in (
            (501, ["--remove"]),
            (0, ["--remove"]),
            (0, ["--remove", "--expect-plan-sha", "unsafe"]),
            (0, ["--expect-plan-sha", "0" * 64]),
        ):
            with (
                self.subTest(uid=uid, arguments=arguments),
                patch.object(uninstall_loopz.os, "geteuid", return_value=uid),
                patch.object(uninstall_loopz, "UninstallHost") as host,
                redirect_stderr(io.StringIO()),
            ):
                with self.assertRaises(SystemExit) as error:
                    uninstall_loopz.main(arguments)
                self.assertEqual(error.exception.code, 2)
                host.assert_not_called()


class DriverInventoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.binary = Path(self.temporary.name) / "LoopZAudio"
        self.binary.write_bytes(b"private fixture")
        self.identity = {"pid": 123, "uid": 202, "startSeconds": 42}
        self.command = patch.object(uninstall_driver.driver_host, "command")
        self.command_mock = self.command.start()
        self.addCleanup(self.command.stop)
        self.kernel = patch.object(
            uninstall_driver.driver_host, "identity", return_value=self.identity
        )
        self.kernel_mock = self.kernel.start()
        self.addCleanup(self.kernel.stop)

    def mapping(self, bundle="LoopZAudio.driver", inode=None, binary=None):
        info = self.binary.stat()
        return (
            f"ftxt\0D{info.st_dev:x}\0i{info.st_ino if inode is None else inode}\0"
            f"n/Library/Audio/Plug-Ins/HAL/{bundle}/Contents/MacOS/{binary or self.binary.name}\0"
        ).encode()

    def inspect(self, records):
        self.command_mock.return_value = SimpleNamespace(stdout=b"p123\0" + records, stderr=b"")
        return uninstall_driver.inspect(123, self.binary, {"LoopZAudio.driver"})

    def test_exact_owned_image_with_complete_identity(self):
        result = self.inspect(self.mapping())
        self.assertEqual(result["inode"], self.binary.stat().st_ino)
        self.assertEqual(result["HALBundles"], ["LoopZAudio.driver"])

    def test_fresh_slot_executable_name_is_supported(self):
        self.binary = self.binary.rename(self.binary.with_name("LoopZRoutingAudio"))
        self.command_mock.return_value = SimpleNamespace(
            stdout=b"p123\0" + self.mapping(bundle="LoopZRoutingAudio.driver"), stderr=b""
        )
        result = uninstall_driver.inspect(123, self.binary, {"LoopZRoutingAudio.driver"})
        self.assertEqual(result["HALBundles"], ["LoopZRoutingAudio.driver"])

    def test_other_hal_components_and_mixed_images_are_rejected(self):
        for additional in (
            self.mapping(bundle="Other.driver"),
            self.mapping(inode=1),
            b"fmem\0n/System/Library/Audio/Plug-Ins/HAL/System.driver/plugin\0",
            b"fmem\0n/System/Volumes/Data/Library/Audio/Plug-Ins/HAL/Other.driver/plugin\0",
        ):
            with self.subTest(additional=additional), self.assertRaises(RuntimeError):
                self.inspect(self.mapping() + additional)

    def test_missing_inode_pid_and_inspection_warnings_are_rejected(self):
        with self.assertRaises(RuntimeError):
            self.inspect(
                b"ftxt\0n/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver/Contents/MacOS/LoopZAudio\0"
            )
        with self.assertRaises(RuntimeError):
            self.inspect(b"p999\0" + self.mapping())
        self.command_mock.return_value = SimpleNamespace(
            stdout=b"p123\0" + self.mapping(), stderr=b"incomplete"
        )
        with self.assertRaises(RuntimeError):
            uninstall_driver.inspect(123, self.binary, {"LoopZAudio.driver"})

    def test_kernel_generation_change_is_rejected(self):
        self.kernel_mock.side_effect = [self.identity, {**self.identity, "startSeconds": 43}]
        with self.assertRaises(RuntimeError):
            self.inspect(self.mapping())

    def transition(self):
        value = uninstall_driver.DriverRemoval.__new__(uninstall_driver.DriverRemoval)
        value.original = {**self.identity, "inode": 5, "HALBundles": ["LoopZAudio.driver"]}
        value.name, value.bundle, value.executable = (
            "driver",
            self.binary.parent / "LoopZAudio.driver",
            "LoopZAudio",
        )
        value.signalled = False
        return value

    def test_changed_host_never_receives_a_signal(self):
        transition = self.transition()
        removal = SimpleNamespace(
            quarantined=lambda _: self.binary.parent / "quarantine/LoopZAudio.driver"
        )
        with (
            patch.object(uninstall_driver.os, "kill"),
            patch.object(
                uninstall_driver,
                "inspect",
                return_value={**transition.original, "startSeconds": 99},
            ),
            patch.object(uninstall_driver.driver_host, "signal_checked") as signal,
        ):
            with self.assertRaises(RuntimeError):
                transition.deactivate(removal)
            signal.assert_not_called()

    def test_only_the_reaudited_generation_can_be_signalled(self):
        transition = self.transition()
        removal = SimpleNamespace(
            quarantined=lambda _: self.binary.parent / "quarantine/LoopZAudio.driver"
        )
        audited = {**transition.original, "HALBundles": ["quarantine"]}
        with (
            patch.object(uninstall_driver.os, "kill"),
            patch.object(uninstall_driver, "inspect", return_value=audited),
            patch.object(transition, "pids", return_value=[]),
            patch.object(uninstall_driver.driver_host, "signal_checked") as signal,
        ):
            transition.deactivate(removal)
            signal.assert_called_once_with(audited, force=True)
        self.assertTrue(transition.signalled)

    def test_automatic_unload_never_signals_a_replacement(self):
        transition = self.transition()
        with (
            patch.object(uninstall_driver.os, "kill", side_effect=ProcessLookupError()),
            patch.object(transition, "pids", return_value=[]),
            patch.object(uninstall_driver.driver_host, "signal_checked") as signal,
        ):
            transition.deactivate(None)
            signal.assert_not_called()


if __name__ == "__main__":
    unittest.main()
