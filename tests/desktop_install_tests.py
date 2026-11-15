"""Desktop package policy and rollback; every target is temporary."""

import copy
import json
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch
import os
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import desktop_install as install
from install_transaction import FileTransaction, files
from desktop_files import DesktopFileTransaction, identity, remove_at


class EngineLocationTests(unittest.TestCase):
    def test_dual_read_prefers_new_and_maintenance_targets_new(self):
        from engine_location import ENGINE_TARGET, NEW_ENGINE, read_engine_path

        self.assertEqual(ENGINE_TARGET, NEW_ENGINE)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            new, old = root / "new.app", root / "old.app"
            (old / "Contents/MacOS").mkdir(parents=True)
            (old / "Contents/MacOS/LoopZ Engine").write_text("fixture")
            (old / "Contents/MacOS/LoopZ Engine").chmod(0o755)
            self.assertEqual(read_engine_path(new, old), old)
            for kind in ("empty", "file", "symlink"):
                if kind == "empty":
                    new.mkdir()
                elif kind == "file":
                    new.write_text("stray")
                else:
                    new.symlink_to(old, target_is_directory=True)
                self.assertEqual(read_engine_path(new, old), old)
                if kind == "empty":
                    new.rmdir()
                else:
                    new.unlink()
            (new / "Contents/MacOS").mkdir(parents=True)
            executable = new / "Contents/MacOS/LoopZ Engine"
            executable.write_text("fixture")
            executable.chmod(0o755)
            self.assertEqual(read_engine_path(new, old), new)

    def test_existing_paths_include_legacy_and_damaged_real_bundles(self):
        from engine_location import installed_engine_path

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            new, old = root / "new.app", root / "old.app"
            self.assertEqual(installed_engine_path(new, old), new)
            old.mkdir()
            self.assertEqual(installed_engine_path(new, old), old)
            new.symlink_to(old, target_is_directory=True)
            self.assertEqual(installed_engine_path(new, old), old)
            new.unlink()
            new.mkdir()
            self.assertEqual(installed_engine_path(new, old), new)

    def test_account_job_wins_over_another_accounts_bundle(self):
        from engine_location import installed_engine_path

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            new, old, job = root / "new.app", root / "old.app", root / "engine.plist"
            new.mkdir()
            old.mkdir()
            for selected in (new, old):
                job.write_bytes(
                    plistlib.dumps(
                        {"ProgramArguments": [str(selected / "Contents/MacOS/LoopZ Engine")]}
                    )
                )
                self.assertEqual(installed_engine_path(new, old, job), selected)
            old.rmdir()
            job.write_bytes(
                plistlib.dumps({"ProgramArguments": [str(old / "Contents/MacOS/LoopZ Engine")]})
            )
            self.assertEqual(installed_engine_path(new, old, job), old)
            old.mkdir()
            job.write_bytes(
                plistlib.dumps({"ProgramArguments": [str(new / "Contents/MacOS/LoopZ Engine")]})
            )
            self.assertEqual(installed_engine_path(new, old, job), new)

    def test_inventory_imports_do_not_probe_an_unknown_or_other_users_job(self):
        import subprocess

        program = """
from unittest.mock import patch
import engine_location
with patch.object(engine_location, 'installed_engine_path', side_effect=AssertionError('job read at import')) as resolver:
    import desktop_install, routing_install, install_local, driver_update, active_app_update
    assert resolver.call_count == 0
"""
        subprocess.run(
            [sys.executable, "-c", program],
            cwd=Path(__file__).resolve().parents[1] / "scripts",
            check=True,
        )

    def test_lazy_inventory_resolves_once_when_used(self):
        import engine_location

        with patch.object(
            engine_location, "installed_engine_path", return_value=Path("/private/tmp/Engine.app")
        ) as resolver:
            path = engine_location.lazy_installed_engine_path()
            self.assertEqual(resolver.call_count, 0)
            self.assertEqual(path / "Contents", Path("/private/tmp/Engine.app/Contents"))
            self.assertEqual(os.fspath(path), "/private/tmp/Engine.app")
            self.assertEqual(resolver.call_count, 1)

    def test_uninstall_inventory_retains_both_engine_paths(self):
        import uninstall_policy
        from engine_location import NEW_ENGINE, LEGACY_ENGINE

        self.assertEqual(uninstall_policy.TARGETS["engine"], NEW_ENGINE)
        self.assertEqual(uninstall_policy.TARGETS["legacyEngine"], LEGACY_ENGINE)


