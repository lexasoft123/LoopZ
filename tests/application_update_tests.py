"""User updates and mixed-owner maintenance use only private temporary targets."""

from contextlib import contextmanager, redirect_stdout
import copy
import fcntl
import io
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from application_ownership import APPLICATIONS, application_owners, snapshot_owner
from desktop_files import DesktopFileTransaction
from install_transaction import files
from uninstall_files import RemovalFiles
from uninstall_journal import RemovalJournal
from suite_files import SuiteFiles
from suite_update import NAMES, BROKER
import update_desktop
import manage_app_update
from desktop_install_tests import Host


@contextmanager
def root_view(owned):
    """Simulate root/user metadata while all actual IO stays under this account."""
    real_stat, real_fstat = os.stat, os.fstat
    real_path_stat, real_path_lstat = Path.stat, Path.lstat

    class Info:
        def __init__(self, original):
            self.original = original
            self.st_uid, self.st_gid = owned.get(original.st_ino, (0, original.st_gid))

        def __getattr__(self, key):
            return getattr(self.original, key)

    def chown(path, uid, gid, **kw):
        owned[real_stat(path, **kw).st_ino] = (uid, gid)

    with (
        patch.object(os, "stat", side_effect=lambda *a, **kw: Info(real_stat(*a, **kw))),
        patch.object(os, "fstat", side_effect=lambda *a, **kw: Info(real_fstat(*a, **kw))),
        patch.object(os, "geteuid", return_value=0),
        patch.object(os, "chown", side_effect=chown),
        patch.object(Path, "stat", lambda p, *a, **kw: Info(real_path_stat(p, *a, **kw))),
        patch.object(Path, "lstat", lambda p, *a, **kw: Info(real_path_lstat(p, *a, **kw))),
    ):
        yield


