"""Full owner crash/rollback rehearsal with real private files and modeled services."""

import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from broker_host import BrokerPolicy
from driver_lifecycle import DriverLifecycle
from driver_record import DriverRecord
from driver_runtime import DriverInspector, DriverPolicy
from install_transaction import files
from suite_context import SuiteContext
from suite_files import SuiteFiles
from suite_transaction import SuiteTransaction
from suite_update import BROKER, NAMES
from uninstall_files import RemovalFiles
from uninstall_sessions import LABELS

MANIFEST = "c" * 64


def targets(root):
    return {
        name: root / ("HAL" if name == "LoopZAudio.driver" else "installed") / name
        for name in NAMES
    }


def process(pid, path):
    return {
        "pid": pid,
        "uid": os.geteuid(),
        "startSeconds": pid + 10000,
        "startMicroseconds": 1,
        "executable": str(path),
    }


class State:
    def __init__(self, root):
        self.path = root / "state.json"

    def load(self):
        return json.loads(self.path.read_text())

    def save(self, value):
        self.path.write_text(json.dumps(value))


class BrokerModel:
    def __init__(self, host):
        self.host, self.state = host, host.state
        self.policy = BrokerPolicy(
            program=targets(host.root)[BROKER],
            definition=host.root / "broker.plist",
            owner=os.geteuid(),
            domain=f"gui/{os.geteuid()}",
            label="com.lexasoft.loopz.test.suite-model",
            arguments=("--fixture",),
            mach_service="",
        )

    def inventory(self, path):
        with RemovalFiles({"node": path}) as reader:
            value = reader.inventory()["node"]
        if value is None:
            raise RuntimeError("Missing modeled broker image")
        return value

    def job(self):
        state = self.state.load()
        return (
            {
                "target": self.policy.target,
                "pid": state["brokerPid"],
                "execs": 1,
                "asid": 777,
                "ready": True,
            }
            if state["brokerLoaded"]
            else None
        )

    def definition(self):
        return self.inventory(self.policy.definition)

    def image(self, path, expected):
        node = self.inventory(path)
        if node != expected:
            raise RuntimeError("Modeled broker image changed")
        return node

    def generation(self, pid, owner=None):
        state = self.state.load()
        if not state["brokerLoaded"] or state["brokerPid"] != pid:
            raise ProcessLookupError()
        return process(pid, self.policy.program)

    def inspect(self, image=None, expected=None):
        job = self.job()
        if job is None:
            raise RuntimeError("Modeled broker has no process")
        image = image or self.policy.program
        node = self.inventory(image)
        if node != self.state.load()["brokerImage"] or expected is not None and node != expected:
            raise RuntimeError("Another broker inode is resident")
        return {
            "job": job,
            "process": self.generation(job["pid"]),
            "image": node,
            "definition": self.definition(),
        }

    def run(self, *args, **kwargs):
        state = self.state.load()
        if args == ("/bin/launchctl", "bootout", self.policy.target):
            state["brokerLoaded"] = False
        elif args == ("/bin/launchctl", "bootstrap", self.policy.domain, self.policy.definition):
            state["brokerLoaded"] = True
            state["brokerPid"] += 1
            state["brokerImage"] = self.inventory(self.policy.program)
        else:
            raise AssertionError("Only fixed modeled broker operations are allowed")
        state["operations"].append([str(arg) for arg in args])
        self.state.save(state)
        return SimpleNamespace(returncode=0, stdout="", stderr="")


class DriverModel:
    def __init__(self, host):
        self.host = host
        self.policy = DriverPolicy(
            plugin_root=host.root / "HAL",
            file_owner=os.geteuid(),
            host=host.root / "driver-host",
            host_owner=os.geteuid(),
            host_requirement='identifier "fixture"',
        )
        self.reader = DriverInspector(self.policy, verify_bundle=lambda path: None)

    def images(self, transaction):
        return self.reader.images(transaction)

    def observe(self, images):
        state = self.host.state.load()
        labels = sorted(
            label for label, image in images.items() if image["tree"] == state["driverImage"]
        )
        if not labels:
            raise RuntimeError("Unknown modeled driver inode")
        return [
            {
                "process": process(state["driverPid"], self.policy.host),
                "images": labels,
                "HALBundles": ["LoopZAudio.driver"],
            }
        ]

    def inspect(self, pid, images):
        host = self.observe(images)[0]
        if host["process"]["pid"] != pid:
            raise ProcessLookupError()
        return host

    def signal(self, target, *, force):
        state = self.host.state.load()
        if process(state["driverPid"], self.policy.host) != target or not force:
            raise RuntimeError("Wrong modeled driver signal generation")
        state["driverPid"] += 1
        state["driverImage"] = self.reader.inventory(self.policy.bundle)
        state["operations"].append(["private-driver-signal", target["pid"]])
        self.host.state.save(state)


