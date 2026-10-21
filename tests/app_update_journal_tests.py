"""SIGKILL fault injection against private apps; no installed files or audio."""

import copy
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import app_update_journal as journal_module
from app_update_journal import AppUpdateJournal, NAME
from desktop_files import DesktopFileTransaction
from desktop_install import NAMES
from install_transaction import files


def targets(root):
    return {name: root / "Applications" / name for name in NAMES}


class RecordedTransaction(DesktopFileTransaction):
    def __init__(self, root, journal):
        super().__init__(root / "package", targets(root), NAMES)
        self.journal = journal

    def will_prepare(self, name, prepared):
        self.journal.prepared_node(name, prepared)


def verify_fixture(name, path):
    if name not in NAMES or not (path / "payload").is_file():
        raise ValueError("Invalid fixture application")


def child(root, operation, stop):
    def checkpoint(event):
        if event == stop:
            os.kill(os.getpid(), signal.SIGKILL)

    rename = os.rename
    rename_exclusive = journal_module.rename_exclusive
    unlink = os.unlink
    rmdir = os.rmdir
    save = AppUpdateJournal.save
    counters = {"rename": 0, "restore-rename": 0}

    def counted_rename(*args, **kwargs):
        rename(*args, **kwargs)
        if args and any(".loopz-" in str(arg) for arg in args[:2]):
            counters["rename"] += 1
            checkpoint(f"rename:{counters['rename']}")

    def counted_restore(*args, **kwargs):
        rename_exclusive(*args, **kwargs)
        counters["restore-rename"] += 1
        checkpoint(f"restore-rename:{counters['restore-rename']}")

    def counted_unlink(path, *args, **kwargs):
        unlink(path, *args, **kwargs)
        if path == "payload" and "dir_fd" in kwargs:
            checkpoint("partial:cleanup-file")

    def counted_rmdir(path, *args, **kwargs):
        rmdir(path, *args, **kwargs)
        if path == "nested" and "dir_fd" in kwargs:
            checkpoint("partial:cleanup-directory")

    def saved(self, value, **kwargs):
        previous = self.value
        save(self, value, **kwargs)
        if previous is None or previous["phase"] != value["phase"]:
            checkpoint("phase:" + value["phase"])
        for name in value["names"]:
            if (
                previous is not None
                and previous["new"][name] is None
                and value["new"][name] is not None
            ):
                checkpoint("record:" + name)

    os.rename, os.unlink, os.rmdir = counted_rename, counted_unlink, counted_rmdir
    journal_module.rename_exclusive = counted_restore
    AppUpdateJournal.save = saved
    with AppUpdateJournal(root / "updates", targets(root)) as journal:
        if operation == "update":
            transaction = RecordedTransaction(root, journal)
            journal.begin(transaction, {}, "a" * 64)
            transaction.prepare()
            journal.phase("prepared")
            journal.phase("activating")
            transaction.apply()
            journal.phase("validating")
            journal.phase("committed")
            journal.finish(verify_fixture, committed=True)
            transaction.finish()
        else:
            journal.load()
            journal.restore_files(verify_fixture)
            journal.finish(verify_fixture)
    raise RuntimeError("The requested crash checkpoint was not reached: " + stop)


class JournalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir="/private/tmp", prefix="loopz-app-state-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "updates").mkdir(mode=0o700)
        (self.root / "package").mkdir(mode=0o700)
        (self.root / "Applications").mkdir()
        for name in NAMES:
            for folder, content in (("package", "new"), ("Applications", "old")):
                app = self.root / folder / name
                app.mkdir()
                (app / "payload").write_text(content + name)
                (app / "nested").mkdir()
                (app / "nested/resource").write_text(content + "resource")
                (app / "empty").mkdir()
        self.originals = self.snapshot()
        self.original_inodes = {
            name: path.stat().st_ino for name, path in targets(self.root).items()
        }
        self.new = {name: files(self.root / "package" / name) for name in NAMES}

    def open(self):
        return AppUpdateJournal(self.root / "updates", targets(self.root))

    def snapshot(self):
        return {name: files(path) for name, path in targets(self.root).items()}

    def crash(self, operation, stop):
        result = subprocess.run(
            [
                sys.executable, "-B", "-E", "-s", "-S", __file__,
                "--crash", str(self.root), operation, stop,
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, -signal.SIGKILL, result.stdout + result.stderr)

    def recover(self, *, committed=False):
        with self.open() as journal:
            journal.load()
            if not committed:
                journal.restore_files(verify_fixture)
            journal.finish(verify_fixture, committed=committed)
        self.assertEqual(self.snapshot(), self.new if committed else self.originals)
        if not committed:
            self.assertEqual(
                {name: path.stat().st_ino for name, path in targets(self.root).items()},
                self.original_inodes,
            )
        self.assertFalse((self.root / "updates" / NAME).exists())
        self.assertEqual(
            set(path.name for path in (self.root / "Applications").iterdir()), set(NAMES)
        )

    def prepared(self):
        journal = self.open()
        self.addCleanup(journal.close)
        transaction = RecordedTransaction(self.root, journal)
        self.addCleanup(lambda: [os.close(fd) for fd in transaction.parent_fds.values()])
        journal.begin(transaction, {}, "a" * 64)
        transaction.prepare()
        journal.phase("prepared")
        return journal, transaction

    def test_recover_after_each_preparation_and_promotion_rename(self):
        # Each subtest has independent files and actual SIGKILL, not an exception.
        for stop in (
            "phase:preparing", "record:LoopZ.app", "record:LoopZ Engine.app",
            *[f"rename:{number}" for number in range(1, 7)],
            "phase:prepared", "phase:activating", "phase:validating",
        ):
            with self.subTest(stop=stop):
                fixture = JournalTests()
                fixture.setUp()
                try:
                    fixture.crash("update", stop)
                    fixture.recover()
                finally:
                    fixture.doCleanups()

    def test_recover_after_each_rollback_rename(self):
        for stop in (
            *[f"restore-rename:{number}" for number in range(1, 5)],
            "phase:restoring", "phase:restored", "phase:restored_cleanup",
            "partial:cleanup-file", "partial:cleanup-directory",
        ):
            with self.subTest(stop=stop):
                fixture = JournalTests()
                fixture.setUp()
                try:
                    fixture.crash("update", "phase:validating")
                    fixture.crash("restore", stop)
                    fixture.recover()
                finally:
                    fixture.doCleanups()

    def test_committed_update_finishes_cleanup_without_rollback(self):
        for stop in (
            "phase:committed", "phase:committed_cleanup",
            "partial:cleanup-file", "partial:cleanup-directory",
        ):
            with self.subTest(stop=stop):
                fixture = JournalTests()
                fixture.setUp()
                try:
                    fixture.crash("update", stop)
                    with fixture.open() as journal:
                        journal.load()
                        with self.assertRaises(RuntimeError):
                            journal.restore_files(verify_fixture)
                    fixture.recover(committed=True)
                finally:
                    fixture.doCleanups()

    def test_refuses_cleanup_before_runtime_outcome(self):
        journal, _ = self.prepared()
        for committed in (False, True):
            with self.assertRaises(RuntimeError):
                journal.finish(verify_fixture, committed=committed)
        self.assertEqual(self.snapshot(), self.originals)
        self.assertTrue(journal.pending())

    def test_original_signature_failure_precedes_any_restore_mutation(self):
        self.crash("update", "phase:validating")
        before = self.snapshot()
        with self.open() as journal:
            journal.load()

            def rejected(*args):
                raise ValueError("Signature rejected")

            with self.assertRaisesRegex(ValueError, "Signature rejected"):
                journal.restore_files(rejected)
        self.assertEqual(self.snapshot(), before)

    def test_changed_original_payload_is_retained_for_inspection(self):
        self.crash("update", "phase:validating")
        with self.open() as journal:
            journal.load()
            backup = journal.slot(NAMES[0], "old")
            (backup / "payload").write_text("unrecorded user edit")
            with self.assertRaises(RuntimeError):
                journal.restore_files(verify_fixture)
            self.assertEqual((backup / "payload").read_text(), "unrecorded user edit")
        self.assertEqual(self.snapshot(), self.new)

    def test_replaced_backup_inode_is_rejected(self):
        self.crash("update", "phase:validating")
        with self.open() as journal:
            journal.load()
            backup = journal.slot(NAMES[0], "old")
            moved = backup.with_name("saved-original")
            backup.rename(moved)
            shutil.copytree(moved, backup)
            with self.assertRaisesRegex(RuntimeError, "identity"):
                journal.restore_files(verify_fixture)
        self.assertEqual(self.snapshot(), self.new)

    def test_parent_replacement_is_rejected(self):
        self.crash("update", "phase:validating")
        folder = self.root / "Applications"
        folder.rename(self.root / "moved")
        shutil.copytree(self.root / "moved", folder)
        with self.open() as journal:
            journal.load()
            with self.assertRaisesRegex(RuntimeError, "parent changed"):
                journal.restore_files(verify_fixture)

    def test_symlinked_candidate_is_not_followed_or_removed(self):
        journal, _ = self.prepared()
        slot = journal.slot(NAMES[0], "new")
        shutil.rmtree(slot)
        slot.symlink_to(self.root / "package" / NAMES[0])
        journal.restore_files(verify_fixture)
        with self.assertRaises(ValueError):
            journal.finish(verify_fixture)
        self.assertTrue(slot.is_symlink())
        self.assertTrue((self.root / "package" / NAMES[0] / "payload").exists())

    def test_unknown_empty_directory_prevents_cleanup(self):
        journal, _ = self.prepared()
        slot = journal.slot(NAMES[0], "new")
        (slot / "unrecorded").mkdir()
        journal.restore_files(verify_fixture)
        with self.assertRaisesRegex(RuntimeError, "unrecorded"):
            journal.finish(verify_fixture)
        self.assertTrue((slot / "unrecorded").is_dir())

    def test_missing_original_and_target_never_finalizes(self):
        self.crash("update", "phase:validating")
        with self.open() as journal:
            journal.load()
            shutil.rmtree(journal.slot(NAMES[0], "old"))
            with self.assertRaises(RuntimeError):
                journal.restore_files(verify_fixture)
            self.assertTrue(journal.pending())

    def test_missing_restored_target_is_not_treated_as_verified(self):
        journal, _ = self.prepared()
        journal.restore_files(verify_fixture)
        shutil.rmtree(targets(self.root)[NAMES[0]])
        with self.assertRaisesRegex(RuntimeError, "missing"):
            journal.finish(lambda *args: None)
        self.assertTrue(journal.pending())

    def test_changed_record_inode_prevents_overwrite(self):
        journal, _ = self.prepared()
        record = self.root / "updates" / NAME
        content = record.read_bytes()
        record.rename(record.with_suffix(".retained"))
        record.write_bytes(content)
        record.chmod(0o600)
        with self.assertRaisesRegex(RuntimeError, "changed"):
            journal.phase("activating")

    def test_record_schema_rejects_traversal_commands_and_incomplete_nodes(self):
        journal, _ = self.prepared()
        for mutate in (
            lambda value: value.update(command=["arbitrary", "command"]),
            lambda value: value.update(owner=0),
            lambda value: value.update(token="../../bad"),
            lambda value: value.update(names=["other.app"]),
            lambda value: value["old"][NAMES[0]]["files"].update({"../outside": "b" * 64}),
            lambda value: value["old"][NAMES[0]]["directories"].append("../outside"),
            lambda value: value["new"].update({NAMES[0]: None}),
            lambda value: value.update(parents={}),
        ):
            value = copy.deepcopy(journal.value)
            mutate(value)
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    journal.validate(value)

    def test_private_record_permissions_and_duplicate_fields(self):
        journal, _ = self.prepared()
        record = self.root / "updates" / NAME
        self.assertEqual(record.stat().st_mode & 0o777, 0o600)
        record.write_text('{"version":1,"version":1}')
        with self.assertRaises(ValueError):
            journal.load()

    def test_second_transaction_cannot_replace_pending_record(self):
        journal, _ = self.prepared()
        another = RecordedTransaction(self.root, journal)
        with self.assertRaisesRegex(RuntimeError, "interrupted"):
            journal.begin(another, {}, "a" * 64)

    def test_gui_only_recovery_preserves_engine_inode_and_contents(self):
        with self.open() as journal:
            transaction = RecordedTransaction(self.root, journal)
            transaction.names = (NAMES[0],)
            journal.begin(transaction, {}, "a" * 64)
            try:
                transaction.prepare()
                journal.phase("prepared")
                journal.phase("activating")
                transaction.apply()
                journal.restore_files(verify_fixture)
                journal.finish(verify_fixture)
                self.assertEqual(self.snapshot(), self.originals)
                self.assertEqual(
                    targets(self.root)[NAMES[1]].stat().st_ino,
                    self.original_inodes[NAMES[1]],
                )
            finally:
                transaction.finish()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--crash":
        child(Path(sys.argv[2]), sys.argv[3], sys.argv[4])
    else:
        unittest.main()
