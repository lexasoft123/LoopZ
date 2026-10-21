"""Interface-only replacement must preserve active routing and the Engine."""

from contextlib import redirect_stdout, redirect_stderr
import copy
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
import editor_update
import update_editor
import manage_editor_update
from editor_update import EDITOR, ENGINE, EditorUpdateHost, PassiveSessionGuard, deploy_editor
from install_transaction import files
from desktop_install_tests import Host


class ActiveHost(Host):
    def __init__(self, targets, document):
        super().__init__()
        self.targets = targets
        self.document = document
        self.pid = 1234
        self.state["routingDevices"] = [{"uid": "retained-device", "alive": True, "running": True}]
        self.guard = PassiveSessionGuard(self.engine_state)
        self.uid = os.getuid()
        self.existing_apps = lambda *args: None

    def engine_state(self):
        path = self.targets[ENGINE]
        return {"files": files(path), "inode": path.stat().st_ino, "pid": self.pid}

    def require_idle(self, snapshot):
        pass  # Engine and public IO clients are intentionally running.

    def protected_state(self):
        return {
            **super().protected_state(),
            "engine": self.engine_state(),
            "document": self.document.read_bytes(),
        }

    def validate_activation(self, work):
        self.calls.append("observe existing engine")
        return EditorUpdateHost.validate_activation(self, work)

    def cleanup_validation(self):
        self.calls.append("no validation device to withdraw")


class EditorTransactionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.package = self.root / "package"
        self.package.mkdir(mode=0o700)
        self.targets = {name: self.root / "Applications" / name for name in [EDITOR, ENGINE]}
        for name, target in self.targets.items():
            target.mkdir(parents=True)
            (target / "binary").write_text("current " + name)
            (self.package / name).mkdir()
            (self.package / name / "binary").write_text(
                "current " + name if name == ENGINE else "new interface"
            )
        (self.package / "manifest.json").write_text("{}")
        self.document = self.root / "routes.json"
        self.document.write_text(json.dumps({"version": 2, "devices": [{"id": "retained-mix"}]}))
        self.host = ActiveHost(self.targets, self.document)
        self.original = {name: files(path) for name, path in self.targets.items()}
        self.engine_inode = self.targets[ENGINE].stat().st_ino
        self.saved_document = self.document.read_bytes()

    def deploy(self):
        return deploy_editor(self.package, self.targets, self.host, self.root)

    def assert_engine_and_graph(self):
        self.assertEqual(files(self.targets[ENGINE]), self.original[ENGINE])
        self.assertEqual(self.targets[ENGINE].stat().st_ino, self.engine_inode)
        self.assertEqual(self.document.read_bytes(), self.saved_document)
        self.assertEqual(
            self.host.state["routingDevices"],
            [{"uid": "retained-device", "alive": True, "running": True}],
        )

    def test_updates_only_interface_with_active_mix_and_clients(self):
        result = self.deploy()
        self.assertEqual(result["status"], "passed")
        self.assertFalse(result["engineReplaced"] or result["engineSessionStopped"])
        self.assertFalse(result["activation"]["validationDevicePublished"])
        self.assertFalse(result["runtimePreservation"]["audioOpened"])
        self.assertNotIn("hardwareContinuity", result)
        self.assertEqual((self.targets[EDITOR] / "binary").read_text(), "new interface")
        self.assert_engine_and_graph()
        self.assertEqual(self.host.pid, 1234)

    def test_registered_login_is_reported_without_claiming_the_updater_registered_it(self):
        self.host.validate_apps = lambda: {
            "desktop": {
                "version": 1,
                "login": {
                    "available": True,
                    "enabled": True,
                    "registered": True,
                    "requiresApproval": False,
                },
            }
        }
        result = self.deploy()
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["loginRegistered"])
        self.assertFalse(result["loginRegisteredByInstaller"])
        self.assert_engine_and_graph()

    def test_signature_or_interface_failure_rolls_back_without_touching_audio(self):
        def failed():
            raise RuntimeError("new interface seal is invalid")

        self.host.validate_apps = failed
        result = self.deploy()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["filesRestored"])
        self.assertEqual(files(self.targets[EDITOR]), self.original[EDITOR])
        self.assert_engine_and_graph()

    def test_engine_update_is_rejected_before_preparation(self):
        (self.package / ENGINE / "binary").write_text("different engine")
        with patch.object(editor_update, "deploy") as transaction:
            with self.assertRaisesRegex(RuntimeError, "unchanged installed Engine"):
                self.deploy()
        transaction.assert_not_called()
        self.assertEqual(files(self.targets[EDITOR]), self.original[EDITOR])
        self.assert_engine_and_graph()

    def test_engine_restart_during_update_cannot_be_certified_as_uninterrupted(self):
        def restarted():
            self.host.pid = 9999
            return {"interfaceSealVerified": True}

        self.host.validate_apps = restarted
        result = self.deploy()
        self.assertEqual(result["status"], "failed")
        self.assertEqual(files(self.targets[EDITOR]), self.original[EDITOR])
        self.assertIn("repairRequired", result)
        self.assert_engine_and_graph()

    def test_guard_detects_engine_change_without_opening_audio(self):
        self.host.guard.ready()
        self.host.pid = 9999
        with self.assertRaisesRegex(RuntimeError, "Engine or its session changed"):
            self.host.guard.check()

    def test_cli_as_user_does_not_require_empty_graph_or_replace_engine(self):
        self.host.user = SimpleNamespace(pw_dir=str(self.root))
        self.host.expected_login = {
            "available": True, "enabled": False, "registered": False, "requiresApproval": False
        }
        self.support = self.root / "system"
        self.support.mkdir()
        (self.support / ".install.lock").touch(mode=0o644)
        real_lock = update_editor.shared_backend_lock

        class Lease:
            def __init__(lease, *args):
                lease.work = self.root / "work"
                lease.work.mkdir()
                lease.tools = "b" * 64
                lease.state = SimpleNamespace(
                    value={"token": "c" * 32}, record_intent=lambda context: None
                )

            def __enter__(lease):
                return lease

            def __exit__(lease, *args):
                pass

            def require_ready(lease):
                pass

        def same(package):
            return editor_update.require_same_engine(package, self.targets)

        out = io.StringIO()
        with (
            patch.object(update_editor, "TARGETS", self.targets),
            patch("app_recovery_context.TARGETS", self.targets),
            patch("user_app_transaction.verify_code"),
            patch.object(update_editor, "SUPPORT", self.support),
            patch.object(update_editor, "EditorUpdateHost", return_value=self.host),
            patch.object(update_editor, "AppRecoveryLease", Lease),
            patch.object(update_editor, "verify_package", return_value=({}, "a" * 64)),
            patch.object(update_editor, "require_same_engine", side_effect=same),
            patch.object(
                update_editor,
                "shared_backend_lock",
                side_effect=lambda p, **kw: real_lock(p, expected_owner=os.getuid(), **kw),
            ),
            redirect_stdout(out),
        ):
            code = update_editor.main(["--package", str(self.package), "--manifest-sha", "a" * 64])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.getvalue())["mode"], "user-interface-update")
        self.assert_engine_and_graph()