class Guard:
    def __init__(self, host):
        self.host = host
        self.live = True

    def ready(self):
        self.check()

    def check(self):
        if not self.live:
            raise RuntimeError("Modeled guard stopped")

    def stop(self):
        self.check()
        self.live = False
        return {"passed": True, "modeled": True}

    def close(self):
        self.live = False


class Host:
    def __init__(self, root):
        self.root, self.state = root, State(root)
        self.broker = BrokerModel(self)
        self.driver = DriverModel(self)
        self.clock = 0.0

    def capture(self):
        state = self.state.load()
        return {
            "consoleUID": os.geteuid(),
            "coreAudio": state["coreAudio"],
            "defaults": state["defaults"],
            "sessions": {
                "version": 1,
                "coverage": "activeGuiSessions",
                "desktopAvailable": True,
                "accounts": [
                    {
                        "uid": os.geteuid(),
                        "asid": 777,
                        "handle": 777,
                        "jobs": dict.fromkeys(LABELS, False),
                        "login": {"registered": False, "requiresApproval": False},
                    }
                ],
            },
            "preserved": {
                "document": hashlib.sha256((self.root / "user-document").read_bytes()).hexdigest()
            },
        }

    def admit(self, before):
        if self.capture() != before:
            raise RuntimeError("Modeled protected state changed")

    def recovery_baseline(self, before):
        if before["consoleUID"] != os.geteuid():
            raise RuntimeError("Wrong recovery console")
        return self.capture()

    def start_guard(self, work, before, context):
        self.admit(before)
        return Guard(self)

    def driver_controller(self, record, admit):
        def sleep(duration):
            self.clock += duration

        return DriverLifecycle(
            self.driver,
            record,
            admit,
            lambda: None,
            signaller=self.driver.signal,
            clock=lambda: self.clock,
            sleep=sleep,
            timeout=1,
        )

    def validate(self, target, work, context):
        state = self.state.load()
        state["probe"] = True
        self.state.save(state)
        if target == "replacement" and state["failure"] == "validation":
            state["failure"] = None
            self.state.save(state)
            raise RuntimeError("Simulated public endpoint validation failure")
        return {"passed": True, "target": target, "modeled": True}

    def withdraw(self, context):
        state = self.state.load()
        state["probe"] = False
        self.state.save(state)

    def quiesce(self, context):
        self.withdraw(context)

    def cleanup_artifacts(self, artifacts):
        pass  # The model starts no real diagnostic or validation processes.


def initialize(root):
    for name in ("installed", "HAL", "support", "source", "run"):
        (root / name).mkdir(mode=0o700)
    (root / "user-document").write_text("Preserve my routing and preferences")
    (root / "broker.plist").write_text("private modeled policy")
    for name, target in targets(root).items():
        for path, value in ((target, "old"), (root / "source" / name, "new")):
            if name == BROKER:
                path.write_text(value + name)
                path.chmod(0o755)
            elif name == "LoopZAudio.driver":
                binary = path / "Contents/MacOS/LoopZAudio"
                binary.parent.mkdir(parents=True)
                binary.write_text(value + name)
                binary.chmod(0o755)
            else:
                path.mkdir()
                (path / "payload").write_text(value + name)
    host = Host(root)
    host.state.save(
        {
            "brokerLoaded": True,
            "brokerPid": 1001,
            "brokerImage": host.broker.inventory(host.broker.policy.program),
            "driverPid": 2001,
            "driverImage": host.driver.reader.inventory(host.driver.policy.bundle),
            "coreAudio": "modeled-core-audio-unchanged",
            "defaults": {
                key: "modeled-physical"
                for key in ("defaultInput", "defaultOutput", "defaultSystemOutput")
            },
            "operations": [],
            "probe": False,
            "failure": None,
        }
    )