class OwnershipTests(unittest.TestCase):
    def test_policy_cannot_change_backend_or_nested_owners(self):
        self.assertEqual(set(application_owners(501, 20)), APPLICATIONS)
        for uid in [0, 202, 500, True, "501"]:
            with self.assertRaises(ValueError):
                application_owners(uid, 20)
        self.assertEqual(snapshot_owner("desktop", {"owner": 501}, 0, {"desktop"}), 501)
        self.assertEqual(snapshot_owner("driver", {"owner": 501}, 0, {"desktop"}), 0)
        self.assertEqual(snapshot_owner("desktop", {"owner": 202}, 0, {"desktop"}), 0)
        self.assertEqual(snapshot_owner("desktop", {"owner": True}, 0, {"desktop"}), 0)

    def test_root_app_migration_changes_only_app_owner_and_preserves_rollback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            stage = root / "stage"
            stage.mkdir(mode=0o700)
            destination = root / "installed"
            destination.mkdir()
            names = (*sorted(APPLICATIONS), "LoopZAudio.driver")
            targets = {name: destination / name for name in names}
            for name in names:
                (stage / name).mkdir()
                (stage / name / "payload").write_text("new")
                targets[name].mkdir()
                (targets[name] / "payload").write_text("old")
            owned = {}
            with root_view(owned):
                transaction = DesktopFileTransaction(
                    stage, targets, names, owners=application_owners(501, 20)
                )
                transaction.prepare()
                transaction.apply()
                for name in names:
                    for path in [targets[name], targets[name] / "payload"]:
                        self.assertEqual(path.stat().st_uid, 501 if name in APPLICATIONS else 0)
                transaction.rollback()
                transaction.finish()
                for name in names:
                    self.assertEqual(targets[name].stat().st_uid, 0)
                    self.assertEqual((targets[name] / "payload").read_text(), "old")
                with self.assertRaises(ValueError):
                    DesktopFileTransaction(
                        stage, targets, names, owners={"LoopZAudio.driver": (501, 20)}
                    )

    def test_root_uninstall_and_durable_recovery_accept_user_apps_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            support = root / "support"
            support.mkdir(mode=0o700)
            app = root / "LoopZ.app"
            app.mkdir()
            (app / "payload").write_text("saved app")
            broker = support / "broker"
            broker.write_text("saved broker")
            work = root / "loopz-uninstall-test"
            work.mkdir(mode=0o700)
            targets = {"desktop": app, "broker": broker}
            owned = {p.stat().st_ino: (501, 20) for p in [app, app / "payload"]}
            with root_view(owned):
                with RemovalFiles(targets, owner=0, user_components={"desktop"}) as removal:
                    inventory = removal.prepare()
                    self.assertEqual(inventory["desktop"]["owner"], 501)
                    self.assertEqual(inventory["broker"]["owner"], 0)
                    plan = {
                        "version": 1,
                        "mode": "loopz-uninstall",
                        "targets": {k: str(v) for k, v in targets.items()},
                        "inventory": inventory,
                        "before": {},
                        "preserve": [],
                    }
                    with RemovalJournal(
                        support, targets, owner=0, workspace_root=root, user_components={"desktop"}
                    ) as journal:
                        journal.begin(plan, removal, work)
                        removal.apply()
                    with RemovalJournal(
                        support, targets, owner=0, workspace_root=root, user_components={"desktop"}
                    ) as reopened:
                        result = reopened.recover_files()
                        self.assertEqual(result["status"], "originalFilesRestored")
                        self.assertEqual(app.stat().st_uid, 501)
                        self.assertEqual(broker.stat().st_uid, 0)
                        changed = copy.deepcopy(reopened.value)
                        changed["plan"]["inventory"]["broker"]["owner"] = 501
                        from uninstall_policy import digest

                        changed["planSHA256"] = digest(changed["plan"])
                        with self.assertRaises(ValueError):
                            reopened.validate(changed)
                # Nested ownership cannot be widened independently of its app.
                owned[(app / "payload").stat().st_ino] = (0, 0)
                with RemovalFiles(targets, owner=0, user_components={"desktop"}) as inspect:
                    with self.assertRaisesRegex(ValueError, "owned and protected"):
                        inspect.inventory()

    def test_coordinated_backend_update_keeps_apps_user_owned(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            support = root / "support"
            support.mkdir(mode=0o700)
            stage = root / "source"
            stage.mkdir(mode=0o700)
            installed = root / "installed"
            installed.mkdir()
            targets = {name: installed / name for name in NAMES}
            owned = {}
            for name in NAMES:
                for parent, text in [(stage, "new"), (installed, "old")]:
                    path = parent / name
                    if name == BROKER:
                        path.write_text(text)
                    else:
                        path.mkdir()
                        (path / "payload").write_text(text)
                if name in APPLICATIONS:
                    for path in [targets[name], targets[name] / "payload"]:
                        owned[path.stat().st_ino] = (501, 20)
            with root_view(owned):
                with SuiteFiles(support, targets, owner=0) as transaction:
                    transaction.prepare(stage)
                    transaction.apply()
                    for name in NAMES:
                        self.assertEqual(
                            targets[name].stat().st_uid, 501 if name in APPLICATIONS else 0
                        )
                    transaction.rollback()
                    for name in NAMES:
                        self.assertEqual(
                            targets[name].stat().st_uid, 501 if name in APPLICATIONS else 0
                        )


class UserUpdaterTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.package = self.root / "package"
        self.package.mkdir(mode=0o700)
        self.support = self.root / "system"
        self.support.mkdir()
        (self.support / ".install.lock").touch(mode=0o644)
        self.targets = {name: self.root / "Applications" / name for name in APPLICATIONS}
        for name, path in self.targets.items():
            path.mkdir(parents=True)
            (path / "payload").write_text("old")
            (self.package / name).mkdir()
            (self.package / name / "payload").write_text("new")
        (self.package / "manifest.json").write_text("{}")
        self.host = Host()
        self.host.uid = os.getuid()
        self.host.user = SimpleNamespace(pw_dir=str(self.root))
        self.host.existing_apps = lambda *args: None
        self.sha = "a" * 64

    def run_update(self, *, review=False):
        lock = update_desktop.shared_backend_lock
        out = io.StringIO()
        with (
            patch.object(update_desktop, "TARGETS", self.targets),
            patch.object(update_desktop, "SUPPORT", self.support),
            patch.object(update_desktop, "DesktopHost", return_value=self.host),
            patch.object(update_desktop, "verify_package", return_value=({}, self.sha)),
            patch.object(
                update_desktop,
                "shared_backend_lock",
                side_effect=lambda p, **kw: lock(p, expected_owner=os.getuid(), **kw),
            ),
            redirect_stdout(out),
        ):
            code = update_desktop.main(
                ["--package", str(self.package), "--manifest-sha", self.sha]
                + (["--check"] if review else [])
            )
        return code, json.loads(out.getvalue())

    def test_real_user_file_update_preserves_system_and_saves_private_receipt(self):
        code, result = self.run_update()
        self.assertEqual(code, 0)
        self.assertEqual(result["status"], "passed")
        self.assertFalse(result["requiresAdministrator"])
        for path in self.targets.values():
            self.assertEqual((path / "payload").read_text(), "new")
            self.assertEqual(path.stat().st_uid, os.getuid())
        receipt = self.root / "Library/Application Support/LoopZ/Updates/last-result.json"
        self.assertEqual(receipt.stat().st_mode & 0o777, 0o600)
        self.assertEqual(
            self.host.protected,
            {"driver": "same inode", "broker": "same pid", "document": "same bytes"},
        )

    def test_failed_validation_rolls_back_both_apps(self):
        self.host.fail_activation = True
        code, result = self.run_update()
        self.assertEqual(code, 1)
        self.assertTrue(result["filesRestored"])
        for path in self.targets.values():
            self.assertEqual((path / "payload").read_text(), "old")

    def test_check_does_not_create_records_or_promote(self):
        code, result = self.run_update(review=True)
        self.assertEqual(code, 0)
        self.assertFalse(result["deploymentVerified"])
        self.assertFalse((self.root / "Library").exists())
        for path in self.targets.values():
            self.assertEqual((path / "payload").read_text(), "old")

    def test_shared_lock_contends_with_existing_installer(self):
        path = self.support / ".install.lock"
        descriptor = os.open(path, os.O_RDWR)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(RuntimeError, "Another LoopZ"):
                with update_desktop.shared_backend_lock(path, expected_owner=os.getuid()):
                    pass
        finally:
            os.close(descriptor)

    def test_rejects_recovery_state_before_mutation(self):
        (self.support / ".uninstall-state.json").touch()
        with self.assertRaisesRegex(RuntimeError, "interrupted system"):
            self.run_update()
        for path in self.targets.values():
            self.assertEqual((path / "payload").read_text(), "old")

    def test_rejects_linked_or_foreign_owned_apps(self):
        app = next(iter(self.targets.values()))
        (app / "bad").symlink_to(self.root)
        with self.assertRaises(ValueError):
            update_desktop.require_user_apps(self.targets, os.getuid())
        (app / "bad").unlink()
        with self.assertRaises(update_desktop.MigrationRequired):
            update_desktop.require_user_apps(self.targets, os.getuid() + 1)


class ManagedUpdateTests(unittest.TestCase):
    def exercise(self, *, migration=False, review=False, failure=False):
        host = SimpleNamespace(uid=os.getuid(), existing_apps=lambda *args: None)
        events = []

        class Session:
            def __init__(self, host):
                pass

            def inspect(self):
                events.append("inspect")

            def pause(self):
                events.append("pause")

            def restore(self):
                events.append("restore")

        def run(arguments, **kw):
            events.append(arguments)
            if failure and arguments[0] != "/usr/bin/sudo":
                raise OSError("injected launch failure")
            return SimpleNamespace(returncode=0)

        own = update_desktop.MigrationRequired("old root ownership") if migration else None
        with (
            patch.object(manage_app_update, "DesktopHost", return_value=host),
            patch.object(manage_app_update, "verify_package"),
            patch.object(manage_app_update, "require_user_apps", side_effect=own),
            patch.object(manage_app_update, "EmptySession", Session),
            patch.object(manage_app_update.subprocess, "run", side_effect=run),
            patch.object(manage_app_update.sys.stdin, "isatty", return_value=True),
            redirect_stdout(io.StringIO()),
        ):
            args = ["--package", "/private/tmp/package", "--manifest-sha", "a" * 64]
            if migration:
                args += ["--migrate"]
            if review:
                args += ["--check"]
            if failure:
                with self.assertRaises(OSError):
                    manage_app_update.main(args)
            else:
                self.assertEqual(manage_app_update.main(args), 0)
        return events

    def test_ordinary_update_never_invokes_sudo_and_restores_session(self):
        events = self.exercise()
        self.assertEqual(len(events), 1)
        invocation = events[0]
        self.assertEqual(invocation[0], "/usr/bin/python3")
        self.assertTrue(invocation[4].endswith("/update_active_apps.py"))

    def test_migration_authorizes_before_pausing_and_calls_only_app_installer(self):
        events = self.exercise(migration=True)
        self.assertEqual(events[:3], ["inspect", ["/usr/bin/sudo", "-v"], "pause"])
        self.assertEqual(events[-1], "restore")
        self.assertTrue(events[3][6].endswith("/install_desktop.py"))
        self.assertNotIn("--replace-driver", events[3])

    def test_review_never_authorizes_or_pauses(self):
        self.assertEqual(self.exercise(migration=True, review=True), ["inspect"])

    def test_ordinary_child_launch_failure_never_pauses_in_the_parent(self):
        events = self.exercise(failure=True)
        self.assertEqual(len(events), 1)
        self.assertTrue(events[0][4].endswith("/update_active_apps.py"))


if __name__ == "__main__":
    unittest.main()
