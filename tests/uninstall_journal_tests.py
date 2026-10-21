"""Durability/recovery tests operate only on private temporary components."""

import copy
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from uninstall_files import RemovalFiles
from uninstall_journal import RemovalJournal, NAME
import uninstall_policy
import uninstall_files
from uninstall_tests import Host
from uninstall_workspace import DiagnosticWorkspaces


class JournalFixture:
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="loopz-journal-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "fixture.marker").write_text("private-test")
        app = self.root / "Applications/LoopZ.app"
        app.mkdir(parents=True)
        (app / "first").write_bytes(b"first original")
        (app / "second").write_bytes(b"second original")
        broker = self.root / "Support/broker"
        broker.parent.mkdir()
        broker.write_bytes(b"original broker")
        self.targets = {"app": app, "broker": broker}
        self.work = self.root / "loopz-uninstall-work"
        self.work.mkdir(mode=0o700)
        self.files = RemovalFiles(self.targets)
        self.addCleanup(self.files.close)
        inventory = self.files.prepare()
        self.original = copy.deepcopy(inventory)
        self.plan = {
            "version": 1,
            "mode": "loopz-uninstall",
            "targets": {k: str(v) for k, v in self.targets.items()},
            "inventory": inventory,
            "before": {"fixture": True},
            "preserve": [],
        }
        self.journal = self.open_journal()
        self.addCleanup(self.journal.close)

    def open_journal(self):
        return RemovalJournal(self.root / "Support", self.targets, workspace_root=self.root)

    def begin(self):
        self.journal.begin(self.plan, self.files, self.work)

    def assert_originals(self):
        with RemovalFiles(self.targets) as inspect:
            self.assertEqual(inspect.inventory(), self.original)


