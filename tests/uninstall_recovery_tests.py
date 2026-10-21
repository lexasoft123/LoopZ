"""Recovery coordinator and private workspace rehearsals; no system audio/jobs."""

from contextlib import nullcontext, redirect_stdout, redirect_stderr
import io
import json
import os
import selectors
import subprocess
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from uninstall_journal_tests import JournalFixture
import uninstall_journal_tests as journal_tests
from uninstall_tests import Host, Guard
from uninstall_recovery import recover_product
from uninstall_workspace import DiagnosticWorkspaces, diagnostic_processes
from uninstall_host import UninstallHost, JOBS
from uninstall_driver import DriverRemoval
import uninstall_host
import uninstall_loopz
import uninstall_workspace


class RecoveryHost(Host):
    def __init__(self, document):
        super().__init__(document)
        self.calls = []

    def require_recovery_stopped(self, state, before):
        self.calls.append("admit recovery")
        self.require_stopped(state)

    def start_guard(self, *_):
        self.calls.append("start guard")
        self.guard = Guard(self)
        return self.guard

    def restore_services(self, before):
        self.calls.append("restore services")
        super().restore_services(before)

    def restore_driver(self, removal):
        self.calls.append("verify restored driver")
        self.fail("restore driver")


class RecoveryTests(JournalFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.document = self.root / "routes.json"
        self.document.write_text("saved user graph")
        self.host = RecoveryHost(self.document)
        self.plan["before"] = self.host.state()
        self.host.loaded = False
        self.begin()
        self.files.apply()
        self.workspaces = DiagnosticWorkspaces(self.root, processes=lambda _: [])
        self.factory = lambda *_: self.host

    def recover(self):
        with self.open_journal() as journal:
            return recover_product(journal, self.factory, self.workspaces)

    def test_original_files_services_and_workspace_finish_before_record_clear(self):
        result = self.recover()
        self.assertEqual(result["status"], "originalInstallationRecovered", result)
        self.assertFalse(result["journalRetained"])
        self.assertTrue(self.host.loaded and self.host.guard.closed)
        self.assert_originals()
        self.assertFalse(self.journal.pending())
        self.assertFalse(list(self.root.glob("loopz-uninstall-*")))
        self.assertLess(
            self.host.calls.index("start guard"), self.host.calls.index("restore services")
        )

    def test_committed_recovery_needs_no_installed_tools_or_service_changes(self):
        self.journal.begin_purge()
        self.targets["broker"].write_bytes(b"new installation")
        self.factory = lambda *_: self.fail("Committed cleanup must not construct a runtime host")
        result = self.recover()
        self.assertEqual(result["status"], "committedRemovalRecovered", result)
        self.assertEqual(result["newTargetsPreserved"], ["broker"])
        self.assertEqual(self.targets["broker"].read_bytes(), b"new installation")
        self.assertFalse(self.host.calls or self.host.loaded)
        self.assertFalse(self.journal.pending())

    def test_bad_context_is_rejected_before_old_workspace_or_files_change(self):
        def reject(*_):
            raise RuntimeError("wrong console context")

        self.factory = reject
        result = self.recover()
        self.assertEqual(result["status"], "recoveryRequired")
        self.assertTrue(self.work.exists() and self.journal.pending())
        self.assertFalse(self.targets["app"].exists())

    def test_runtime_failure_is_retryable_after_files_have_returned(self):
        self.host.failures = {"restore service"}
        first = self.recover()
        self.assertEqual(first["status"], "recoveryRequired")
        self.assert_originals()
        self.assertTrue(self.journal.pending())
        self.host.failures.clear()
        second = self.recover()
        self.assertEqual(second["status"], "originalInstallationRecovered", second)
        self.assertFalse(self.journal.pending())

    def test_recovery_preserves_current_defaults_documents_and_server_context(self):
        self.host.catalog["defaultOutput"] = "new external selection"
        self.document.write_text("new user graph")
        state = self.host.state
        self.host.state = lambda: {**state(), "coreAudio": "new server after reboot"}
        result = self.recover()
        self.assertEqual(result["status"], "originalInstallationRecovered", result)
        self.assertTrue(result["documentsChangedSinceOriginalReview"])
        self.assertEqual(self.host.catalog["defaultOutput"], "new external selection")
        self.assertEqual(self.document.read_text(), "new user graph")
        self.assertEqual(result["afterRecovery"]["coreAudio"], "new server after reboot")

    def test_failed_continuity_and_guard_cleanup_retain_record_and_new_workspace(self):
        self.host.failures = {"continuity", "close"}
        result = self.recover()
        self.assertEqual(result["status"], "recoveryRequired")
        self.assertTrue(result["repairRequired"])
        self.assertTrue(self.journal.pending())
        self.assertTrue(Path(result["retainedWorkspace"]["path"]).exists())

    def test_foreign_workspace_content_is_preserved(self):
        (self.work / "notes.txt").write_bytes(b"new user data")
        result = self.recover()
        self.assertEqual(result["status"], "recoveryRequired")
        self.assertEqual((self.work / "notes.txt").read_bytes(), b"new user data")
        self.assertFalse(self.targets["app"].exists())
        self.assertTrue(self.journal.pending())

    def test_committed_cleanup_does_not_stop_a_new_diagnostic(self):
        self.journal.begin_purge()
        self.workspaces.processes = lambda _: [123]
        result = self.recover()
        self.assertEqual(result["status"], "recoveryRequired")
        self.assertFalse((self.work / "guard.stop").exists())
        self.assertTrue(self.files.quarantined("broker").exists())

    def test_durable_record_remains_if_finalization_fails_after_workspace_cleanup(self):
        with self.open_journal() as journal:
            with patch.object(journal, "finish", side_effect=OSError("metadata sync failed")):
                result = recover_product(journal, self.factory, self.workspaces)
        self.assertEqual(result["status"], "recoveryRequired")
        self.assertTrue(self.journal.pending())
        self.assertFalse(Path(result["retainedWorkspace"]["path"]).exists())
        self.assertEqual(self.recover()["status"], "originalInstallationRecovered")


class WorkspaceTests(JournalFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.manager = DiagnosticWorkspaces(self.root, processes=lambda _: [])
        self.value = self.manager.describe(self.work)

    def test_old_diagnostic_receives_only_its_scoped_stop_file(self):
        self.manager.processes = lambda _: [] if (self.work / "guard.stop").exists() else [123]
        with patch("os.kill", side_effect=AssertionError("No PID signal is permitted")):
            self.manager.quiesce(self.value, request_stop=True)
        self.assertTrue((self.work / "guard.stop").exists())
        self.manager.clean(self.value)
        self.assertFalse(self.work.exists())

    def test_workspace_identity_change_blocks_stop_and_cleanup(self):
        self.work.rename(self.root / "retained-workspace")
        self.work.mkdir(mode=0o700)
        with self.assertRaises(RuntimeError):
            self.manager.quiesce(self.value, request_stop=True)
        with self.assertRaises(RuntimeError):
            self.manager.clean(self.value)
        self.assertFalse((self.work / "guard.stop").exists())

    def test_foreign_files_links_and_active_processes_are_not_cleaned(self):
        (self.work / "foreign").write_text("keep")
        with self.assertRaises(RuntimeError):
            self.manager.clean(self.value)
        (self.work / "foreign").unlink()
        (self.work / "guard.stop").symlink_to(self.targets["broker"])
        with self.assertRaises(RuntimeError):
            self.manager.quiesce(self.value, request_stop=True)
        (self.work / "guard.stop").unlink()
        self.manager.processes = lambda _: [123]
        with self.assertRaises(RuntimeError):
            self.manager.clean(self.value)
        self.assertTrue(self.work.exists())

    def test_missing_workspace_is_safe_only_without_matching_diagnostic(self):
        self.work.rmdir()
        self.manager.clean(self.value)
        self.manager.processes = lambda _: [123]
        with self.assertRaises(RuntimeError):
            self.manager.quiesce(self.value, request_stop=True)

    def test_mismatched_kernel_executable_is_rejected(self):
        target = self.work / "loopz-removal-check"
        with (
            patch.object(
                uninstall_workspace,
                "command",
                return_value=SimpleNamespace(stdout=f"123 {target}\n"),
            ),
            patch.object(uninstall_workspace, "executable", return_value="/foreign/tool"),
        ):
            with self.assertRaises(RuntimeError):
                diagnostic_processes(target)


class NativeAdmissionTests(unittest.TestCase):
    def test_malformed_original_context_cannot_construct_mac_host(self):
        for before in (None, {"jobs": list(JOBS)}, {"jobs": {"system/foreign": "123"}}):
            with patch.object(uninstall_host.MacHost, "__init__") as initialize:
                with self.assertRaises(ValueError):
                    UninstallHost.for_recovery(SimpleNamespace(), before)
                initialize.assert_not_called()

    def test_original_console_identity_is_required_before_checker_lookup(self):
        before = {
            "jobs": {key: None for key in JOBS},
            "consoleUID": 501,
            "login": {"registered": False, "requiresApproval": False},
            "catalog": {
                "defaultInput": "i",
                "defaultOutput": "o",
                "defaultSystemOutput": "s",
                "routingDevices": [],
                "routingBlockingRoutes": [],
                "loopzBlockingRoutes": [],
                "loopzVisible": False,
                "loopzRunning": False,
            },
            "documents": {},
            "coreAudio": "old",
        }

        def wrong_user(instance, *_):
            instance.uid = 502

        with (
            patch.object(uninstall_host.MacHost, "__init__", wrong_user),
            patch.object(uninstall_host, "verify_code") as verify,
        ):
            with self.assertRaisesRegex(RuntimeError, "original console"):
                UninstallHost.for_recovery(SimpleNamespace(), before)
            verify.assert_not_called()

    def test_restore_observer_has_no_signal_authority(self):
        source = SimpleNamespace(
            originals={"driver": {"recorded": True}},
            targets={"driver": Path("/private/fixture/LoopZAudio.driver")},
        )
        with patch.object(
            DriverRemoval, "discover", side_effect=AssertionError("No discovery at construction")
        ):
            observer = DriverRemoval.for_restoration(source)
        with patch("os.kill") as kill:
            with self.assertRaisesRegex(RuntimeError, "restoration-only"):
                observer.deactivate(source)
            kill.assert_not_called()


class RecoveryProcessLossTests(JournalFixture, unittest.TestCase):
    kill_at = journal_tests.ActualProcessLossTests.kill_at

    def recover_after_death(self, checkpoint):
        self.kill_at(checkpoint)
        document = self.root / "recovery-routes.json"
        host = RecoveryHost(document)
        with self.open_journal() as journal:
            result = recover_product(
                journal, lambda *_: host, DiagnosticWorkspaces(self.root, processes=lambda _: [])
            )
        self.assertEqual(result["status"], "originalInstallationRecovered", result)
        self.assert_originals()
        self.assertFalse(result["documentsChangedSinceOriginalReview"])
        self.assertFalse(self.journal.pending())
        self.assertFalse(list(self.root.glob("loopz-uninstall-*")))

    def test_death_after_new_workspace_record(self):
        self.recover_after_death("recovery-workspace-recorded")

    def test_death_after_original_files_return(self):
        self.recover_after_death("recovery-files-restored")

    def test_death_after_workspace_cleanup_before_finalization(self):
        self.recover_after_death("recovery-workspace-cleaned")


class NativeDiagnosticProcessTests(JournalFixture, unittest.TestCase):
    def test_actual_native_child_is_identified_and_stops_without_a_signal(self):
        source = Path(__file__).parent / "editor/diagnostic_process_fixture.cpp"
        binary = self.work / "loopz-removal-check"
        subprocess.run(
            ["/usr/bin/clang++", "-std=c++20", str(source), "-o", str(binary)],
            check=True,
            capture_output=True,
            timeout=30,
        )
        child = subprocess.Popen(
            [str(binary), str(self.work / "guard.stop")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        selector = selectors.DefaultSelector()
        selector.register(child.stdout, selectors.EVENT_READ)
        manager = DiagnosticWorkspaces(self.root)
        try:
            self.assertTrue(selector.select(5))
            self.assertEqual(child.stdout.readline().strip(), "ready")
            self.assertIsNone(child.poll())
            self.assertEqual(diagnostic_processes(binary), [child.pid])
            manager.quiesce(manager.describe(self.work), request_stop=True)
            _, error = child.communicate(timeout=5)
            self.assertEqual(child.returncode, 0, error)
            manager.clean(manager.describe(self.work))
            self.assertFalse(self.work.exists())
        finally:
            selector.close()
            if child.poll() is None:
                child.terminate()  # Only the test's own child on assertion failure.
                child.communicate(timeout=5)


class RecoveryCommandTests(unittest.TestCase):
    def test_recovery_requires_administrator_before_file_or_host_access(self):
        with (
            patch.object(uninstall_loopz.os, "geteuid", return_value=501),
            patch.object(uninstall_loopz, "pending_recovery") as pending,
            redirect_stderr(io.StringIO()),
        ):
            with self.assertRaises(SystemExit):
                uninstall_loopz.main(["--recover"])
            pending.assert_not_called()

    def test_no_pending_record_is_a_read_only_noop_without_host_lookup(self):
        with (
            patch.object(uninstall_loopz.os, "geteuid", return_value=0),
            patch.object(uninstall_loopz, "pending_recovery", return_value=None),
            patch.object(uninstall_loopz, "UninstallHost") as host,
            redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(uninstall_loopz.main(["--recover"]), 0)
            self.assertTrue(json.loads(output.getvalue())["readOnly"])
            host.assert_not_called()

    def test_pending_recovery_dispatches_before_missing_tool_constructor(self):
        with (
            patch.object(uninstall_loopz.os, "geteuid", return_value=0),
            patch.object(
                uninstall_loopz, "pending_recovery", return_value={"status": "recoveryRequired"}
            ),
            patch.object(uninstall_loopz, "exclusive_lock", return_value=nullcontext()),
            patch.object(uninstall_loopz, "RemovalJournal"),
            patch.object(uninstall_loopz, "DiagnosticWorkspaces"),
            patch.object(
                uninstall_loopz,
                "recover_product",
                return_value={"status": "committedRemovalRecovered"},
            ) as recover,
            patch.object(uninstall_loopz, "UninstallHost") as host,
            redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(uninstall_loopz.main(["--recover"]), 0)
            host.assert_not_called()
            recover.assert_called_once()


if __name__ == "__main__":
    unittest.main()
