"""Production update/recovery wiring with private app trees and fake service APIs."""

import copy
from functools import partial
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import user_app_transaction as transaction_module
from active_app_update import deploy_active
from app_recovery_context import digest, engine_fingerprint, validate_context
from app_update_admission import require_no_pending_app_update
from app_update_journal import AppUpdateJournal
from desktop_install import NAMES, deploy
from desktop_install_tests import Guard
from install_transaction import files
from install_routing import MacHost
from install_desktop import DesktopHost
from uninstall_host import UninstallHost
from recover_app_update import recover
from user_app_transaction import UserAppTransaction

LOGIN = {"available": True, "enabled": False, "registered": False, "requiresApproval": False}


def targets(root):
    return {name: root / "Applications" / name for name in NAMES}


def verify_fixture(name, path):
    if name not in NAMES or not (path / "payload").is_file():
        raise ValueError("The fixture application signature was rejected")


class Session:
    def __init__(self, host):
        self.host = host
        self.definition = b"fixed canonical definition"
        self.ui_closed = False
        self.document_bytes = None
        self.last_acknowledgement = None
        self.paused = False

    def read_intent(self):
        self.document_bytes = self.host.document.read_bytes()
        return json.loads(self.document_bytes)

    def close_interface(self, **kwargs):
        self.host.events.append("close interface")

    def pause_engine(self):
        self.host.events.append("pause engine")
        self.paused = True
        if self.host.fail_pause:
            raise RuntimeError("Engine stop acknowledgement lost")

    def stop_for_rollback(self):
        self.host.events.append("stop candidate")
        self.paused = True

    def restore(self):
        self.host.events.append("restore routing")
        if self.host.fail_restore or (
            self.host.fail_new_restore
            and (targets(self.host.root)[NAMES[0]] / "payload").read_text() == "new"
        ):
            raise RuntimeError("Routing restoration failed")
        self.paused = False
        self.last_acknowledgement = {
            "document": json.loads(self.host.document.read_bytes()),
            "runtime": {"restored": not self.host.disconnected},
        }
        return self.last_acknowledgement

    def restore_current(self):
        self.host.events.append("restore current intent")
        return self.restore()

    def reopen_interface(self):
        self.host.events.append("reopen interface")


class Host:
    def __init__(self, root):
        self.root = root
        self.uid = os.getuid()
        self.document = root / "routes.json"
        self.session = Session(self)
        self.expected_login = copy.deepcopy(LOGIN)
        self.events = []
        self.guard = Guard()
        self.fail_new_restore = self.fail_restore = self.fail_pause = self.disconnected = False

    def snapshot(self):
        return {
            "defaultInput": "input", "defaultOutput": "output", "defaultSystemOutput": "system",
            "routingDevices": json.loads(self.document.read_bytes())["devices"],
        }

    def require_idle(self, state):
        pass

    def service_identity(self):
        return "same Core Audio generation"

    def protected_state(self):
        return {"driver": "same inode", "broker": "same pid", "routes": self.document.read_bytes()}

    def start_guard(self, work, before):
        return self.guard

    def validate_apps(self):
        self.events.append("validate signed apps")
        return {"desktop": self.app_status()}

    def app_status(self):
        return {"version": 1, "login": copy.deepcopy(self.expected_login)}

    def validate_activation(self, work):
        return self.session.restore()

    def cleanup_validation(self):
        pass

    def activation_diagnostics(self):
        return {"test": True}


def context(root, *, interface=False):
    return {
        "version": 1, "mode": "interface" if interface else "routing",
        "manifestSHA256": "a" * 64, "interfaceWasOpen": True,
        "sessionDefinitionSHA256": None if interface else digest(b"fixed canonical definition"),
        "login": copy.deepcopy(LOGIN),
        "unchangedEngine": (
            engine_fingerprint(targets(root)[NAMES[1]], os.getuid()) if interface else None
        ),
    }


def run_update(root, *, interface=False, host=None):
    host = host or Host(root)
    with AppUpdateJournal(root / "updates", targets(root)) as journal:
        factory = partial(
            UserAppTransaction, journal=journal, context=context(root, interface=interface),
            verify=verify_fixture,
        )
        if interface:
            host.validate_activation = lambda work: {"engineSessionStopped": False}
            return deploy(
                root / "package", targets(root), host, root, transaction_factory=factory,
                names=(NAMES[0],),
            )
        return deploy_active(
            root / "package", targets(root), host, root, transaction_factory=factory
        )