class JournalTests(JournalFixture, unittest.TestCase):
    def test_round_trip_records_fixed_plan_and_private_workspace(self):
        self.begin()
        with self.open_journal() as reopened:
            value = reopened.load()
        self.assertEqual(value["plan"], self.plan)
        self.assertEqual(value["phase"], "removing")
        self.assertEqual(value["workspace"]["inode"], self.work.stat().st_ino)
        self.assertEqual((self.root / "Support" / NAME).stat().st_mode & 0o777, 0o600)
        self.assert_originals()

    def test_quarantine_identity_is_durable_before_first_rename(self):
        self.begin()
        self.files.apply()
        with self.open_journal() as reopened:
            value = reopened.load()
            for path, info in value["quarantines"].items():
                saved = Path(path) / f".loopz-remove-{value['token']}"
                self.assertEqual(
                    (info["device"], info["inode"]), (saved.stat().st_dev, saved.stat().st_ino)
                )
            report = reopened.recover_files()
        self.assertEqual(report["status"], "originalFilesRestored")
        self.assertTrue(report["runtimeFinalizationRequired"] and report["journalRetained"])
        self.assert_originals()

    def test_recovery_is_idempotent_without_premature_journal_clear(self):
        self.begin()
        self.files.apply()
        with self.open_journal() as reopened:
            first = reopened.recover_files()
            second = reopened.recover_files()
            self.assertEqual(first, second)
            self.assertTrue(reopened.pending())
        self.assert_originals()

    def test_collision_prevents_every_restore_and_preserves_both_versions(self):
        self.begin()
        self.files.apply()
        self.targets["broker"].write_bytes(b"new broker")
        with self.open_journal() as reopened:
            with self.assertRaisesRegex(RuntimeError, "collision"):
                reopened.recover_files()
        self.assertFalse(self.targets["app"].exists())
        self.assertEqual(self.targets["broker"].read_bytes(), b"new broker")
        self.assertEqual(self.files.quarantined("broker").read_bytes(), b"original broker")

    def test_committed_cleanup_preserves_new_destinations(self):
        self.begin()
        self.files.apply()
        self.journal.begin_purge()
        self.targets["broker"].write_bytes(b"new installation")
        self.targets["app"].symlink_to(self.root / "unrelated")
        with self.open_journal() as reopened:
            report = reopened.recover_files()
        self.assertEqual(report["status"], "committedFilesCleaned")
        self.assertEqual(self.targets["broker"].read_bytes(), b"new installation")
        self.assertTrue(self.targets["app"].is_symlink())
        self.assertFalse(list(self.root.rglob(".loopz-remove-*")))

    def test_partial_committed_tree_is_cleaned_but_changed_data_is_not(self):
        self.begin()
        self.files.apply()
        self.journal.begin_purge()
        saved = self.files.quarantined("app")
        (saved / "first").unlink()
        with self.open_journal() as reopened:
            self.assertEqual(reopened.recover_files()["status"], "committedFilesCleaned")
        self.assertFalse(saved.exists())

    def test_foreign_quarantine_file_aborts_before_any_cleanup(self):
        self.begin()
        self.files.apply()
        self.journal.begin_purge()
        saved = self.files.quarantined("app")
        (saved / "foreign").write_bytes(b"preserve")
        with self.open_journal() as reopened:
            with self.assertRaisesRegex(RuntimeError, "changed data"):
                reopened.recover_files()
        self.assertTrue((saved / "foreign").exists() and self.files.quarantined("broker").exists())

    def test_changed_original_is_not_restored_or_purged(self):
        self.begin()
        self.files.apply()
        self.files.quarantined("broker").write_bytes(b"changed saved data")
        with self.open_journal() as reopened:
            with self.assertRaises(RuntimeError):
                reopened.recover_files()
        self.assertFalse(self.targets["app"].exists())

    def test_private_empty_unrecorded_quarantine_can_be_recovered(self):
        self.begin()
        folder = self.targets["app"].parent / f".loopz-remove-{self.files.token}"
        folder.mkdir(mode=0o700)
        with self.open_journal() as reopened:
            self.assertEqual(reopened.recover_files()["status"], "originalFilesRestored")
        self.assertFalse(folder.exists())
        self.assert_originals()

    def test_changed_parent_or_quarantine_identity_is_rejected(self):
        self.begin()
        self.files.apply()
        folder = self.files.quarantined("app").parent
        folder.rename(folder.with_name("retained-original-quarantine"))
        folder.mkdir(mode=0o700)
        with self.open_journal() as reopened:
            with self.assertRaisesRegex(RuntimeError, "identity"):
                reopened.recover_files()

    def test_finish_is_explicit_after_external_runtime_verification(self):
        self.begin()
        self.files.apply()
        self.files.rollback()
        self.assertTrue(self.journal.pending())
        self.journal.finish("restored")
        self.assertFalse(self.journal.pending())

    def test_existing_record_cannot_be_overwritten(self):
        self.begin()
        with self.open_journal() as other:
            with self.assertRaisesRegex(RuntimeError, "unfinished"):
                other.begin(self.plan, self.files, self.work)
        self.assertEqual(self.journal.load()["plan"], self.plan)

    def test_original_plan_is_frozen_before_later_checkpoints(self):
        self.begin()
        self.plan["preserve"].append("mutated caller list")
        self.files.apply()
        with self.open_journal() as reopened:
            self.assertEqual(reopened.load()["plan"]["preserve"], [])

    def test_finalization_refuses_remaining_originals_and_wrong_disposition(self):
        self.begin()
        self.files.apply()
        with self.assertRaisesRegex(RuntimeError, "must finish"):
            self.journal.finish("restored")
        with self.assertRaises(ValueError):
            self.journal.finish("removed")
        self.journal.begin_purge()
        with self.assertRaises(ValueError):
            self.journal.finish("restored")

    def test_journal_sync_failure_prevents_file_mutation(self):
        with patch("uninstall_journal.fcntl.fcntl", side_effect=OSError("flush failed")):
            with self.assertRaisesRegex(OSError, "flush failed"):
                self.begin()
        self.assertFalse(self.journal.pending())
        self.assert_originals()

    def test_failure_after_publication_retains_a_readable_recovery_record(self):
        real = os.fsync

        def fail_directory(descriptor):
            if descriptor == self.journal.directory:
                raise OSError("directory sync failed")
            real(descriptor)

        with patch("uninstall_journal.os.fsync", fail_directory):
            with self.assertRaisesRegex(OSError, "directory sync failed"):
                self.begin()
        with self.open_journal() as reopened:
            self.assertEqual(reopened.load()["phase"], "removing")
            self.assertEqual(reopened.recover_files()["status"], "originalFilesRestored")
        self.assert_originals()

    def test_a_different_removal_cannot_publish_quarantines_into_this_record(self):
        self.begin()
        token = self.files.token
        self.files.token = "a" * 32
        try:
            with self.assertRaisesRegex(RuntimeError, "does not belong"):
                self.files.apply()
        finally:
            self.files.token = token
        self.assert_originals()

    def test_schema_paths_and_snapshot_names_are_validated(self):
        self.begin()
        base = self.journal.load()
        changes = [
            lambda v: v.update(schema=True),
            lambda v: v.update(phase=[]),
            lambda v: v.update(token="../escape"),
            lambda v: v["plan"]["targets"].update(app="/Applications/Foreign.app"),
            lambda v: v["parents"].update({"/etc": {"device": 1, "inode": 1}}),
            lambda v: v["workspace"].update(path="/etc/loopz-uninstall-escape"),
            lambda v: v["plan"]["inventory"]["app"]["entries"].update(
                {"../escape": v["plan"]["inventory"]["broker"]}
            ),
        ]
        for change in changes:
            value = copy.deepcopy(base)
            change(value)
            value["planSHA256"] = uninstall_policy.digest(value["plan"])
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.journal.validate(value)

    def test_links_permissions_and_truncated_records_are_rejected(self):
        self.begin()
        path = self.root / "Support" / NAME
        path.chmod(0o644)
        with self.assertRaises(ValueError):
            self.journal.load()
        path.chmod(0o600)
        path.write_text('{"schema":')
        with self.assertRaises(ValueError):
            self.journal.load()
        path.unlink()
        path.symlink_to(self.targets["broker"])
        with self.assertRaises(ValueError):
            self.journal.load()


