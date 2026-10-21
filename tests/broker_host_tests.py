"""Private broker file/parser/transition fixtures; no launchd or PID mutations."""

import copy
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from broker_host import BrokerInspector, BrokerPolicy, mapped_executable
from broker_lifecycle import BrokerLifecycle
from process_code import same_generation, valid_pid


def response(text="", status=0, error=""):
    return SimpleNamespace(stdout=text, stderr=error, returncode=status)


def job_text(policy, pid=12345):
    return (
        "\n".join(
            [
                policy.target + " = {",
                "\tpath = " + str(policy.definition),
                "\tprogram = " + str(policy.program),
                "\ttype = " + ("LaunchDaemon" if policy.owner == 0 else "LaunchAgent"),
                "\tstate = running",
                "\tdomain = " + (policy.domain + " [777]" if policy.owner else policy.domain),
                *(["\tasid = 777"] if policy.owner else []),
                "\targuments = {",
                *["\t\t" + a for a in policy.argv],
                "\t}",
                "\tenvironment = {",
                "\t\tOSLogRateLimit => 64",
                "\t\tXPC_SERVICE_NAME => " + policy.label,
                "\t}",
                "\tpid = " + str(pid),
                "\texecs = 1",
                *(
                    [
                        "\tendpoints = {",
                        '\t\t"' + policy.mach_service + '" = {',
                        "\t\t\tmanaged = 1",
                        "\t\t}",
                        "\t}",
                    ]
                    if policy.mach_service
                    else []
                ),
                "}",
            ]
        )
        + "\n"
    )


def mapping(pid, path, node):
    return f"p{pid}\0ftxt\0D0x{node['device']:x}\0i{node['inode']}\0n{path}\0\n"


class InspectorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            dir="/private/tmp", prefix="loopz-broker-unit-"
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.program = self.root / "loopz-routing-broker"
        self.program.write_bytes(b"private broker fixture")
        self.program.chmod(0o755)
        self.policy = BrokerPolicy(
            program=self.program,
            definition=self.root / "broker.plist",
            owner=os.geteuid(),
            domain=f"gui/{os.geteuid()}",
            label="com.lexasoft.loopz.test.broker.unit",
            arguments=("--private-fixture",),
            mach_service="",
            identifier="com.lexasoft.loopz.broker-fixture",
            requirement='identifier "com.lexasoft.loopz.broker-fixture"',
        )
        self.policy.definition.write_bytes(plistlib.dumps(self.policy.plist()))
        self.generation = {
            "pid": 12345,
            "uid": os.geteuid(),
            "startSeconds": 123,
            "startMicroseconds": 456,
            "executable": str(self.program),
        }
        self.calls = []
        self.text = job_text(self.policy)
        self.inventory_path = self.program
        self.mapped_override = None
        self.static = Mock()
        self.dynamic = Mock()
        self.observer = Mock(side_effect=lambda pid, owner=None: copy.deepcopy(self.generation))
        self.inspector = BrokerInspector(
            self.policy,
            run=self.run_command,
            generation=self.observer,
            static_code=self.static,
            dynamic_code=self.dynamic,
        )

    def run_command(self, *arguments, **kwargs):
        self.calls.append(arguments)
        if arguments[:2] == ("/bin/launchctl", "print"):
            return response(self.text)
        if arguments[0] == "/usr/sbin/lsof":
            node = self.inspector.inventory(self.inventory_path)
            return response(
                self.mapped_override
                if self.mapped_override is not None
                else mapping(12345, self.inventory_path, node)
            )
        raise AssertionError("Only read-only broker inspection is permitted")

    def test_production_policy_uses_only_fixed_root_paths_and_publisher_pins(self):
        policy = BrokerPolicy()
        self.assertEqual(policy.target, "system/com.lexasoft.loopz.routing")
        self.assertEqual(policy.owner, 0)
        self.assertEqual(
            str(policy.program), "/Library/Application Support/LoopZ/loopz-routing-broker"
        )
        self.assertIn('certificate leaf[subject.OU] = "USJ7H3X44X"', policy.requirement)
        self.assertEqual(policy.argv[1], "--engine-requirement")
        self.assertIn('identifier "com.lexasoft.loopz.engine"', policy.argv[2])
        self.assertEqual(
            policy.plist()["MachServices"], {"com.lexasoft.loopz.routing.bridge": True}
        )
        for pid in (True, 0, 1, -1, 0x80000000, "12345"):
            with self.assertRaises(ValueError):
                valid_pid(pid)

    def test_complete_audit_binds_dynamic_signature_kernel_generation_job_and_vnode(self):
        value = self.inspector.inspect()
        self.assertEqual(value["process"], self.generation)
        self.assertEqual(value["image"]["inode"], self.program.stat().st_ino)
        self.dynamic.assert_called_once_with(12345, self.policy.requirement)
        self.static.assert_called_once_with(
            self.program, self.policy.identifier, self.policy.requirement, False
        )
        self.assertEqual(self.observer.call_count, 2)
        self.assertEqual(self.inspector.inspect(expected=value["image"]), value)

    def test_resident_old_image_is_audited_after_rename_even_if_kernel_path_is_cached(self):
        before = self.inspector.inspect()
        backup = self.root / ("." + self.program.name + ".loopz-old-" + "a" * 32)
        self.program.rename(backup)
        self.program.write_bytes(b"replacement")
        self.program.chmod(0o755)
        self.inventory_path = backup
        audited = self.inspector.inspect(backup, before["image"])
        self.assertEqual(audited["image"], before["image"])
        self.assertTrue(same_generation(audited["process"], before["process"]))
        self.generation["executable"] = str(backup)
        self.assertEqual(self.inspector.inspect(backup, before["image"])["image"], before["image"])
        with self.assertRaisesRegex(RuntimeError, "recorded image"):
            self.inspector.inspect(expected=before["image"])

    def test_foreign_policy_fields_and_boolean_lookalikes_are_rejected(self):
        original = self.policy.plist()
        changes = [
            {"EnvironmentVariables": {"DYLD_INSERT_LIBRARIES": "foreign"}},
            {"Program": "/bin/true"},
            {"KeepAlive": 1},
            {"MachServices": {"foreign": True}},
            {"WorkingDirectory": "/"},
            {"ProgramArguments": [str(self.program), "--other-policy"]},
        ]
        for change in changes:
            with self.subTest(change=change):
                value = {**original, **change}
                self.policy.definition.write_bytes(plistlib.dumps(value))
                with self.assertRaisesRegex(RuntimeError, "launch policy"):
                    self.inspector.inspect()
        self.dynamic.assert_not_called()

    def test_loaded_job_substitutions_or_ambiguous_fields_are_rejected_before_code_checks(self):
        original = self.text
        changes = [
            ("\tprogram = " + str(self.program), "\tprogram = /bin/true"),
            ("\tpid = 12345", "\tpid = 12345\n\tpid = 12345"),
            ("\tpid = 12345", "\tpid = 1"),
            ("\texecs = 1", "\texecs = -1"),
            ("\tdomain = " + self.policy.domain, "\tdomain = system"),
            ("\t\tOSLogRateLimit => 64", "\t\tDYLD_INSERT_LIBRARIES => foreign"),
            ("\t\t--private-fixture", "\t\t--untrusted"),
        ]
        for old, new in changes:
            with self.subTest(change=new):
                self.text = original.replace(old, new)
                with self.assertRaises(RuntimeError):
                    self.inspector.inspect()
        self.dynamic.assert_not_called()

    def test_missing_job_is_distinguished_from_inaccessible_or_ambiguous_inventory(self):
        for status, error, absent in (
            (113, "Could not find service", True),
            (3, "Could not find service", True),
            (1, "Operation not permitted", False),
            (113, "Unknown error", False),
        ):
            self.inspector.run = Mock(return_value=response(status=status, error=error))
            if absent:
                self.assertIsNone(self.inspector.job())
            else:
                with self.assertRaises(RuntimeError):
                    self.inspector.job()

    def test_root_job_loaded_endpoint_must_match_the_fixed_mach_service(self):
        policy = BrokerPolicy()
        self.inspector.policy = policy
        text = job_text(policy)
        self.inspector.run = Mock(return_value=response(text))
        self.assertTrue(self.inspector.job()["ready"])
        for wrong in (
            text.replace(policy.mach_service, "com.lexasoft.foreign.bridge"),
            text.replace("\t\t\tmanaged = 1", "\t\t\tmanaged = 0"),
        ):
            self.inspector.run = Mock(return_value=response(wrong))
            with self.assertRaisesRegex(RuntimeError, "loaded Mach endpoint"):
                self.inspector.job()

    def test_startup_proxy_and_inconsistent_gui_session_do_not_establish_a_live_broker(self):
        self.text = self.text.replace("\tstate = running", "\tstate = xpcproxy").replace(
            "\texecs = 1", "\texecs = 0"
        )
        self.assertFalse(self.inspector.job()["ready"])
        with self.assertRaisesRegex(RuntimeError, "no live process"):
            self.inspector.inspect()
        self.dynamic.assert_not_called()
        self.text = job_text(self.policy).replace("\tasid = 777", "\tasid = 778")
        with self.assertRaisesRegex(RuntimeError, "GUI domain identity"):
            self.inspector.job()

    def test_keepalive_waiting_without_pid_or_exec_counter_remains_unready(self):
        self.text = (
            self.text.replace("\tstate = running", "\tstate = spawn scheduled")
            .replace("\tpid = 12345\n", "")
            .replace("\texecs = 1\n", "")
        )
        job = self.inspector.job()
        self.assertIsNone(job["pid"])
        self.assertEqual(job["execs"], 0)
        self.assertFalse(job["ready"])

    def test_signature_rejection_is_terminal_and_does_not_fall_back_to_path_identity(self):
        self.dynamic.side_effect = RuntimeError("wrong running publisher")
        with self.assertRaisesRegex(RuntimeError, "wrong running publisher"):
            self.inspector.inspect()
        self.assertFalse(any(call[0] == "/usr/sbin/lsof" for call in self.calls))

    def test_kernel_generation_or_job_change_during_audit_is_rejected(self):
        changed = {**self.generation, "startMicroseconds": 999}
        self.observer.side_effect = [self.generation, changed]
        with self.assertRaisesRegex(RuntimeError, "changed during inspection"):
            self.inspector.inspect()

    def test_changed_image_and_unrecorded_recovery_paths_are_rejected(self):
        node = self.inspector.inventory(self.program)
        self.program.write_bytes(b"changed in place")
        with self.assertRaisesRegex(RuntimeError, "recorded image"):
            self.inspector.inspect(expected=node)
        for path in (
            self.root / "foreign",
            self.root / ".loopz-routing-broker.loopz-old-invalid",
            self.root.parent / self.program.name,
        ):
            with self.assertRaises(ValueError):
                self.inspector.allowed_image(path)

    def test_links_and_hardlinks_are_never_signature_targets(self):
        alias = self.root / "linked"
        os.link(self.program, alias)
        with self.assertRaises(ValueError):
            self.inspector.inspect()
        alias.unlink()
        self.program.unlink()
        self.program.symlink_to(self.policy.definition)
        with self.assertRaises(ValueError):
            self.inspector.inspect()
        self.static.assert_not_called()

    def test_mapping_parser_rejects_wrong_missing_and_mixed_executable_images(self):
        node = self.inspector.inventory(self.program)
        valid = mapping(12345, self.program, node)
        bad = [
            valid.replace("p12345", "p98765"),
            valid + valid,
            valid.replace(f"i{node['inode']}", "i999999"),
            valid.replace(f"i{node['inode']}\0", ""),
            valid.replace("D0x", "Dinvalid"),
            valid.replace("ftxt", "f3"),
            valid.replace(str(self.program), str(self.root / "other")),
            valid.replace(str(self.program), str(self.program) + " (deleted)"),
            valid.replace(f"i{node['inode']}", f"i{node['inode']}\0i{node['inode']}"),
            valid
            + mapping(12345, self.program, {**node, "inode": node["inode"] + 1}).split("\0", 1)[1],
        ]
        for encoded in bad:
            with self.subTest(encoded=encoded):
                with self.assertRaises(RuntimeError):
                    mapped_executable(encoded.encode(), 12345, {str(self.program)}, node)