def child(root, stop, interface):
    def checkpoint(event):
        if event == stop:
            os.kill(os.getpid(), signal.SIGKILL)

    rename = transaction_module.rename_exclusive
    phase = AppUpdateJournal.phase
    count = 0

    def renamed(*args, **kwargs):
        nonlocal count
        rename(*args, **kwargs)
        count += 1
        checkpoint("rename:" + str(count))

    def phased(self, name):
        phase(self, name)
        checkpoint("phase:" + name)

    transaction_module.rename_exclusive = renamed
    AppUpdateJournal.phase = phased
    result = run_update(root, interface=interface)
    raise RuntimeError("Crash checkpoint not reached: " + str(result))


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            dir="/private/tmp", prefix="loopz-user-recovery-"
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "updates").mkdir(mode=0o700)
        (self.root / "package").mkdir(mode=0o700)
        for name, path in targets(self.root).items():
            path.mkdir(parents=True)
            (path / "payload").write_text("old")
            (self.root / "package" / name).mkdir()
            (self.root / "package" / name / "payload").write_text("new")
        (self.root / "routes.json").write_text(
            json.dumps({"version": 2, "devices": [{"id": "mix"}]})
        )
        self.host = Host(self.root)
        self.original = {name: files(path) for name, path in targets(self.root).items()}
        self.original_inodes = {
            name: path.stat().st_ino for name, path in targets(self.root).items()
        }

    def crash(self, stop, *, interface=False):
        result = subprocess.run(
            [sys.executable, "-B", "-E", "-s", "-S", __file__, "--crash", str(self.root), stop,
             "interface" if interface else "routing"],
            capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, -signal.SIGKILL, result.stdout + result.stderr)

    def recover(self):
        with AppUpdateJournal(self.root / "updates", targets(self.root)) as journal:
            with patch("recover_app_update.TARGETS", targets(self.root)):
                result = recover(journal, self.host, verify=verify_fixture, session_factory=Session)
            self.assertFalse(journal.pending())
        return result

    def assert_original(self):
        self.assertEqual(
            {name: files(path) for name, path in targets(self.root).items()}, self.original
        )
        self.assertEqual(
            {name: path.stat().st_ino for name, path in targets(self.root).items()},
            self.original_inodes,
        )

    def test_success_commits_only_after_runtime_and_guard_validation(self):
        result = run_update(self.root, host=self.host)
        self.assertEqual(result["status"], "passed", result)
        self.assertEqual(
            self.host.events, ["pause engine", "validate signed apps", "restore routing"]
        )
        self.assertFalse((self.root / "updates/.app-update-state.json").exists())

    def test_candidate_restore_failure_rolls_back_apps_and_original_runtime(self):
        self.host.fail_new_restore = True
        result = run_update(self.root, host=self.host)
        self.assertEqual(result["status"], "failed", result)
        self.assertTrue(result["rolledBack"])
        self.assert_original()
        self.assertFalse((self.root / "updates/.app-update-state.json").exists())
        self.assertIn("stop candidate", self.host.events)

    def test_uncertain_stop_acknowledgement_restores_original_session(self):
        self.host.fail_pause = True
        result = run_update(self.root, host=self.host)
        self.assertEqual(result["status"], "failed", result)
        self.assertTrue(result["rolledBack"])
        self.assert_original()
        self.assertFalse(self.host.session.paused)

    def test_runtime_failure_retains_journal_until_explicit_recovery(self):
        self.host.fail_restore = True
        result = run_update(self.root, host=self.host)
        self.assertIn("repairRequired", result)
        self.assertTrue((self.root / "updates/.app-update-state.json").exists())
        self.host.fail_restore = False
        self.assertEqual(self.recover()["outcome"], "original-apps-restored")
        self.assert_original()

    def test_guard_cleanup_failure_never_commits_or_deletes_old_apps(self):
        def failed():
            raise RuntimeError("Guard cleanup failed")
        self.host.guard.close = failed
        result = run_update(self.root, host=self.host)
        self.assertIn("repairRequired", result)
        with AppUpdateJournal(self.root / "updates", targets(self.root)) as journal:
            self.assertEqual(journal.load()["phase"], "validating")
        self.recover()
        self.assert_original()

    def test_actual_sigkill_after_each_move_restores_current_saved_mix(self):
        for stop in (
            "phase:prepared", "phase:activating",
            *[f"rename:{i}" for i in range(1, 5)], "phase:validating",
        ):
            with self.subTest(stop=stop):
                fixture = RecoveryTests()
                fixture.setUp()
                try:
                    fixture.crash(stop)
                    current = {"version": 2, "devices": [{"id": "newer saved mix"}]}
                    fixture.host.document.write_text(json.dumps(current))
                    result = fixture.recover()
                    fixture.assert_original()
                    self.assertEqual(result["currentRoutingAcknowledgement"]["document"], current)
                    self.assertEqual(json.loads(fixture.host.document.read_bytes()), current)
                finally:
                    fixture.doCleanups()

    def test_committed_crash_keeps_new_apps_and_does_not_stop_engine(self):
        self.crash("phase:committed")
        result = self.recover()
        self.assertEqual(result["outcome"], "committed-update-retained")
        self.assertFalse(result["engineSessionStopped"])
        self.assertNotIn("stop candidate", self.host.events)
        for path in targets(self.root).values():
            self.assertEqual((path / "payload").read_text(), "new")

    def test_interface_crash_never_stops_or_restores_engine(self):
        self.crash("rename:1", interface=True)
        result = self.recover()
        self.assertFalse(result["engineSessionStopped"])
        self.assertIsNone(result["currentRoutingAcknowledgement"])
        self.assert_original()
        self.assertEqual(self.host.events, ["close interface", "reopen interface"])

    def test_interface_recovery_detects_external_engine_changes_without_reverting_them(self):
        self.crash("rename:1", interface=True)

        class ChangedEngineSession(Session):
            def close_interface(self, **kwargs):
                super().close_interface(**kwargs)
                (targets(self.host.root)[NAMES[1]] / "payload").write_text("external edit")

        with AppUpdateJournal(self.root / "updates", targets(self.root)) as journal:
            with patch("recover_app_update.TARGETS", targets(self.root)):
                with self.assertRaisesRegex(RuntimeError, "Engine changed"):
                    recover(
                        journal, self.host, verify=verify_fixture,
                        session_factory=ChangedEngineSession,
                    )
            self.assertTrue(journal.pending())
        self.assertEqual((targets(self.root)[NAMES[1]] / "payload").read_text(), "external edit")
        self.assertNotIn("stop candidate", self.host.events)

    def test_unavailable_hardware_preserves_graph_and_reports_pending_recovery(self):
        self.crash("phase:validating")
        self.host.disconnected = True
        result = self.recover()
        self.assertTrue(result["restorationPending"])
        self.assertEqual(
            result["currentRoutingAcknowledgement"]["document"]["devices"], [{"id": "mix"}]
        )

    def test_changed_session_definition_stops_recovery_before_any_process_operation(self):
        self.crash("phase:validating")
        with AppUpdateJournal(self.root / "updates", targets(self.root)) as journal:
            journal.load()
            value = copy.deepcopy(journal.value)
            value["context"]["sessionDefinitionSHA256"] = "b" * 64
            journal.save(value)
        with self.assertRaisesRegex(RuntimeError, "definition changed"):
            self.recover()
        self.assertFalse(self.host.events)

    def test_invalid_context_never_executes_a_stored_command_or_changes_apps(self):
        self.crash("phase:validating")
        with AppUpdateJournal(self.root / "updates", targets(self.root)) as journal:
            journal.load()
            value = copy.deepcopy(journal.value)
            value["context"]["command"] = ["arbitrary", "command"]
            journal.save(value)
        with self.assertRaises(ValueError):
            self.recover()
        self.assertFalse(self.host.events)

    def test_chosen_app_signature_is_verified_before_closing_gui_or_engine(self):
        self.crash("rename:1")
        def rejected(*args):
            raise ValueError("Signature rejected")
        with AppUpdateJournal(self.root / "updates", targets(self.root)) as journal:
            with self.assertRaisesRegex(ValueError, "Signature rejected"):
                recover(journal, self.host, verify=rejected, session_factory=Session)
        self.assertFalse(self.host.events)

    def test_pending_journal_blocks_other_maintenance_and_second_update(self):
        self.crash("phase:prepared")
        with patch("app_update_admission.updates_directory", return_value=self.root / "updates"):
            with self.assertRaisesRegex(RuntimeError, "--recover"):
                require_no_pending_app_update(self.host.uid)

    def test_context_rejects_mode_role_mismatch_and_invalid_login_choice(self):
        value = context(self.root)
        for modify in (
            lambda v: v.update(mode="interface"),
            lambda v: v.update(interfaceWasOpen=1),
            lambda v: v.update(sessionDefinitionSHA256="wrong"),
            lambda v: v["login"].update(enabled=True, registered=False),
        ):
            changed = copy.deepcopy(value)
            modify(changed)
            with self.assertRaises((ValueError, RuntimeError)):
                validate_context(changed, NAMES)

    def test_privileged_maintenance_rechecks_after_initial_admission(self):
        host = UninstallHost.__new__(UninstallHost)
        host.uid = self.host.uid
        with patch("app_update_admission.updates_directory", return_value=self.root / "updates"):
            MacHost.require_app_recovery_clear(host)
            (self.root / "updates/.app-update-state.json").write_text("{}")
            for operation in (
                lambda: MacHost.require_app_recovery_clear(host),
                lambda: DesktopHost.protected_state(host),
                lambda: UninstallHost.require_stopped(host, {}),
            ):
                with self.assertRaisesRegex(RuntimeError, "--recover"):
                    operation()

    def test_explicit_recovery_requires_no_package_or_manifest_options(self):
        from manage_app_update import main

        with patch("recover_app_update.main", return_value=0) as recovered:
            self.assertEqual(main(["--recover"]), 0)
        recovered.assert_called_once_with([])

    def test_journal_cannot_use_root_real_uid_with_user_effective_uid(self):
        with patch("app_update_journal.os.getuid", return_value=0):
            with self.assertRaises(ValueError):
                AppUpdateJournal(self.root / "updates", targets(self.root))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--crash":
        child(Path(sys.argv[2]), sys.argv[3], sys.argv[4] == "interface")
    else:
        unittest.main()