class PolicyJournalTests(JournalFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.document = self.root / "routes.json"
        self.document.write_text("keep desired routing")
        self.host = Host(self.document)
        with RemovalFiles(self.targets) as review:
            self.expected = uninstall_policy.digest(uninstall_policy.preflight(review, self.host))
        self.files.originals = {}

    def test_verified_success_clears_durable_record(self):
        result = uninstall_policy.remove_product(
            self.files, self.host, self.work, self.expected, journal=self.journal
        )
        self.assertEqual(result["status"], "passed")
        self.assertFalse(self.journal.pending())

    def test_verified_rollback_clears_durable_record(self):
        self.host.failures = {"withdraw"}
        result = uninstall_policy.remove_product(
            self.files, self.host, self.work, self.expected, journal=self.journal
        )
        self.assertTrue(result["rolledBack"])
        self.assertFalse(self.journal.pending())
        self.assert_originals()

    def test_commit_write_failure_never_purges_or_claims_confirmed_commit(self):
        original_save = self.journal.save

        def fail_commit(value, **options):
            if value["phase"] == "purging":
                raise OSError("injected journal write failure")
            original_save(value, **options)

        with patch.object(self.journal, "save", fail_commit):
            result = uninstall_policy.remove_product(
                self.files, self.host, self.work, self.expected, journal=self.journal
            )
        self.assertEqual(result["status"], "recoveryRequired")
        self.assertIsNone(result["committed"])
        self.assertTrue(self.files.quarantined("broker").exists())
        with self.open_journal() as reopened:
            self.assertEqual(reopened.recover_files()["status"], "originalFilesRestored")
        self.assert_originals()

    def test_error_after_actual_deletion_keeps_a_usable_commit_record(self):
        real_remove = uninstall_files.remove_at

        def fail_after_delete(parent, name, expected):
            real_remove(parent, name, expected)
            if name == "broker":
                raise OSError("post-delete failure")

        with patch.object(uninstall_files, "remove_at", fail_after_delete):
            result = uninstall_policy.remove_product(
                self.files, self.host, self.work, self.expected, journal=self.journal
            )
        self.assertEqual(result["status"], "cleanupRequired")
        self.assertTrue(result["committed"])
        self.assertNotIn("rolledBack", result)
        self.assertNotIn("retainedQuarantines", result)
        with self.open_journal() as reopened:
            self.assertEqual(reopened.recover_files()["status"], "committedFilesCleaned")

    def test_normal_workspace_cleanup_precedes_journal_clear(self):
        manager = DiagnosticWorkspaces(self.root, processes=lambda _: [])

        def finalize(value):
            self.assertTrue(self.journal.pending())
            manager.clean(value)

        result = uninstall_policy.remove_product(
            self.files,
            self.host,
            self.work,
            self.expected,
            journal=self.journal,
            finalize_workspace=finalize,
        )
        self.assertEqual(result["status"], "passed")
        self.assertFalse(self.work.exists() or self.journal.pending())

    def test_workspace_failure_cannot_claim_a_complete_rollback(self):
        self.host.failures = {"withdraw"}

        def fail_cleanup(_):
            raise OSError("workspace cleanup failed")

        result = uninstall_policy.remove_product(
            self.files,
            self.host,
            self.work,
            self.expected,
            journal=self.journal,
            finalize_workspace=fail_cleanup,
        )
        self.assertEqual(result["status"], "recoveryRequired")
        self.assertFalse(result["rolledBack"])
        self.assertTrue(result["filesAndServicesRestored"])
        self.assertTrue(self.journal.pending() and self.work.exists())
        self.assert_originals()


class ActualProcessLossTests(JournalFixture, unittest.TestCase):
    def kill_at(self, checkpoint):
        fixture = Path(__file__).with_name("uninstall_crash_fixture.py")
        child = subprocess.Popen(
            [sys.executable, "-E", "-s", "-S", str(fixture), str(self.root), checkpoint],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        selector = selectors.DefaultSelector()
        selector.register(child.stdout, selectors.EVENT_READ)
        try:
            self.assertTrue(selector.select(10), "Crash checkpoint did not become ready")
            self.assertEqual(child.stdout.readline().strip(), "ready")
            self.assertIsNone(child.poll())
            child.kill()  # Only this verified-live owned Popen child, never a system process.
            _, error = child.communicate(timeout=5)
            self.assertEqual(child.returncode, -signal.SIGKILL, error)
        finally:
            selector.close()
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=5)

    def recover_checkpoint(self, checkpoint, committed):
        self.kill_at(checkpoint)
        with self.open_journal() as reopened:
            report = reopened.recover_files()
        if committed:
            self.assertEqual(report["status"], "committedFilesCleaned")
            self.assertTrue(all(not path.exists() for path in self.targets.values()))
        else:
            self.assertEqual(report["status"], "originalFilesRestored")
            self.assert_originals()
        self.assertTrue(report["journalRetained"] and report["runtimeFinalizationRequired"])

    def test_death_after_initial_record(self):
        self.recover_checkpoint("journal-recorded", False)

    def test_death_after_quarantine_record(self):
        self.recover_checkpoint("quarantine-recorded", False)

    def test_death_immediately_after_first_rename(self):
        self.recover_checkpoint("first-rename", False)

    def test_death_after_all_renames(self):
        self.recover_checkpoint("all-renames", False)

    def test_death_after_commit_before_any_deletion(self):
        self.recover_checkpoint("commit-recorded", True)

    def test_death_partway_through_bundle_deletion(self):
        self.recover_checkpoint("partial-purge", True)

    def test_death_after_purge_before_journal_finalization(self):
        self.recover_checkpoint("purge-complete", True)

    def test_death_immediately_after_rollback_rename(self):
        self.recover_checkpoint("rollback-rename", False)

    def test_death_after_terminal_restore_record(self):
        self.recover_checkpoint("restored-recorded", False)


if __name__ == "__main__":
    unittest.main()