class NativeHostPolicyTests(unittest.TestCase):
    def test_active_devices_do_not_require_stopping_engine(self):
        host = EditorUpdateHost.__new__(EditorUpdateHost)
        host.engine_state = lambda: {"pid": 1}
        with patch.object(
            editor_update,
            "command",
            return_value=SimpleNamespace(returncode=1, stdout="", stderr=""),
        ) as run:
            host.require_idle({"routingDevices": [{"running": True}]})
        self.assertEqual(run.call_args.args, ("/usr/bin/pgrep", "-x", "LoopZ"))

    def test_running_desktop_blocks_replacement(self):
        host = EditorUpdateHost.__new__(EditorUpdateHost)
        host.engine_state = lambda: {"pid": 1}
        with (
            patch.object(
                editor_update,
                "command",
                return_value=SimpleNamespace(returncode=0, stdout="100\n", stderr=""),
            ),
            patch.object(
                editor_update,
                "executable",
                return_value="/Applications/LoopZ.app/Contents/MacOS/LoopZ",
            ),
        ):
            with self.assertRaisesRegex(RuntimeError, "Close the desktop"):
                host.require_idle({"routingDevices": [{"running": True}]})

    def test_session_generation_is_observed_without_control_commands(self):
        host = SimpleNamespace(uid=501, domain="gui/501")
        calls = []

        def run(*args, **kw):
            calls.append(args)
            if args[0] == "/bin/ps":
                return SimpleNamespace(
                    returncode=0,
                    stdout="501 123 Mon Oct 20 /Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
                    stderr="",
                )
            if args[-1].endswith(".session"):
                return SimpleNamespace(returncode=0, stdout="  pid = 123\n", stderr="")
            return SimpleNamespace(returncode=113, stdout="", stderr="Could not find service")

        host.user_command = run
        with patch.object(
            editor_update,
            "executable",
            return_value="/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
        ):
            result = editor_update.session_generations(host, Path("/Applications/LoopZ Engine.app"))
        self.assertIn("123", result["com.lexasoft.loopz.session"])
        self.assertTrue(all("bootout" not in call and "bootstrap" not in call for call in calls))

    def test_validation_does_not_publish_device_or_open_output(self):
        host = EditorUpdateHost.__new__(EditorUpdateHost)
        with patch.object(editor_update, "command") as run:
            status = host.validate_activation(Path("/private/tmp"))
            host.cleanup_validation()
        run.assert_not_called()
        self.assertFalse(status["updateOpenedPhysicalOutput"])

    def test_wrong_engine_owner_or_unreadable_session_does_not_get_ignored(self):
        host = SimpleNamespace(uid=501, domain="gui/501")
        host.user_command = lambda *a, **kw: SimpleNamespace(
            returncode=1, stdout="", stderr="Not privileged"
        )
        with self.assertRaisesRegex(RuntimeError, "could not be inspected"):
            editor_update.session_generations(host, Path("/Applications/LoopZ Engine.app"))

    def test_unknown_engine_process_cannot_pass_the_guard(self):
        host = SimpleNamespace(uid=501, domain="gui/501")
        host.user_command = lambda *a, **kw: SimpleNamespace(
            returncode=0, stdout="  pid = 123\n", stderr=""
        )
        with patch.object(editor_update, "executable", return_value="/unrelated/program"):
            with self.assertRaisesRegex(RuntimeError, "unrecognized process"):
                editor_update.session_generations(host, Path("/Applications/LoopZ Engine.app"))


class LoginPreservationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.package = self.root / "package"
        self.targets = {name: self.root / "installed" / name for name in [EDITOR, ENGINE]}
        for name in [EDITOR, ENGINE]:
            for root in [self.package, self.root / "installed"]:
                app = root / name
                app.mkdir(parents=True)
                (app / "binary").write_text("same engine or editor")
        for relative in editor_update.LOGIN_FILES:
            for app in [self.package / EDITOR, self.targets[EDITOR]]:
                target = app / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("same signed helper or job")
        self.status = {
            "version": 1,
            "login": {
                "available": True,
                "registered": True,
                "enabled": True,
                "requiresApproval": False,
            },
        }
        self.host = EditorUpdateHost.__new__(EditorUpdateHost)
        self.host.expected_login = None
        self.host.app_status = lambda: copy.deepcopy(self.status)

    def review(self):
        with (
            patch.object(editor_update, "TARGETS", self.targets),
            patch.object(editor_update, "verify_code"),
        ):
            self.host.existing_apps(self.package, False, True)

    def test_enabled_login_can_update_when_sealed_helper_and_job_are_unchanged(self):
        self.review()
        self.assertEqual(self.host.expected_login, self.status["login"])

    def test_pending_approval_is_preserved_without_attempting_to_approve(self):
        self.status["login"].update(enabled=False, requiresApproval=True)
        self.review()
        self.assertTrue(self.host.expected_login["requiresApproval"])

    def test_registered_helper_changes_require_separate_login_update(self):
        (self.package / EDITOR / editor_update.LOGIN_FILES[0]).write_text("new helper")
        with self.assertRaisesRegex(RuntimeError, "helper and definition"):
            self.review()

    def test_second_review_cannot_accept_external_login_state_change(self):
        self.review()
        self.status["login"].update(enabled=False, registered=False)
        with self.assertRaisesRegex(RuntimeError, "changed after"):
            self.review()

    def test_validation_observes_login_state_and_never_registers_or_unregisters(self):
        self.review()
        calls = []

        def run(*args, **kw):
            calls.append(args)
            return SimpleNamespace(
                stdout=json.dumps(
                    {
                        "version": 1,
                        "backgroundSession": 1,
                        "routingDocument": True,
                        "editorService": True,
                    }
                )
            )

        self.host.user_command = run
        with (
            patch.object(editor_update, "TARGETS", self.targets),
            patch.object(editor_update, "verify_code"),
        ):
            result = self.host.validate_apps()
        self.assertTrue(result["desktop"]["login"]["registered"])
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0][-1], "--verify-interface")
        self.assertEqual(calls[1][-1], "--runtime-capabilities")
        self.status["login"].update(enabled=False, registered=False)
        with (
            patch.object(editor_update, "TARGETS", self.targets),
            patch.object(editor_update, "verify_code"),
        ):
            with self.assertRaisesRegex(RuntimeError, "preserve login"):
                self.host.validate_apps()

    def test_invalid_login_metadata_is_not_treated_as_disabled(self):
        for status in [
            {},
            {"version": True, "login": self.status["login"]},
            {"version": 1, "login": {**self.status["login"], "enabled": 1}},
            {"version": 1, "login": {**self.status["login"], "registered": False}},
            {"version": 1, "login": {**self.status["login"], "requiresApproval": True}},
        ]:
            with self.assertRaises(RuntimeError):
                editor_update.login_state(status)