class RuntimeFixture:
    def __init__(self):
        self.policy = SimpleNamespace(
            target="gui/501/com.lexasoft.loopz.test.fake",
            program=Path("/private/tmp/broker-fixture"),
            domain="gui/501",
            definition=Path("/private/tmp/broker-fixture.plist"),
            owner=501,
        )
        self.definition_node = {"inode": 1}
        self.disk = {"inode": 2}
        self.resident = self.disk
        self.pid = 101
        self.processes = {}
        self.loaded = False
        self.calls = []
        self.admissions = 0
        self.time = 0.0
        self.mode = "normal"
        self.trusted = True

    def admit(self):
        self.admissions += 1

    def sleep(self, duration):
        self.time += duration

    def definition(self):
        return self.definition_node.copy()

    def image(self, path, expected):
        if expected != self.disk:
            raise RuntimeError("wrong installed image")
        return expected

    def generation(self, pid, owner=None):
        if pid not in self.processes:
            raise ProcessLookupError()
        return self.processes[pid].copy()

    def job(self):
        return (
            {"target": self.policy.target, "pid": self.pid, "execs": 1, "ready": True}
            if self.loaded
            else None
        )

    def inspect(self, image=None, expected=None):
        if not self.trusted or expected != self.resident:
            raise RuntimeError("untrusted resident image")
        return {
            "job": self.job(),
            "process": self.generation(self.pid),
            "image": copy.deepcopy(self.resident),
            "definition": self.definition(),
        }

    def start(self):
        self.loaded = True
        self.pid += 1
        self.resident = self.disk.copy()
        self.processes[self.pid] = {
            "pid": self.pid,
            "uid": 501,
            "startSeconds": self.pid,
            "startMicroseconds": 0,
            "executable": str(self.policy.program),
        }

    def run(self, *arguments, **kwargs):
        self.calls.append(arguments)
        if self.mode == "timeout-no-effect":
            raise subprocess.TimeoutExpired(arguments, 10)
        if arguments == ("/bin/launchctl", "bootout", self.policy.target):
            self.loaded = False
            self.processes.pop(self.pid)
        elif arguments == (
            "/bin/launchctl",
            "bootstrap",
            self.policy.domain,
            self.policy.definition,
        ):
            self.start()
        else:
            raise AssertionError("Unexpected service operation")
        if self.mode == "timeout-effect":
            raise subprocess.TimeoutExpired(arguments, 10)
        return response(status=5 if self.mode == "nonzero-effect" else 0)

    def controller(self):
        return BrokerLifecycle(
            self, self.admit, clock=lambda: self.time, sleep=self.sleep, timeout=1
        )


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.runtime = RuntimeFixture()
        self.controller = self.runtime.controller()

    def test_restart_audits_old_generation_and_starts_only_fixed_replacement_job(self):
        self.runtime.start()
        before = self.runtime.inspect(expected=self.runtime.resident)
        self.runtime.disk = {"inode": 3}
        report = self.controller.restart(before, Path("recorded-old-slot"), self.runtime.disk)
        self.assertNotEqual(report["start"]["audit"]["process"]["pid"], before["process"]["pid"])
        self.assertEqual(len(self.runtime.calls), 2)
        self.assertGreaterEqual(self.runtime.admissions, 4)

    def test_ensure_reuses_the_expected_running_generation_without_dispatch(self):
        first = self.controller.ensure_started(self.runtime.disk)
        second = self.runtime.controller().ensure_started(self.runtime.disk)
        self.assertEqual(first["audit"], second["audit"])
        self.assertTrue(second["reused"])
        self.assertEqual(len(self.runtime.calls), 1)

    def test_ensure_waits_for_the_existing_startup_proxy_without_another_bootstrap(self):
        self.runtime.start()
        job = self.runtime.job
        sleep = self.runtime.sleep

        def pending_job():
            value = job()
            value["ready"] = self.runtime.time > 0
            return value

        self.runtime.job = pending_job
        self.controller.sleep = sleep
        result = self.controller.ensure_started(self.runtime.disk)
        self.assertTrue(result["reused"])
        self.assertEqual(self.runtime.calls, [])
        self.assertGreater(self.runtime.time, 0)

    def test_repeated_stop_observes_the_completed_transition_without_another_command(self):
        self.runtime.start()
        audit = self.runtime.inspect(expected=self.runtime.disk)
        self.controller.stop(audit)
        self.assertTrue(self.runtime.controller().stop(audit)["alreadyAbsent"])
        self.assertEqual(len(self.runtime.calls), 1)

    def test_a_running_old_image_is_not_implicitly_stopped_by_ensure(self):
        self.runtime.start()
        self.runtime.disk = {"inode": 3}
        with self.assertRaisesRegex(RuntimeError, "untrusted resident"):
            self.controller.ensure_started(self.runtime.disk)
        self.assertEqual(self.runtime.calls, [])

    def test_admission_refusal_happens_before_any_service_mutation(self):
        self.controller.admit = Mock(side_effect=RuntimeError("graph became active"))
        with self.assertRaisesRegex(RuntimeError, "graph became active"):
            self.controller.ensure_started(self.runtime.disk)
        self.assertEqual(self.runtime.calls, [])

    def test_kernel_generation_change_immediately_before_stop_sends_no_command(self):
        self.runtime.start()
        expected = self.runtime.inspect(expected=self.runtime.disk)
        self.runtime.processes[self.runtime.pid]["startMicroseconds"] = 7
        with self.assertRaisesRegex(RuntimeError, "generation changed"):
            self.controller.stop(expected)
        self.assertEqual(self.runtime.calls, [])

    def test_missing_job_and_reused_pid_do_not_cause_an_unrelated_process_signal(self):
        self.runtime.start()
        expected = self.runtime.inspect(expected=self.runtime.disk)
        self.runtime.loaded = False
        self.runtime.processes[self.runtime.pid]["startSeconds"] += 1
        report = self.controller.stop(expected)
        self.assertTrue(report["alreadyAbsent"])
        self.assertEqual(self.runtime.calls, [])

    def test_timeout_or_nonzero_after_effect_is_observed_without_a_second_dispatch(self):
        for mode in ("timeout-effect", "nonzero-effect"):
            with self.subTest(mode=mode):
                runtime = RuntimeFixture()
                runtime.mode = mode
                controller = runtime.controller()
                started = controller.ensure_started(runtime.disk)
                self.assertTrue(started["dispatchError"])
                stopped = controller.stop(started["audit"])
                self.assertTrue(stopped["dispatchError"])
                self.assertEqual(len(runtime.calls), 2)

    def test_timeout_without_effect_is_not_treated_as_a_stopped_job_or_retried(self):
        self.runtime.start()
        expected = self.runtime.inspect(expected=self.runtime.disk)
        self.runtime.mode = "timeout-no-effect"
        with self.assertRaisesRegex(RuntimeError, "did not unload"):
            self.controller.stop(expected)
        self.assertTrue(self.runtime.loaded)
        self.assertEqual(len(self.runtime.calls), 1)

    def test_failed_start_observation_does_not_repeat_bootstrap(self):
        self.runtime.trusted = False
        for _ in range(2):
            with self.assertRaisesRegex(RuntimeError, "untrusted resident"):
                self.controller.ensure_started(self.runtime.disk)
        self.assertEqual(len(self.runtime.calls), 1)

    def test_unknown_new_generation_during_stop_is_retained_without_a_second_stop(self):
        self.runtime.start()
        expected = self.runtime.inspect(expected=self.runtime.disk)
        original = self.runtime.run

        def replace_after_stop(*args, **kwargs):
            result = original(*args, **kwargs)
            self.runtime.start()
            return result

        self.runtime.run = replace_after_stop
        with self.assertRaisesRegex(RuntimeError, "unverified broker generation"):
            self.controller.stop(expected)
        self.assertEqual(len(self.runtime.calls), 1)


if __name__ == "__main__":
    unittest.main()
