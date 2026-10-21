"""Private automatic recovery checks; no real launch jobs, audio or installed files."""

import copy
import ctypes
from functools import partial
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from app_login_session import restore_audio
from app_recovery_agent import AppRecoveryLease, agent_definition, agent_paths, publish
from app_recovery_state import AppRecoveryState, RecoveryReady, CHECKER
from app_recovery_tools import stage_tools, verify_tools
from app_recovery_workspace import AppRecoveryWorkspace
from app_recovery_verification import VerifiedAppCache
from app_update_admission import require_no_pending_app_update
from app_update_journal import AppUpdateJournal
from user_app_transaction import UserAppTransaction
from user_app_recovery_tests import (
    Host,
    Session as BaseSession,
    context,
    targets,
    verify_fixture,
)
from watch_app_update import recover_owned

SESSION = {"boot": "11111111-2222-3333-4444-555555555555", "audit": 100010}
NEXT_LOGIN = {**SESSION, "audit": SESSION["audit"] + 1}
NEXT_BOOT = {"boot": "AAAAAAAA-2222-3333-4444-555555555555", "audit": SESSION["audit"]}


class Session(BaseSession):
    def job_state(self):
        return {"loaded": True} if self.host.loaded else None


class QuietWorkspace(AppRecoveryWorkspace):
    def __init__(self, state):
        super().__init__(state, processes=lambda path: [])


class RecoveryLifetimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            dir="/private/tmp", prefix="loopz-auto-recovery-"
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.folder = self.root / "home/Library/Application Support/LoopZ/Updates"
        self.folder.mkdir(parents=True, mode=0o700)
        self.host = Host(self.root)
        self.host.loaded = False
        self.host.session = Session(self.host)
        self.state = AppRecoveryState(self.folder)
        self.addCleanup(self.state.close)
        self.state.begin("a" * 64, SESSION)
        self.lease = SimpleNamespace(
            state=self.state,
            tools="a" * 64,
            work=self.state.work,
            require_ready=lambda: None,
        )
        for path in targets(self.root).values():
            path.mkdir(parents=True)
            (path / "payload").write_text("old")
        (self.root / "routes.json").write_text(
            json.dumps({"version": 2, "devices": [{"id": "mix"}]})
        )
        self.stage = self.state.work / "package"
        self.stage.mkdir(mode=0o700)
        for name in targets(self.root):
            (self.stage / name).mkdir()
            (self.stage / name / "payload").write_text("new")
        self.journal = AppUpdateJournal(self.folder, targets(self.root))
        self.addCleanup(self.journal.close)

    def pending_update(self, *, interface=False, committed=False, login=False):
        value = context(self.root, interface=interface)
        if login:
            value["login"].update(enabled=True, registered=True)
            self.host.expected_login = copy.deepcopy(value["login"])
        transaction = UserAppTransaction(
            self.stage,
            targets(self.root),
            ["LoopZ.app"] if interface else list(targets(self.root)),
            journal=self.journal,
            context=value,
            recovery=self.lease,
            session=None if interface else self.host.session,
            verify=verify_fixture,
        )
        transaction.prepare()
        transaction.apply()
        if committed:
            self.host.session.restore()
            transaction.commit()
        transaction.close()
        self.host.events.clear()

    def recover(self, current=SESSION, **options):
        with (
            patch("watch_app_update.TARGETS", targets(self.root)),
            patch("recover_app_update.TARGETS", targets(self.root)),
        ):
            return recover_owned(
                self.state,
                self.host,
                current_session=current,
                verify=verify_fixture,
                session_factory=Session,
                workspace_factory=options.pop("workspace_factory", QuietWorkspace),
                **options,
            )

    def assert_finished(self):
        self.assertFalse(self.state.pending())
        self.assertFalse(self.journal.pending())
        self.assertFalse(self.state.work.exists())

    def test_same_login_restores_full_graph_and_original_apps(self):
        self.pending_update()
        result = self.recover()
        self.assertEqual(result["outcome"], "original-apps-restored")
        self.assertEqual(
            result["currentRoutingAcknowledgement"]["document"]["devices"], [{"id": "mix"}]
        )
        self.assertTrue(result["runtimeRestorationAllowed"])
        self.assertTrue(result["interfaceReopenRequested"])
        self.assert_finished()

    def test_later_login_with_startup_off_repairs_files_without_starting_audio(self):
        self.pending_update()
        result = self.recover(NEXT_LOGIN)
        self.assertIsNone(result["currentRoutingAcknowledgement"])
        self.assertFalse(result["runtimeRestorationAllowed"])
        self.assertFalse(result["engineSessionStopped"])
        self.assertFalse(result["interfaceReopenRequested"])
        self.assertNotIn("restore current intent", self.host.events)
        self.assertNotIn("stop candidate", self.host.events)
        self.assert_finished()

    def test_boot_policy_does_not_infer_same_login_from_reused_audit_id(self):
        self.assertFalse(restore_audio(SESSION, NEXT_BOOT, False, False))
        self.assertTrue(restore_audio(SESSION, NEXT_BOOT, True, False))
        self.assertTrue(restore_audio(SESSION, NEXT_BOOT, False, True))

    def test_later_login_honors_previously_enabled_startup(self):
        self.pending_update(login=True)
        result = self.recover(NEXT_LOGIN)
        self.assertTrue(result["runtimeRestorationAllowed"])
        self.assertIn("restore current intent", self.host.events)
        self.assertTrue(self.host.expected_login["enabled"])
        self.assert_finished()

    def test_later_login_preserves_a_session_explicitly_started_by_the_user(self):
        self.pending_update()
        self.host.loaded = True
        result = self.recover(NEXT_LOGIN)
        self.assertTrue(result["runtimeRestorationAllowed"])
        self.assertIn("restore current intent", self.host.events)
        self.assertFalse(self.host.expected_login["enabled"])
        self.assert_finished()

    def test_explicit_recovery_can_restore_audio_after_a_later_login(self):
        self.pending_update()
        result = self.recover(NEXT_LOGIN, force_runtime=True)
        self.assertTrue(result["runtimeRestorationAllowed"])
        self.assertFalse(self.host.expected_login["enabled"])
        self.assert_finished()

    def test_interface_recovery_never_stops_or_restores_engine(self):
        self.pending_update(interface=True)
        inode = targets(self.root)["LoopZ Engine.app"].stat().st_ino
        result = self.recover()
        self.assertFalse(result["engineSessionStopped"])
        self.assertIsNone(result["currentRoutingAcknowledgement"])
        self.assertEqual(targets(self.root)["LoopZ Engine.app"].stat().st_ino, inode)
        self.assertNotIn("stop candidate", self.host.events)
        self.assertNotIn("restore routing", self.host.events)
        self.assert_finished()

    def test_committed_crash_retains_validated_new_apps(self):
        self.pending_update(committed=True)
        result = self.recover()
        self.assertEqual(result["outcome"], "committed-update-retained")
        self.assertNotIn("stop candidate", self.host.events)
        self.assertEqual((targets(self.root)["LoopZ.app"] / "payload").read_text(), "new")
        self.assert_finished()

    def test_newer_saved_mix_survives_automatic_recovery(self):
        self.pending_update()
        current = {"version": 2, "devices": [{"id": "latest user edit"}]}
        self.host.document.write_text(json.dumps(current))
        result = self.recover()
        self.assertEqual(result["currentRoutingAcknowledgement"]["document"], current)
        self.assertEqual(json.loads(self.host.document.read_bytes()), current)
        self.assert_finished()

    def test_guard_shutdown_does_not_delay_restoring_the_audio_graph(self):
        self.pending_update()
        events = self.host.events

        class OrderedWorkspace(QuietWorkspace):
            def quiesce(workspace):
                events.append("quiesce guard")
                super().quiesce()

        self.recover(workspace_factory=OrderedWorkspace)
        self.assertLess(events.index("restore current intent"), events.index("quiesce guard"))
        self.assert_finished()

    def test_preparation_failure_without_app_journal_only_cleans_workspace(self):
        result = self.recover()
        self.assertFalse(result["appFilesChanged"])
        self.assertFalse(self.host.events)
        self.assertEqual((targets(self.root)["LoopZ.app"] / "payload").read_text(), "old")
        self.assert_finished()

    def test_intent_survives_journal_cleanup_and_reopens_only_in_same_login(self):
        value = context(self.root)
        self.state.record_intent(value)
        result = self.recover()
        self.assertTrue(result["interfaceReopenRequested"])
        self.assertFalse(result["appFilesChanged"])
        self.assert_finished()

    def test_foreign_workspace_content_is_preserved_and_blocks_more_updates(self):
        (self.state.work / "unrelated-document.txt").write_text("keep this")
        with self.assertRaisesRegex(RuntimeError, "Foreign"):
            self.recover()
        self.assertEqual((self.state.work / "unrelated-document.txt").read_text(), "keep this")
        with patch("app_update_admission.updates_directory", return_value=self.folder):
            with self.assertRaises(RuntimeError):
                require_no_pending_app_update(self.host.uid)

    def test_replaced_workspace_inode_is_not_removed(self):
        moved = self.state.work.with_name("retained-original")
        self.state.work.rename(moved)
        self.state.work.mkdir(mode=0o700)
        with self.assertRaisesRegex(RuntimeError, "identity"):
            self.recover()
        self.assertTrue(self.state.work.exists())
        self.assertTrue(moved.exists())

    def test_workspace_links_are_never_followed_or_removed(self):
        external = self.root / "keep"
        external.write_text("preserve")
        (self.stage / "linked").symlink_to(external)
        with self.assertRaises(RuntimeError):
            self.recover()
        self.assertEqual(external.read_text(), "preserve")
        self.assertTrue(self.state.pending())

    def test_state_rejects_arbitrary_paths_processes_and_startup_intent(self):
        for mutate in (
            lambda v: v.update(workspace=[True, 1]),
            lambda v: v.update(token="../other"),
            lambda v: v.update(command=["arbitrary", "program"]),
            lambda v: v.update(guard={"pid": 7}),
            lambda v: v.update(intent={"mode": "other"}),
            lambda v: v.update(session={"boot": SESSION["boot"], "audit": True}),
        ):
            value = copy.deepcopy(self.state.value)
            mutate(value)
            with self.assertRaises(ValueError):
                self.state.validate(value)

    def test_guard_stop_is_cooperative_and_bound_to_this_workspace(self):
        process = {
            "pid": 12345,
            "uid": os.getuid(),
            "startSeconds": 200,
            "startMicroseconds": 1,
            "executable": str(CHECKER),
        }
        self.state.guard_started(process)
        stop = self.state.work / "guard.stop"
        verified = []
        workspace = AppRecoveryWorkspace(
            self.state,
            processes=lambda path: [] if stop.exists() else [12345],
            generation=lambda *args: process,
            dynamic_code=lambda pid, requirement: verified.append(pid),
            run=lambda *args, **kw: SimpleNamespace(
                returncode=0,
                stdout=f"{CHECKER} --guard device --stop-file {stop} --seconds 120\n",
            ),
        )
        with patch("os.kill") as killed:
            workspace.quiesce()
        killed.assert_not_called()
        self.assertTrue(stop.exists())
        self.assertTrue(verified)

    def test_guard_registration_window_is_covered_by_scoped_stop_file(self):
        workspace = QuietWorkspace(self.state)
        workspace.quiesce()
        self.assertTrue((self.state.work / "guard.stop").exists())

    def test_readiness_requires_matching_live_launch_job_generation(self):
        process = {
            "pid": 12345,
            "uid": os.getuid(),
            "startSeconds": 200,
            "startMicroseconds": 1,
            "executable": "/usr/bin/python3",
        }
        with RecoveryReady(self.folder) as ready:
            ready.save(
                {
                    "version": 1,
                    "token": self.state.value["token"],
                    "tools": self.state.value["tools"],
                    "process": process,
                    "session": SESSION,
                },
                initial=True,
            )
        lease = AppRecoveryLease(self.host, self.folder)
        lease.state, lease.tools = self.state, self.state.value["tools"]
        with (
            patch("app_recovery_agent.job_state", return_value={"pid": 12345}),
            patch("app_recovery_agent.process_generation", return_value=process),
            patch("app_recovery_agent.require_process_code") as code,
            patch("app_recovery_agent.verify_tools"),
        ):
            lease.require_ready()
        code.assert_called_once_with(12345, "anchor apple")

    def test_readiness_refuses_a_reused_pid_before_any_file_promotion(self):
        process = {
            "pid": 12345,
            "uid": os.getuid(),
            "startSeconds": 200,
            "startMicroseconds": 1,
            "executable": "/usr/bin/python3",
        }
        with RecoveryReady(self.folder) as ready:
            ready.save(
                {
                    "version": 1,
                    "token": self.state.value["token"],
                    "tools": self.state.value["tools"],
                    "process": process,
                    "session": SESSION,
                },
                initial=True,
            )
        lease = AppRecoveryLease(self.host, self.folder)
        lease.state, lease.tools = self.state, self.state.value["tools"]
        with (
            patch("app_recovery_agent.job_state", return_value={"pid": 12345}),
            patch(
                "app_recovery_agent.process_generation",
                return_value={**process, "startSeconds": 201},
            ),
            self.assertRaisesRegex(RuntimeError, "restarted"),
        ):
            lease.require_ready()

    def test_engine_promotion_waits_for_its_guard_signature_acknowledgement(self):
        process = {
            "pid": 12345,
            "uid": os.getuid(),
            "startSeconds": 200,
            "startMicroseconds": 1,
            "executable": "/usr/bin/python3",
        }
        guard = {**process, "pid": 54321, "executable": str(CHECKER)}
        self.state.guard_started(guard)
        with RecoveryReady(self.folder) as ready:
            ready.save(
                {
                    "version": 1,
                    "token": self.state.value["token"],
                    "tools": self.state.value["tools"],
                    "process": process,
                    "session": SESSION,
                    "guard": None,
                },
                initial=True,
            )
        acknowledged = []

        def acknowledge(delay):
            with RecoveryReady(self.folder) as ready:
                value = ready.load()
                value["guard"] = guard
                ready.save(value)
            acknowledged.append(True)

        lease = AppRecoveryLease(self.host, self.folder)
        lease.state, lease.tools = self.state, self.state.value["tools"]
        with (
            patch("app_recovery_agent.job_state", return_value={"pid": 12345}),
            patch("app_recovery_agent.process_generation", return_value=process),
            patch("app_recovery_agent.require_process_code"),
            patch("app_recovery_agent.verify_tools"),
            patch("app_recovery_agent.time.sleep", side_effect=acknowledge),
        ):
            lease.require_ready()
        self.assertEqual(acknowledged, [True])

    def test_agent_definition_is_per_update_and_does_not_enable_normal_startup(self):
        value = agent_definition(
            self.folder, self.state.value["token"], self.state.value["tools"]
        )
        _, permit = agent_paths(self.folder, self.state.value["token"])
        self.assertEqual(value["KeepAlive"], {"PathState": {str(permit): True}})
        self.assertEqual(
            value["ProgramArguments"][:5], ["/usr/bin/python3", "-B", "-E", "-s", "-S"]
        )
        self.assertFalse(self.host.expected_login["enabled"])
        self.assertNotIn("com.lexasoft.loopz.session", value["Label"])


class FrozenToolsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            dir="/private/tmp", prefix="loopz-frozen-tools-"
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.folder = self.root / "updates"
        self.folder.mkdir(mode=0o700)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "watch_app_update.py").write_text("import dependency\n")
        (self.source / "bootstrap_app_recovery.py").write_text("# trusted bootstrap fixture\n")
        (self.source / "dependency.py").write_text("value = 1\n")
        (self.source / "unused.py").write_text("raise RuntimeError('do not copy')\n")
        (self.source / ".local").mkdir()
        (self.source / ".local/secret.json").write_text('"never copy"')

    def test_copies_only_module_closure_and_reuses_identical_snapshot(self):
        digest = stage_tools(self.folder, source=self.source)
        path = self.folder / "RecoveryTools" / digest
        self.assertEqual(
            set(item.name for item in path.iterdir()),
            {
                "watch_app_update.py",
                "bootstrap_app_recovery.py",
                "dependency.py",
                "manifest.json",
            },
        )
        self.assertEqual(stage_tools(self.folder, source=self.source), digest)
        self.assertTrue(verify_tools(path, digest, os.getuid()))

    def test_source_change_creates_a_new_toolset_without_altering_old_recovery(self):
        old = stage_tools(self.folder, source=self.source)
        (self.source / "dependency.py").write_text("value = 2\n")
        new = stage_tools(self.folder, source=self.source)
        self.assertNotEqual(old, new)
        verify_tools(self.folder / "RecoveryTools" / old, old, os.getuid())
        self.assertEqual(
            (self.folder / "RecoveryTools" / old / "dependency.py").read_text(), "value = 1\n"
        )

    def test_tampered_cached_code_is_rejected_instead_of_overwritten(self):
        digest = stage_tools(self.folder, source=self.source)
        path = self.folder / "RecoveryTools" / digest / "dependency.py"
        path.write_text("unrecorded code\n")
        with self.assertRaises(ValueError):
            stage_tools(self.folder, source=self.source)
        self.assertEqual(path.read_text(), "unrecorded code\n")

    def test_linked_source_module_is_not_followed(self):
        path = self.source / "dependency.py"
        path.unlink()
        path.symlink_to(self.source / "unused.py")
        with self.assertRaises(ValueError):
            stage_tools(self.folder, source=self.source)

    def test_manifest_symlink_is_rejected_before_reading_external_data(self):
        digest = stage_tools(self.folder, source=self.source)
        path = self.folder / "RecoveryTools" / digest / "manifest.json"
        path.unlink()
        path.symlink_to(self.source / ".local/secret.json")
        with self.assertRaises(ValueError):
            verify_tools(path.parent, digest, os.getuid())


class VerificationCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            dir="/private/tmp", prefix="loopz-code-seal-"
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.app = self.root / "application"
        self.app.mkdir()
        self.payload = self.app / "payload"
        self.payload.write_text("original signed fixture")
        self.calls = []
        self.cache = VerifiedAppCache(lambda role, path: self.calls.append((role, path)))

    def test_same_signed_inode_and_contents_can_move_without_reverification(self):
        self.cache("LoopZ.app", self.app)
        renamed = self.root / "restored"
        self.app.rename(renamed)
        self.cache("LoopZ.app", renamed)
        self.assertEqual(len(self.calls), 1)

    def test_changed_code_requires_a_new_signature_check(self):
        self.cache("LoopZ.app", self.app)
        self.payload.write_text("different code")
        self.cache("LoopZ.app", self.app)
        self.assertEqual(len(self.calls), 2)

    def test_changed_permissions_require_a_new_signature_check(self):
        self.cache("LoopZ.app", self.app)
        self.payload.chmod(0o600)
        self.cache("LoopZ.app", self.app)
        self.assertEqual(len(self.calls), 2)

    def test_changed_extended_metadata_requires_a_new_signature_check(self):
        self.cache("LoopZ.app", self.app)
        library = ctypes.CDLL(None, use_errno=True)
        library.setxattr.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_uint32,
            ctypes.c_int,
        ]
        library.setxattr.restype = ctypes.c_int
        status = library.setxattr(
            os.fsencode(self.payload),
            b"com.lexasoft.loopz.test",
            b"changed",
            7,
            0,
            1,
        )
        self.assertEqual(status, 0)
        self.cache("LoopZ.app", self.app)
        self.assertEqual(len(self.calls), 2)

    def test_same_files_under_another_role_cannot_borrow_the_signature_check(self):
        self.cache("LoopZ.app", self.app)
        self.cache("LoopZ Engine.app", self.app)
        self.assertEqual(len(self.calls), 2)

    def test_mutation_during_signature_verification_is_rejected(self):
        def changed(role, path):
            self.payload.write_text("changed during verification")

        cache = VerifiedAppCache(changed)
        with self.assertRaisesRegex(RuntimeError, "changed during verification"):
            cache("LoopZ.app", self.app)
        self.assertFalse(cache.verified)


if __name__ == "__main__":
    unittest.main()