class ManagedInterfaceTests(unittest.TestCase):
    def test_wrapper_has_no_administrator_or_engine_stop_operation(self):
        events = []

        class Session:
            def __init__(self, host):
                self.ui_closed = False

            def inspect(self):
                events.append("inspect")

            def pause(self):
                events.append("close interface")
                self.ui_closed = True

            def restore(self):
                events.append("open interface")

        host = SimpleNamespace(uid=os.getuid(), existing_apps=lambda *args: None)

        def run(args):
            events.append(args)
            return SimpleNamespace(returncode=0)

        with (
            patch.object(manage_editor_update, "verify_package"),
            patch.object(manage_editor_update, "require_user_apps"),
            patch.object(manage_editor_update, "EditorUpdateHost", return_value=host),
            patch.object(manage_editor_update, "InterfaceSession", Session),
            patch.object(manage_editor_update.subprocess, "run", side_effect=run),
        ):
            self.assertEqual(
                manage_editor_update.main(
                    ["--package", "/private/tmp/package", "--manifest-sha", "a" * 64]
                ),
                0,
            )
        self.assertEqual(events[:2], ["inspect", "close interface"])
        self.assertEqual(events[-1], "open interface")
        self.assertTrue(events[2][4].endswith("/update_editor.py"))
        self.assertNotIn("sudo", " ".join(events[2]))
        self.assertIn("--reopen-interface", events[2])

    def test_malformed_manifest_digest_is_rejected_before_touching_the_session(self):
        with (
            patch.object(manage_editor_update, "EditorUpdateHost") as host,
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            with self.assertRaises(SystemExit):
                manage_editor_update.main(
                    ["--package", "/private/tmp/package", "--manifest-sha", ""]
                )
        host.assert_not_called()

    def test_real_session_pause_only_terminates_verified_interface(self):
        host = SimpleNamespace(uid=501, pw_dir="/private/tmp")
        host.user = SimpleNamespace(pw_dir="/private/tmp")
        session = manage_editor_update.InterfaceSession(host)
        session.inspect = lambda: None
        session.ui = [(123, "known interface")]
        session.process = lambda pid: "known interface"
        session.process_alive = lambda *args: False
        with (
            patch.object(manage_editor_update.os, "kill") as stop,
            patch.object(manage_editor_update, "command") as run,
        ):
            session.pause()
        stop.assert_called_once_with(123, manage_editor_update.signal.SIGTERM)
        run.assert_not_called()
        self.assertFalse(session.paused)

    def test_restoration_reopens_only_the_interface(self):
        host = SimpleNamespace(uid=501, user=SimpleNamespace(pw_dir="/private/tmp"))
        session = manage_editor_update.InterfaceSession(host)
        session.ui_closed = True
        with patch("manage_app_update.command") as run:
            session.restore()
        run.assert_called_once_with("/usr/bin/open", editor_update.TARGETS[EDITOR])
        self.assertFalse(session.paused)


if __name__ == "__main__":
    unittest.main()
