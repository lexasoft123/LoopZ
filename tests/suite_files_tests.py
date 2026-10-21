"""Private real-filesystem crash/recovery checks; no launchd, HAL or installed files."""

import copy
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
from install_transaction import files
from suite_files import NAME, SuiteFiles
from suite_update import BROKER, NAMES


def targets(root):
    return {name: root / "installed" / name for name in NAMES}


def crash_child(root, operation, stop):
    def checkpoint(event):
        if event == stop:
            os._exit(73)

    if stop == "partial:cleanup":
        unlink = os.unlink

        def interrupted_unlink(path, *args, **kwargs):
            unlink(path, *args, **kwargs)
            if path == "payload" and "dir_fd" in kwargs:
                os._exit(73)

        os.unlink = interrupted_unlink

    with SuiteFiles(root / "support", targets(root), checkpoint=checkpoint) as transaction:
        if operation == "prepare":
            transaction.prepare(root / "source")
        else:
            transaction.load()
            getattr(transaction, operation)()
    raise RuntimeError("The requested crash checkpoint was not reached")


class SuiteFileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir="/private/tmp", prefix="loopz-suite-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.folder = self.root / "support"
        self.folder.mkdir(mode=0o700)
        self.stage = self.root / "source"
        self.stage.mkdir(mode=0o700)
        (self.root / "installed").mkdir()
        for name in NAMES:
            for parent, label in ((self.stage, "new"), (self.root / "installed", "old")):
                path = parent / name
                if name != BROKER:
                    path.mkdir()
                    (path / "payload").write_text(label + name)
                    (path / "nested").mkdir()
                    (path / "nested" / "resource").write_text("retained " + label)
                else:
                    path.write_text(label + name)
                    path.chmod(0o755)
        self.originals = self.snapshot()
        self.original_inodes = self.inodes()
        self.new = {name: files(self.stage / name) for name in NAMES}

    def open(self, **kwargs):
        return SuiteFiles(self.folder, targets(self.root), **kwargs)

    def snapshot(self):
        return {name: files(path) for name, path in targets(self.root).items()}

    def inodes(self):
        return {name: path.stat().st_ino for name, path in targets(self.root).items()}

    def prepare(self):
        with self.open() as transaction:
            transaction.prepare(self.stage)

    def apply(self):
        with self.open() as transaction:
            transaction.load()
            transaction.apply()

    def recover(self):
        with self.open() as transaction:
            transaction.load()
            transaction.rollback()
            # This fixture has no service runtime. A production owner must verify
            # the restored broker/driver before authorizing this final cleanup.
            transaction.finish()
        self.assertEqual(self.snapshot(), self.originals)
        self.assertEqual(self.inodes(), self.original_inodes)
        self.assertFalse((self.folder / NAME).exists())
        self.assertFalse(list((self.root / "installed").glob(".loopz-*")))
        self.assertEqual(set(p.name for p in (self.root / "installed").iterdir()), set(NAMES))
        self.assertEqual(list(self.folder.iterdir()), [])

    def crash(self, operation, event):
        result = subprocess.run(
            [
                sys.executable,
                "-E",
                "-s",
                "-S",
                __file__,
                "--crash",
                str(self.root),
                operation,
                event,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 73, result.stdout + result.stderr)

    def test_promotion_retains_all_originals_until_explicit_runtime_commit(self):
        self.prepare()
        self.apply()
        self.assertEqual(self.snapshot(), self.new)
        with self.open() as transaction:
            transaction.load()
            self.assertEqual(transaction.value["phase"], "activating")
            with self.assertRaisesRegex(RuntimeError, "verification"):
                transaction.finish()
            for name in NAMES:
                backup = targets(self.root)[name].parent / transaction.sibling(name, "old")
                self.assertEqual(backup.stat().st_ino, self.original_inodes[name])
            transaction.commit()
            with self.assertRaisesRegex(RuntimeError, "committed"):
                transaction.rollback()
            transaction.finish()
        self.assertEqual(self.snapshot(), self.new)
        self.assertFalse((self.folder / NAME).exists())
        self.assertEqual(list(self.folder.iterdir()), [])

    def test_complete_rollback_restores_original_inodes_and_retains_new_runtime_images(self):
        self.prepare()
        self.apply()
        active = self.inodes()
        with self.open() as transaction:
            transaction.load()
            transaction.rollback()
            self.assertEqual(transaction.value["phase"], "filesRestored")
            self.assertTrue(transaction.pending())
            for name in NAMES:
                candidate = targets(self.root)[name].parent / transaction.sibling(name, "new")
                self.assertEqual(candidate.stat().st_ino, active[name])
        self.recover()

    def test_close_never_removes_recovery_data_or_commits_activation(self):
        self.prepare()
        self.apply()
        before = self.snapshot()
        with self.open() as transaction:
            value = transaction.load()
        self.assertEqual(value["phase"], "activating")
        self.assertEqual(self.snapshot(), before)
        self.assertTrue((self.folder / NAME).exists())
        self.recover()

    def test_process_death_after_every_preparation_boundary_recovers_originals(self):
        events = ["journal:created", "workspace:recorded"]
        events += [
            f"candidate:{step}:{name}" for name in NAMES for step in ("recorded", "published")
        ]
        events.append("phase:prepared")
        for event in events:
            with self.subTest(event=event):
                self.crash("prepare", event)
                self.recover()

    def test_process_death_between_each_backup_and_promotion_restores_all_four(self):
        events = ["phase:activating"]
        events += [
            f"{step}:{name}" for name in NAMES for step in ("original:moved", "replacement:active")
        ]
        for event in events:
            with self.subTest(event=event):
                self.prepare()
                self.crash("apply", event)
                self.recover()

    def test_process_death_during_rollback_is_repeatable(self):
        events = ["phase:restoring"]
        events += [
            f"{step}:{name}"
            for name in reversed(NAMES)
            for step in ("replacement:retained", "original:restored")
        ]
        events.append("phase:filesRestored")
        for event in events:
            with self.subTest(event=event):
                self.prepare()
                self.apply()
                self.crash("rollback", event)
                self.recover()

    def test_durable_commit_survives_process_death_and_never_restores_old_files(self):
        self.prepare()
        self.apply()
        self.crash("commit", "phase:committed")
        with self.open() as transaction:
            transaction.load()
            with self.assertRaises(RuntimeError):
                transaction.rollback()
            transaction.finish()
        self.assertEqual(self.snapshot(), self.new)

    def test_cleanup_after_commit_is_repeatable_after_each_component(self):
        for name in NAMES:
            with self.subTest(name=name):
                # A completed iteration is the next iteration's old installation.
                self.prepare()
                self.apply()
                with self.open() as transaction:
                    transaction.load()
                    transaction.commit()
                self.crash("finish", "cleanup:" + name)
                with self.open() as transaction:
                    transaction.load()
                    transaction.finish()
                self.assertEqual(self.snapshot(), self.new)
                self.assertEqual(list(self.folder.iterdir()), [])

    def test_restored_cleanup_is_repeatable_after_each_component(self):
        for event in ["phase:restored", *["cleanup:" + name for name in NAMES]]:
            with self.subTest(event=event):
                self.prepare()
                self.apply()
                with self.open() as transaction:
                    transaction.load()
                    transaction.rollback()
                self.crash("finish", event)
                with self.open() as transaction:
                    transaction.load()
                    transaction.finish()
                self.assertEqual(self.snapshot(), self.originals)
                self.assertEqual(self.inodes(), self.original_inodes)
                self.assertEqual(list(self.folder.iterdir()), [])

    def test_process_death_inside_recursive_cleanup_resumes_both_durable_decisions(self):
        for committed in (False, True):
            with self.subTest(committed=committed):
                self.prepare()
                self.apply()
                with self.open() as transaction:
                    transaction.load()
                    if committed:
                        transaction.commit()
                    else:
                        transaction.rollback()
                self.crash("finish", "partial:cleanup")
                with self.open() as transaction:
                    transaction.load()
                    self.assertEqual(
                        transaction.value["phase"], "committed" if committed else "restored"
                    )
                    with self.assertRaises(RuntimeError):
                        transaction.rollback()
                    transaction.finish()
                self.assertEqual(self.snapshot(), self.new if committed else self.originals)
                self.assertEqual(list(self.folder.iterdir()), [])

    def test_foreign_target_blocks_whole_rollback_before_any_other_file_moves(self):
        self.prepare()
        self.apply()
        foreign = targets(self.root)[NAMES[-1]]
        foreign.write_text("foreign replacement")
        before = self.snapshot()
        with self.open() as transaction:
            transaction.load()
            with self.assertRaisesRegex(RuntimeError, "location changed"):
                transaction.rollback()
        self.assertEqual(self.snapshot(), before)
        self.assertTrue((self.folder / NAME).exists())

    def test_failed_partial_copy_is_recoverable_without_an_active_file_change(self):
        def fail_copy(source, target, **kwargs):
            target.mkdir()
            (target / "partial").write_text("incomplete payload")
            raise OSError("simulated full disk")

        with (
            self.open() as transaction,
            patch("suite_files.shutil.copytree", side_effect=fail_copy),
        ):
            with self.assertRaisesRegex(OSError, "full disk"):
                transaction.prepare(self.stage)
            self.assertTrue(transaction.pending())
        self.recover()

    def test_workspace_created_without_a_recorded_identity_is_retained_for_inspection(self):
        self.crash("prepare", "journal:created")
        with self.open() as transaction:
            transaction.load()
            # Model death between mkdir and its next durable identity write.
            # Recovery cannot assume this unrecorded inode belongs to the update.
            workspace = self.folder / transaction.workspace_name()
            workspace.mkdir(mode=0o700)
            marker = workspace / "retain"
            marker.write_text("unverified workspace")
            transaction.rollback()
            with self.assertRaisesRegex(RuntimeError, "unrecorded update workspace"):
                transaction.finish()
            self.assertEqual(marker.read_text(), "unverified workspace")
            self.assertTrue(transaction.pending())
        self.assertEqual(self.snapshot(), self.originals)
        self.assertEqual(self.inodes(), self.original_inodes)

    def test_foreign_cleanup_root_blocks_all_deletions_and_retains_the_journal(self):
        self.prepare()
        self.apply()
        with self.open() as transaction:
            transaction.load()
            transaction.commit()
            name = NAMES[-1]
            parent = targets(self.root)[name].parent
            backup = parent / transaction.sibling(name, "old")
            backup.rename(self.folder / "retained-original")
            backup.write_text("foreign file")
            before = sorted(p.name for p in parent.iterdir())
            with self.assertRaisesRegex(RuntimeError, "cleanup location was replaced"):
                transaction.finish()
            self.assertEqual(sorted(p.name for p in parent.iterdir()), before)
            self.assertEqual(backup.read_text(), "foreign file")
            self.assertTrue(transaction.pending())

    def test_parent_substitution_refuses_recovery_without_touching_either_tree(self):
        self.prepare()
        self.apply()
        original = self.root / "installed"
        retained = self.root / "retained"
        original.rename(retained)
        original.mkdir()
        marker = original / "foreign"
        marker.write_text("untouched")
        with self.open() as transaction:
            transaction.load()
            with self.assertRaisesRegex(RuntimeError, "parent was replaced"):
                transaction.rollback()
        self.assertEqual(marker.read_text(), "untouched")
        self.assertEqual(len(list(retained.iterdir())), 8)

    def test_foreign_backup_and_linked_candidate_are_never_overwritten(self):
        self.prepare()
        with self.open() as transaction:
            transaction.load()
            name = NAMES[0]
            backup = targets(self.root)[name].parent / transaction.sibling(name, "old")
            backup.write_text("foreign backup")
            with self.assertRaisesRegex(RuntimeError, "location changed"):
                transaction.apply()
            self.assertEqual(backup.read_text(), "foreign backup")
            backup.unlink()
            candidate = targets(self.root)[name].parent / transaction.sibling(name, "new")
            shutil.rmtree(candidate)
            candidate.symlink_to(self.stage / name)
            with self.assertRaises(ValueError):
                transaction.apply()
        self.assertEqual(self.snapshot(), self.originals)

    def test_pending_journal_and_incomplete_installation_prevent_preparation(self):
        self.prepare()
        with self.open() as transaction:
            with self.assertRaisesRegex(RuntimeError, "unfinished"):
                transaction.prepare(self.stage)
        self.recover()
        targets(self.root)[BROKER].unlink()
        with self.open() as transaction:
            with self.assertRaisesRegex(ValueError, "all four"):
                transaction.prepare(self.stage)
        self.assertFalse((self.folder / NAME).exists())

    def test_unfinished_uninstall_blocks_update_even_with_a_dangling_record_link(self):
        path = self.folder / ".uninstall-state.json"
        for linked in (False, True):
            with self.subTest(linked=linked):
                if linked:
                    path.symlink_to(self.folder / "missing")
                else:
                    path.write_text("pending uninstall")
                with self.open() as transaction:
                    with self.assertRaisesRegex(RuntimeError, "unfinished uninstall"):
                        transaction.prepare(self.stage)
                self.assertEqual(self.snapshot(), self.originals)
                self.assertFalse((self.folder / NAME).exists())
                path.unlink()

    def test_orphan_driver_recovery_record_blocks_another_update(self):
        path = self.folder / ".suite-driver-state.json"
        path.write_text("unfinished driver recovery")
        with self.open() as transaction:
            with self.assertRaisesRegex(RuntimeError, "driver recovery"):
                transaction.prepare(self.stage)
        self.assertEqual(self.snapshot(), self.originals)
        self.assertFalse((self.folder / NAME).exists())

    def test_journal_shape_types_duplicate_keys_and_permissions_fail_closed(self):
        self.prepare()
        path = self.folder / NAME
        encoded = path.read_text()
        value = json.loads(encoded)
        bad = [
            lambda v: v.update(schema=True),
            lambda v: v.update(phase="unknown"),
            lambda v: v.update(token="../../foreign"),
            lambda v: v.update(command=["/bin/sh"]),
            lambda v: v["parents"].update({NAMES[0]: [0, True]}),
            lambda v: v["originals"][BROKER].update(owner=999),
            lambda v: v["originals"][BROKER].update(mode=0o120777),
            lambda v: v["candidates"].update({NAMES[0]: None}),
            lambda v: v.update(workspace=None),
        ]
        for change in bad:
            mutated = copy.deepcopy(value)
            change(mutated)
            path.write_text(json.dumps(mutated))
            with self.open() as transaction:
                with self.assertRaises(ValueError):
                    transaction.load()
        path.write_text(encoded.replace('"schema":1', '"schema":1,"schema":1'))
        with self.open() as transaction:
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                transaction.load()
        path.write_text(encoded)
        path.chmod(0o644)
        with self.open() as transaction:
            with self.assertRaises(ValueError):
                transaction.load()

    def test_foreign_journal_inode_is_not_overwritten(self):
        self.prepare()
        with self.open() as transaction:
            transaction.load()
            path = self.folder / NAME
            path.rename(self.folder / "retained-record")
            path.write_text("foreign journal")
            with self.assertRaisesRegex(RuntimeError, "changed before"):
                transaction.apply()
            self.assertEqual(path.read_text(), "foreign journal")
        self.assertEqual(self.snapshot(), self.originals)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--crash":
        crash_child(Path(sys.argv[2]), sys.argv[3], sys.argv[4])
    else:
        unittest.main()