def owner(root, checkpoint=lambda event: None):
    host = Host(root)
    file_transaction = SuiteFiles(root / "support", targets(root), checkpoint=checkpoint)
    record = DriverRecord(root / "support", host.driver.policy)
    return SuiteTransaction(file_transaction, record, host, root / "run", checkpoint=checkpoint)


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            dir="/private/tmp", prefix="loopz-suite-owner-"
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        initialize(self.root)
        self.originals = {name: files(path) for name, path in targets(self.root).items()}

    def open_owner(self, checkpoint=lambda event: None):
        transaction = owner(self.root, checkpoint)
        self.addCleanup(transaction.files.close)
        self.addCleanup(transaction.record.close)
        self.addCleanup(transaction.close)
        return transaction

    def test_complete_update_commits_all_files_and_new_service_inodes(self):
        result = self.open_owner().deploy(self.root / "source", MANIFEST)
        self.assertEqual(result["status"], "passed", result)
        self.assertEqual(
            {name: files(path) for name, path in targets(self.root).items()},
            {name: files(self.root / "source" / name) for name in NAMES},
        )
        self.assertEqual(list((self.root / "support").iterdir()), [])

    def test_endpoint_failure_restores_all_original_files_and_old_runtime(self):
        state = State(self.root).load()
        state["failure"] = "validation"
        State(self.root).save(state)
        result = self.open_owner().deploy(self.root / "source", MANIFEST)
        self.assertEqual(result["status"], "rolledBack", result)
        self.assertEqual(
            {name: files(path) for name, path in targets(self.root).items()}, self.originals
        )
        self.assertEqual(list((self.root / "support").iterdir()), [])

    def test_process_exit_at_each_service_file_and_finalization_boundary_recovers(self):
        events = [
            "journal:created",
            "workspace:recorded",
            "candidate:recorded:LoopZ.app",
            "candidate:published:LoopZ.app",
            "candidate:recorded:loopz-routing-broker",
            "candidate:published:loopz-routing-broker",
            "files:prepared",
            "runtime:prepared",
            "files:promoted",
            "broker:recorded",
            "broker:stopped",
            "broker:started",
            "driver:ready",
            "validation:ready",
            "continuity:verified",
            "files:decision",
            "driver:decision",
            "files:finalized",
            "artifacts:finalized",
            "driver:finalized",
        ]
        for event in events:
            with self.subTest(event=event):
                # Reinitialize only the fixture after each completed recovery.
                if event != events[0]:
                    self.tearDown_fixture()
                result = subprocess.run(
                    [sys.executable, "-E", "-s", "-S", __file__, "--worker", str(self.root), event],
                    capture_output=True,
                    text=True,
                    timeout=20,
                )
                self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
                recovered = self.open_owner().recover()
                self.assertIn(
                    recovered["status"],
                    (
                        "preparationRecovered",
                        "originalInstallationRecovered",
                        "finalizationRecovered",
                        "nothingToRecover",
                    ),
                )
                self.assertEqual(list((self.root / "support").iterdir()), [])
                if event in (
                    "files:decision",
                    "driver:decision",
                    "files:finalized",
                    "artifacts:finalized",
                ):
                    self.assertEqual(recovered["disposition"], "committed")
                elif event == "driver:finalized":
                    self.assertEqual(recovered["status"], "nothingToRecover")
                else:
                    self.assertEqual(
                        {name: files(path) for name, path in targets(self.root).items()},
                        self.originals,
                    )

    def crash_at(self, event):
        result = subprocess.run(
            [sys.executable, "-E", "-s", "-S", __file__, "--worker", str(self.root), event],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 77, result.stdout + result.stderr)

    def test_orphan_terminal_record_rejects_an_identical_foreign_inode_without_runtime_changes(
        self,
    ):
        self.crash_at("files:finalized")
        original = targets(self.root)[BROKER]
        retained = original.with_name("retained-original-broker")
        original.rename(retained)
        original.write_bytes(retained.read_bytes())
        original.chmod(retained.stat().st_mode & 0o777)
        before = State(self.root).load()
        with self.assertRaisesRegex(RuntimeError, "terminal installation changed"):
            self.open_owner().recover()
        self.assertEqual(State(self.root).load(), before)
        self.assertTrue((self.root / "support/.suite-driver-state.json").exists())
        original.unlink()
        retained.rename(original)
        self.assertEqual(self.open_owner().recover()["disposition"], "committed")

    def test_rollback_finalization_recovers_without_restarting_original_services_again(self):
        state = State(self.root).load()
        state["failure"] = "validation"
        State(self.root).save(state)
        self.crash_at("files:finalized")
        before = State(self.root).load()
        recovered = self.open_owner().recover()
        self.assertEqual(recovered["disposition"], "restored")
        self.assertEqual(State(self.root).load(), before)
        self.assertEqual(
            {name: files(path) for name, path in targets(self.root).items()}, self.originals
        )

    def test_tampered_context_cannot_select_an_external_diagnostic_workspace(self):
        self.crash_at("runtime:prepared")
        transaction = self.open_owner()
        transaction.files.load()
        path = self.root / "support" / transaction.files.workspace_name() / ".runtime-context.json"
        value = json.loads(path.read_text())
        value["artifacts"]["work"] = {"path": "/Applications", "device": 1, "inode": 1}
        path.write_text(json.dumps(value))
        before = State(self.root).load()
        with self.assertRaisesRegex(ValueError, "namespace"):
            transaction.recover()
        self.assertEqual(State(self.root).load(), before)

    def tearDown_fixture(self):
        import shutil

        for child in self.root.iterdir():
            shutil.rmtree(child) if child.is_dir() else child.unlink()
        initialize(self.root)
        self.originals = {name: files(path) for name, path in targets(self.root).items()}


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--worker":

        def crash(event):
            if event == sys.argv[3]:
                os._exit(77)

        owner(Path(sys.argv[2]), crash).deploy(Path(sys.argv[2]) / "source", MANIFEST)
        raise RuntimeError("Requested crash boundary was not reached")
    unittest.main()
