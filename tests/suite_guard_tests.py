"""Private guard/workspace policy fixtures; normal unit mode opens no audio."""

import copy
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from suite_artifacts import validate_artifacts
from suite_guard import SuiteGuard
from suite_workspace import CHECKER_NAME, CHECKER_REQUIREMENT, SuiteWorkspaces


class Context:
    def __init__(self, owner):
        self.owner = owner
        self.value = {"artifacts": {"work": None, "guards": [], "probe": None}}

    def artifacts(self, value):
        validate_artifacts(value, self.owner)
        self.value["artifacts"] = copy.deepcopy(value)


class Child:
    def __init__(self, argv, context, pid):
        self.argv, self.pid, self.returncode = argv, pid, None
        self.report = Path(argv[argv.index("--report") + 1])
        self.stop = Path(argv[argv.index("--stop-file") + 1])
        self.uid = argv[argv.index("--guard") + 1]
        self.fields = {}
        self.owner = context.owner
        recorded = context.value["artifacts"]["guards"][-1]
        assert recorded["process"] is None and recorded["folder"] == self.report.parent.name
        self.write({"state": "running", "uid": self.uid, "rate": 48000})

    def write(self, value):
        self.report.write_text(json.dumps(value))
        self.report.chmod(0o600)

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        assert timeout == 5 and self.stop.is_file()
        self.returncode = 0
        self.write(
            {
                "state": "stopped",
                "uid": self.uid,
                "passed": True,
                "stoppedByRequest": True,
                "frames": 48000,
                "timestampErrors": 0,
                **self.fields,
            }
        )
        return self.returncode

    def terminate(self):
        raise AssertionError("The updater must not signal a diagnostic PID")

    kill = terminate

    def generation(self):
        if self.returncode is not None:
            raise ProcessLookupError("The modeled child ended")
        return {
            "pid": self.pid,
            "uid": self.owner,
            "startSeconds": 10000,
            "startMicroseconds": 1,
            "executable": self.argv[0],
        }


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.context = Context(os.geteuid())
        self.children = []
        self.dynamic = Mock()
        self.manager = SuiteWorkspaces(
            processes=self.processes, generation=self.generation, dynamic_code=self.dynamic
        )
        self.work = self.manager.create()
        self.addCleanup(shutil.rmtree, self.work, True)
        temporary = tempfile.TemporaryDirectory(prefix="loopz-guard-source-", dir="/private/tmp")
        self.addCleanup(temporary.cleanup)
        self.checker = Path(temporary.name) / "checker"
        self.checker.write_bytes(b"modeled-checker")
        self.checker.chmod(0o755)
        self.static = Mock()
        self.popen = patch("suite_guard.subprocess.Popen", self.spawn)
        self.popen.start()
        self.addCleanup(self.popen.stop)

    def spawn(self, argv, **kwargs):
        self.assertTrue(kwargs["start_new_session"])
        child = Child(argv, self.context, 10001 + len(self.children))
        self.children.append(child)
        return child

    def processes(self, path):
        return [
            child.pid
            for child in self.children
            if child.returncode is None and child.argv[0] == str(path)
        ]

    def generation(self, pid, owner=None):
        child = next(c for c in self.children if c.pid == pid)
        value = child.generation()
        if owner is not None and owner != value["uid"]:
            raise RuntimeError("Wrong process owner")
        return value

    def start(self, checkpoint=lambda event: None):
        return SuiteGuard(
            self.manager,
            self.context,
            self.work,
            "physical-output",
            checker=self.checker,
            static_code=self.static,
            generation=self.generation,
            dynamic_code=self.dynamic,
            checkpoint=checkpoint,
        )

    def test_startup_is_recorded_before_spawn_and_stop_is_cooperative(self):
        guard = self.start()
        guard.ready()
        self.assertEqual(self.static.call_count, 2)
        self.assertEqual(self.dynamic.call_args.args, (self.children[0].pid, CHECKER_REQUIREMENT))
        self.assertEqual(
            self.context.value["artifacts"]["guards"][0]["process"], self.children[0].generation()
        )
        self.assertTrue(guard.stop()["passed"])
        guard.close()
        self.manager.clean(self.context.value["artifacts"])
        self.assertFalse(self.work.exists())
        self.manager.clean(self.context.value["artifacts"])

    def test_recovery_finds_spawned_guard_before_pid_record_is_saved(self):
        guard = self.start()
        guard.ready()
        artifacts = copy.deepcopy(self.context.value["artifacts"])
        artifacts["guards"][0]["process"] = None

        def observed(path):
            running = self.processes(path)
            if self.children[0].stop.exists() and running:
                self.children[0].wait(5)
                return []
            return running

        recovery = SuiteWorkspaces(
            processes=observed, generation=self.generation, dynamic_code=self.dynamic
        )
        recovery.quiesce_guards(artifacts)
        self.assertEqual(self.children[0].returncode, 0)
        guard.close()
        recovery.clean(artifacts)

    def test_guard_reservation_before_directory_creation_is_recoverable(self):
        def interrupted(event):
            if event == "guard:recorded":
                raise InterruptedError("Modeled startup cancellation")

        with self.assertRaises(InterruptedError):
            self.start(interrupted)
        self.assertEqual(self.children, [])
        self.manager.quiesce_guards(self.context.value["artifacts"])
        self.manager.clean(self.context.value["artifacts"])
        self.assertFalse(self.work.exists())

    def test_invalid_signature_blocks_stop_request_and_cleanup(self):
        guard = self.start()
        self.dynamic.side_effect = RuntimeError("Wrong running-code identity")
        with self.assertRaisesRegex(RuntimeError, "Wrong running-code"):
            self.manager.quiesce_guards(self.context.value["artifacts"])
        self.assertFalse(self.children[0].stop.exists())
        with self.assertRaises(RuntimeError):
            self.manager.clean(self.context.value["artifacts"])
        self.assertTrue(self.work.exists())
        self.dynamic.side_effect = None
        guard.stop()
        guard.close()

    def test_recorded_generation_prevents_cleanup_after_path_discovery_disappears(self):
        guard = self.start()
        manager = SuiteWorkspaces(processes=lambda path: [], generation=self.generation)
        with self.assertRaisesRegex(RuntimeError, "active guard"):
            manager.clean(self.context.value["artifacts"])
        guard.stop()
        guard.close()
        manager.clean(self.context.value["artifacts"])

    def test_foreign_content_or_links_prevent_any_workspace_deletion(self):
        guard = self.start()
        guard.stop()
        guard.close()
        unexpected = self.work / "foreign-file"
        unexpected.write_text("retain")
        with self.assertRaisesRegex(RuntimeError, "Foreign suite"):
            self.manager.clean(self.context.value["artifacts"])
        self.assertTrue((guard.folder / CHECKER_NAME).is_file())
        unexpected.unlink()
        link = guard.folder / "guard.stderr"
        link.unlink()
        link.symlink_to(self.checker)
        with self.assertRaisesRegex(RuntimeError, "links"):
            self.manager.clean(self.context.value["artifacts"])
        self.assertEqual(self.checker.read_bytes(), b"modeled-checker")

    def test_foreign_workspace_inode_cannot_be_adopted(self):
        self.manager.attach(self.context, self.work)
        displaced = self.work.with_name(self.work.name + "-displaced")
        self.work.rename(displaced)
        self.addCleanup(shutil.rmtree, displaced, True)
        self.work.mkdir(mode=0o711)
        with self.assertRaisesRegex(RuntimeError, "identity"):
            self.manager.clean(self.context.value["artifacts"])
        self.assertTrue(displaced.exists() and self.work.exists())

    def test_report_identity_types_and_audio_failures_are_not_accepted(self):
        for fields in (
            {"uid": "wrong-output"},
            {"frames": True},
            {"timestampErrors": True},
            {"timestampErrors": 1},
            {"passed": False},
        ):
            guard = self.start()
            self.children[-1].fields = fields
            with self.assertRaises(RuntimeError):
                guard.stop()
            guard.close()
        self.manager.clean(self.context.value["artifacts"])

    def test_namespace_reservation_limit_and_duplicate_guards_fail_closed(self):
        for uid in ("", "com.lexasoft.loopz.device.test", "bad\0uid"):
            with self.assertRaises(ValueError):
                SuiteGuard(self.manager, self.context, self.work, uid)
        self.assertIsNone(self.context.value["artifacts"]["work"])
        guard = self.start()
        guard.stop()
        guard.close()
        artifacts = copy.deepcopy(self.context.value["artifacts"])
        artifacts["guards"].append(copy.deepcopy(artifacts["guards"][0]))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            validate_artifacts(artifacts, self.manager.owner)


if __name__ == "__main__":
    unittest.main()