class Guard:
    fail_stop = False
    fail_close = False
    closed = False

    def ready(self):
        pass

    def check(self):
        pass

    def stop(self):
        if self.fail_stop:
            raise RuntimeError("guard failed")
        return {"passed": True}

    def close(self):
        self.closed = True
        if self.fail_close:
            raise RuntimeError("close failed")


class Host:
    def __init__(self):
        self.state = {
            "defaultInput": "input",
            "defaultOutput": "output",
            "defaultSystemOutput": "system",
            "routingDevices": [],
        }
        self.identity = "same server generation"
        self.protected = {"driver": "same inode", "broker": "same pid", "document": "same bytes"}
        self.guard = Guard()
        self.fail_activation = self.fail_cleanup = self.change_protected = False
        self.calls = []

    def snapshot(self):
        return copy.deepcopy(self.state)

    def require_idle(self, snapshot):
        if snapshot["routingDevices"]:
            raise RuntimeError("active graph")

    def service_identity(self):
        return self.identity

    def protected_state(self):
        return copy.deepcopy(self.protected)

    def start_guard(self, work, before):
        return self.guard

    def validate_apps(self):
        self.calls.append("verify apps")
        return {"interfaceSealVerified": True}

    def validate_activation(self, work):
        self.state["routingDevices"] = [{"uid": "temporary validation"}]
        self.calls.append("publish")
        if self.change_protected:
            self.protected["document"] = "external change"
        if self.fail_activation:
            raise RuntimeError("activation failed")
        return {"captureOpened": False}

    def cleanup_validation(self):
        self.calls.append("withdraw")
        if self.fail_cleanup:
            raise RuntimeError("cannot withdraw")
        self.state["routingDevices"] = []

    def activation_diagnostics(self):
        return {"catalog": self.snapshot()}


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.stage = self.root / "stage"
        self.stage.mkdir()
        self.targets = {name: self.root / "installed" / name for name in install.NAMES}
        for name, path in self.targets.items():
            path.parent.mkdir(exist_ok=True)
            path.write_text("old " + name)
            (self.stage / name).write_text("new " + name)
        self.targets["LoopZ.app"].unlink()
        self.original = {name: files(path) for name, path in self.targets.items()}
        self.host = Host()

    def deploy(self, **options):
        return install.deploy(self.stage, self.targets, self.host, self.root, **options)

    def assert_originals(self):
        self.assertEqual(self.original, {name: files(path) for name, path in self.targets.items()})

    def test_success_preserves_system_and_withdraws_validation(self):
        result = self.deploy()
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["driverPreserved"] and result["brokerPreserved"])
        self.assertFalse(result["loginRegistered"] or result["userDocumentModified"])
        self.assertEqual(self.host.calls, ["verify apps", "publish", "withdraw"])
        self.assertTrue(self.host.guard.closed)
        self.assertFalse(list((self.root / "installed").glob(".*loopz-*")))

    def test_activation_failure_restores_both_files_and_missing_editor(self):
        self.host.fail_activation = True
        result = self.deploy()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["rolledBack"] and result["filesRestored"])
        self.assert_originals()
        self.assertEqual(self.host.state["routingDevices"], [])

    def test_partial_promotion_rolls_back_first_app(self):
        def transaction(*args):
            def replace(source, target):
                if "Engine" in target.name and ".loopz-new-" in source.name:
                    raise OSError("second promotion failed")
                source.rename(target)

            return FileTransaction(*args, replace=replace)

        result = self.deploy(transaction_factory=transaction)
        self.assertTrue(result["rolledBack"])
        self.assert_originals()

    def test_continuity_failure_rolls_back(self):
        self.host.guard.fail_stop = True
        result = self.deploy()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["filesRestored"])
        self.assertIn("repairRequired", result)
        self.assert_originals()

    def test_guard_startup_failure_does_not_prepare_or_replace_apps(self):
        def not_ready():
            raise RuntimeError("Physical continuity guard did not start")

        self.host.guard.ready = not_ready
        with patch.object(DesktopFileTransaction, "prepare") as prepare:
            result = self.deploy()
        prepare.assert_not_called()
        self.assertEqual(result["status"], "failed")
        self.assertEqual(self.host.calls, ["withdraw"])
        self.assertTrue(self.host.guard.closed)
        self.assert_originals()

    def test_protected_state_change_is_not_reported_as_successful_rollback(self):
        self.host.change_protected = True
        result = self.deploy()
        self.assertFalse(result["rolledBack"])
        self.assertIn("repairRequired", result)
        self.assertIsNone(result["userDocumentModified"])
        self.assert_originals()

    def test_cleanup_failure_attempts_file_restoration(self):
        self.host.fail_cleanup = True
        result = self.deploy()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["filesRestored"])
        self.assertFalse(result["rolledBack"])
        self.assert_originals()

    def test_guard_close_failure_retains_original_apps_for_repair(self):
        self.host.guard.fail_close = True
        result = self.deploy()
        self.assertEqual(result["status"], "failed")
        self.assertIn("repairRequired", result)
        self.assertTrue(result["retainedRecoveryFiles"])

    def test_active_graph_is_refused_before_file_changes(self):
        self.host.state["routingDevices"] = [{"uid": "active"}]
        with self.assertRaisesRegex(RuntimeError, "active graph"):
            self.deploy()
        self.assert_originals()

    def test_interrupted_validation_restores_original_apps(self):
        self.host.validate_activation = lambda work: (_ for _ in ()).throw(
            KeyboardInterrupt("cancelled")
        )
        result = self.deploy()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["rolledBack"])
        self.assert_originals()

    def test_interrupted_diagnostics_do_not_skip_rollback(self):
        self.host.fail_activation = True
        self.host.activation_diagnostics = lambda: (_ for _ in ()).throw(
            KeyboardInterrupt("diagnostic interrupted")
        )
        result = self.deploy()
        self.assertTrue(result["rolledBack"])
        self.assertIn("diagnosticError", result)
        self.assert_originals()

    def test_changed_candidate_is_not_removed_during_cleanup(self):
        transaction = DesktopFileTransaction(self.stage, self.targets, install.NAMES)
        transaction.prepare()
        candidate = transaction.candidates["LoopZ.app"]
        candidate.rename(candidate.with_name("moved-owned-candidate"))
        candidate.write_text("unrecognized replacement")
        with self.assertRaisesRegex(RuntimeError, "changed application"):
            transaction.finish()
        self.assertEqual(candidate.read_text(), "unrecognized replacement")
        self.assert_originals()

    def test_cleanup_unlinks_nested_symlink_without_following_it(self):
        outside = self.root / "outside"
        outside.mkdir()
        secret = outside / "keep"
        secret.write_text("must survive")
        app = self.root / "owned-app"
        app.mkdir()
        (app / "link").symlink_to(outside, target_is_directory=True)
        expected = identity(app.stat())
        parent = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            remove_at(parent, app.name, expected)
        finally:
            os.close(parent)
        self.assertFalse(app.exists())
        self.assertEqual(secret.read_text(), "must survive")

    def test_cross_filesystem_preparation_fails_before_promotion(self):
        transaction = DesktopFileTransaction(self.stage, self.targets, install.NAMES)
        parent = transaction.parent(self.targets["LoopZ.app"])
        original = os.fstat

        def status(fd):
            return SimpleNamespace(st_dev=-1) if fd == parent else original(fd)

        with patch("desktop_files.os.fstat", side_effect=status):
            with self.assertRaisesRegex(RuntimeError, "same filesystem"):
                transaction.prepare()
        self.assert_originals()


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.stage = Path(self.temporary.name).resolve()
        for name, executable in (("LoopZ.app", "LoopZ"), ("LoopZ Engine.app", "LoopZ Engine")):
            app = self.stage / name
            (app / "Contents/MacOS").mkdir(parents=True)
            binary = app / "Contents/MacOS" / executable
            binary.write_text("fixture code")
            binary.chmod(0o755)
            (app / "Contents/Info.plist").write_bytes(
                plistlib.dumps(
                    {
                        "CFBundleIdentifier": install.IDENTIFIERS[name],
                        "CFBundleExecutable": executable,
                    }
                )
            )
        helper = self.stage / "LoopZ.app/Contents/MacOS/LoopZLogin"
        helper.write_text("fixture login")
        helper.chmod(0o755)
        agent = (
            self.stage / "LoopZ.app/Contents/Library/LaunchAgents/com.lexasoft.loopz.login.plist"
        )
        agent.parent.mkdir(parents=True)
        agent.write_bytes(plistlib.dumps(install.LOGIN_AGENT))
        self.manifest = {
            "schema": 1,
            "kind": "desktop-review",
            "buildConfiguration": "Release",
            "installed": False,
            "loginRegistered": False,
            "identities": {
                name: {"identifier": identifier, "team": install.TEAM, "hardenedRuntime": True}
                for name, identifier in install.IDENTIFIERS.items()
            },
        }
        self.refresh()

    def refresh(self):
        hashes = files(self.stage)
        hashes.pop("manifest.json", None)
        self.manifest["files"] = hashes
        (self.stage / "manifest.json").write_text(json.dumps(self.manifest))

    def verify(self, expected=None):
        return install.verify_package(self.stage, lambda *args: None, expected)

    def test_complete_release_pair_passes_with_three_verified_identities(self):
        seen = []
        install.verify_package(self.stage, lambda *args: seen.append(args))
        self.assertEqual(len(seen), 3)
        self.assertEqual(sum(item[-1] for item in seen), 1)

    def test_debug_wrong_team_and_integer_booleans_are_rejected(self):
        for mutate in (
            lambda m: m.update(buildConfiguration="Debug"),
            lambda m: m["identities"]["LoopZ.app"].update(team="foreign"),
            lambda m: m["identities"]["LoopZ.app"].update(hardenedRuntime=1),
        ):
            original = copy.deepcopy(self.manifest)
            mutate(self.manifest)
            self.refresh()
            with self.assertRaises(ValueError):
                self.verify()
            self.manifest = original

    def test_mismatched_review_digest_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "changed after review"):
            self.verify("0" * 64)

    def test_unmanifested_file_and_changed_code_are_rejected(self):
        path = self.stage / "LoopZ.app/Contents/MacOS/LoopZ"
        path.write_text("tampered")
        with self.assertRaisesRegex(ValueError, "complete manifest"):
            self.verify()

    def test_links_extra_roots_and_traversal_are_rejected(self):
        link = self.stage / "LoopZ.app/linked"
        link.symlink_to("Contents")
        with self.assertRaises(ValueError):
            self.verify()
        link.unlink()
        (self.stage / "extra").write_text("unexpected")
        self.refresh()
        with self.assertRaises(ValueError):
            self.verify()
        (self.stage / "extra").unlink()
        self.refresh()
        self.manifest["files"]["../outside"] = "0" * 64
        (self.stage / "manifest.json").write_text(json.dumps(self.manifest))
        with self.assertRaises(ValueError):
            self.verify()

    def test_auto_enabled_login_job_is_rejected_even_if_rehashed(self):
        path = self.stage / "LoopZ.app/Contents/Library/LaunchAgents/com.lexasoft.loopz.login.plist"
        agent = dict(install.LOGIN_AGENT, KeepAlive=True)
        path.write_bytes(plistlib.dumps(agent))
        self.refresh()
        with self.assertRaisesRegex(ValueError, "one-shot"):
            self.verify()

    def test_login_helper_must_remain_executable(self):
        (self.stage / "LoopZ.app/Contents/MacOS/LoopZLogin").chmod(0o644)
        with self.assertRaisesRegex(ValueError, "not executable"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
