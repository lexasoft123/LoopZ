"""Private image/file/intent tests; simulated signals never target a real PID."""

import copy
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from driver_runtime import DriverInspector, DriverPolicy, binary_node
from driver_record import DriverRecord, NAME, generation_key
from driver_lifecycle import DriverLifecycle


def process(pid, path, uid=None):
    return {
        "pid": pid,
        "uid": os.geteuid() if uid is None else uid,
        "startSeconds": pid + 100,
        "startMicroseconds": 1,
        "executable": str(path),
    }


class PrivateFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            dir="/private/tmp", prefix="loopz-driver-unit-"
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.folder = self.root / "support"
        self.folder.mkdir(mode=0o700)
        self.policy = DriverPolicy(
            plugin_root=self.root / "HAL",
            file_owner=os.geteuid(),
            host=self.root / "isolated-host",
            host_owner=os.geteuid(),
            host_requirement='identifier "private.fixture"',
        )


class InspectorTests(PrivateFixture):
    def setUp(self):
        super().setUp()
        self.policy.plugin_root.mkdir()
        self.pid = 12345
        self.kernel = process(self.pid, self.policy.host)
        self.verifier, self.dynamic = Mock(), Mock()
        self.calls = []
        self.labels = ["original"]
        self.extra = ""
        self.observer = Mock(side_effect=lambda pid, owner=None: copy.deepcopy(self.kernel))
        self.inspector = DriverInspector(
            self.policy,
            run=self.run_command,
            generation=self.observer,
            dynamic_code=self.dynamic,
            verify_bundle=self.verifier,
        )
        self.images = {}
        for label, path in (
            ("original", self.policy.bundle),
            (
                "replacement",
                self.policy.plugin_root
                / ("." + self.policy.bundle_name + ".loopz-new-" + "a" * 32),
            ),
        ):
            binary = path / "Contents/MacOS/LoopZAudio"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(label.encode())
            binary.chmod(0o755)
            self.images[label] = {"path": path, "tree": self.inspector.inventory(path)}

    def run_command(self, *args, **kwargs):
        self.calls.append(args)
        if args[0] == "/bin/ps":
            return SimpleNamespace(
                stdout=f"{self.pid} Core Audio Driver (LoopZAudio.driver)\n44 /usr/sbin/coreaudiod\n",
                stderr="",
            )
        if args[0] == "/usr/sbin/lsof":
            text = f"p{self.pid}\0"
            for label in self.labels:
                image = self.images[label]
                node = binary_node(image["tree"])
                text += f"fmem\0D0x{node['device']:x}\0i{node['inode']}\0n{image['path']}/Contents/MacOS/LoopZAudio\0"
            return SimpleNamespace(stdout=text + self.extra, stderr="")
        raise AssertionError("Only read-only host discovery and mappings are allowed")

    def test_production_policy_is_the_apple_isolated_host_and_existing_loopz_slot(self):
        policy = DriverPolicy()
        self.assertEqual(str(policy.bundle), "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver")
        self.assertEqual(policy.file_owner, 0)
        self.assertNotEqual(policy.host_owner, 0)
        self.assertEqual(policy.host.name, "com.apple.audio.Core-Audio-Driver-Service.helper")
        self.assertEqual(
            'anchor apple and (identifier "com.apple.audio.Core-Audio-Driver-Service" or '
            'identifier "com.apple.audio.Core-Audio-Driver-Service.helper")',
            policy.host_requirement,
        )

    def test_audit_binds_publisher_kernel_and_known_mapped_inode(self):
        hosts = self.inspector.observe(self.images)
        self.assertEqual(len(hosts), 1)
        self.assertEqual(hosts[0]["images"], ["original"])
        self.assertEqual(hosts[0]["process"], self.kernel)
        self.dynamic.assert_called_once_with(self.pid, self.policy.host_requirement)
        self.assertEqual(self.verifier.call_count, 2)
        self.assertEqual(hosts[0]["HALBundles"], ["LoopZAudio.driver"])

    def test_mixed_original_and_replacement_are_reported_without_certifying_either(self):
        self.labels = ["original", "replacement"]
        host = self.inspector.observe(self.images)[0]
        self.assertEqual(host["images"], ["original", "replacement"])

    def test_another_hal_component_or_root_blocks_the_entire_audit(self):
        for path in (
            self.policy.plugin_root / "Other.driver/Contents/MacOS/Other",
            Path("/System/Library/Audio/Plug-Ins/HAL/Other.driver/Contents/MacOS/Other"),
        ):
            self.extra = f"f3\0n{path}\0"
            with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, "HAL"):
                self.inspector.observe(self.images)

    def test_unknown_and_deleted_driver_executable_images_are_rejected(self):
        node = binary_node(self.images["original"]["tree"])
        path = self.policy.bundle / "Contents/MacOS/LoopZAudio"
        for suffix, inode in (("", node["inode"] + 1), (" (deleted)", node["inode"])):
            self.extra = f"fmem\0D0x{node['device']:x}\0i{inode}\0n{path}{suffix}\0"
            with self.subTest(suffix=suffix), self.assertRaises(RuntimeError):
                self.inspector.observe(self.images)

    def test_foreign_publisher_or_nonisolated_kernel_path_is_not_a_candidate(self):
        self.kernel["executable"] = "/usr/sbin/coreaudiod"
        with self.assertRaisesRegex(RuntimeError, "fixed isolated"):
            self.inspector.observe(self.images)
        self.dynamic.assert_not_called()
        self.kernel["executable"] = str(self.policy.host)
        self.dynamic.side_effect = RuntimeError("wrong running publisher")
        with self.assertRaisesRegex(RuntimeError, "wrong running publisher"):
            self.inspector.observe(self.images)

    def test_disappearing_or_changed_host_is_not_a_complete_inventory(self):
        self.inspector.candidates = Mock(side_effect=[[self.pid], [self.pid + 1]])
        with self.assertRaises(ProcessLookupError):
            self.inspector.observe(self.images)
        self.inspector.candidates = lambda: [self.pid]
        self.observer.side_effect = [self.kernel, {**self.kernel, "startMicroseconds": 2}]
        with self.assertRaisesRegex(RuntimeError, "generation changed"):
            self.inspector.observe(self.images)

    def test_driver_file_tampering_and_unrecorded_paths_fail_before_host_checks(self):
        binary = self.images["replacement"]["path"] / "Contents/MacOS/LoopZAudio"
        binary.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "recovery inventory"):
            self.inspector.observe(self.images)
        self.dynamic.assert_not_called()
        with self.assertRaises(ValueError):
            self.inspector.allowed_bundle(self.root / "foreign.driver")


class Model:
    def __init__(self, policy):
        self.policy = policy
        self.hosts = [self.host(101, ["original"])]
        self.pid = 101
        self.desired = "replacement"
        self.signals = []
        self.time = 0.0
        self.error = None
        self.no_effect = False
        self.admissions = 0

    def host(self, pid, labels):
        return {
            "process": process(pid, self.policy.host),
            "images": labels,
            "HALBundles": [self.policy.bundle_name],
        }

    def observe(self, images):
        return copy.deepcopy(self.hosts)

    def inspect(self, pid, images):
        for host in self.hosts:
            if host["process"]["pid"] == pid:
                return copy.deepcopy(host)
        raise ProcessLookupError()

    def signal(self, host, *, force):
        self.signals.append(copy.deepcopy(host))
        if not self.no_effect:
            self.pid += 1
            self.hosts = [self.host(self.pid, [self.desired])]
        if self.error:
            raise self.error

    def admit(self):
        self.admissions += 1

    def sleep(self, duration):
        self.time += duration


class LifecycleTests(PrivateFixture):
    def setUp(self):
        super().setUp()
        self.model = Model(self.policy)
        self.record = DriverRecord(self.folder, self.policy)
        self.addCleanup(self.record.close)
        self.token = "a" * 32
        self.record.begin(self.token, self.model.hosts)
        self.actor = process(701, self.root / "updater")
        self.images = {"original": {}, "replacement": {}}

    def controller(self, actor=None, **kwargs):
        return DriverLifecycle(
            self.model,
            self.record,
            self.model.admit,
            lambda: None,
            actor=actor or self.actor,
            signaller=self.model.signal,
            ended=kwargs.pop("ended", lambda actor: True),
            clock=lambda: self.model.time,
            sleep=self.model.sleep,
            timeout=1,
            **kwargs,
        )

    def test_clean_existing_original_needs_no_signal(self):
        report = self.controller().ensure_image("original", self.images)
        self.assertEqual(report["host"]["process"]["pid"], 101)
        self.assertEqual(self.model.signals, [])

    def test_forward_reload_requires_a_fresh_replacement_generation(self):
        report = self.controller().ensure_image("replacement", self.images)
        self.assertNotEqual(report["host"]["process"]["pid"], 101)
        self.assertEqual(len(self.model.signals), 1)
        entry = self.record.value["processes"][generation_key(process(101, self.policy.host))]
        self.assertEqual(entry["attempts"][0]["result"], "requested")

    def test_rollback_retires_a_host_that_previously_loaded_replacement_even_after_unmapping(self):
        forward = self.controller().ensure_image("replacement", self.images)
        self.model.hosts[0]["images"] = ["original"]
        self.model.desired = "original"
        restored = self.controller().ensure_image("original", self.images)
        self.assertNotEqual(restored["host"]["process"], forward["host"]["process"])
        self.assertEqual(len(self.model.signals), 2)

    def test_mixed_host_cannot_certify_original_runtime(self):
        self.model.hosts[0]["images"] = ["original", "replacement"]
        self.model.desired = "original"
        report = self.controller().ensure_image("original", self.images)
        self.assertEqual(len(self.model.signals), 1)
        self.assertEqual(report["host"]["images"], ["original"])

    def test_same_actor_never_blindly_repeats_a_signal_that_had_no_effect(self):
        self.model.no_effect = True
        self.model.error = PermissionError("signal denied")
        with self.assertRaisesRegex(RuntimeError, "retain recovery state"):
            self.controller().ensure_image("replacement", self.images)
        self.assertEqual(len(self.model.signals), 1)
        self.assertTrue(self.record.pending())

    def test_error_after_effect_is_verified_from_the_actual_new_generation(self):
        self.model.error = ProcessLookupError("lost signal response")
        report = self.controller().ensure_image("replacement", self.images)
        self.assertEqual(report["host"]["images"], ["replacement"])
        self.assertEqual(len(self.model.signals), 1)

    def test_a_new_actor_may_reaudit_pending_intent_only_after_old_actor_exits(self):
        self.record.intent(self.model.hosts[0], self.actor, "replacement")
        new_actor = process(702, self.root / "updater")
        with self.assertRaisesRegex(RuntimeError, "previous recovery actor is still alive"):
            self.controller(new_actor, ended=lambda actor: False).ensure_image(
                "replacement", self.images
            )
        self.assertEqual(self.model.signals, [])
        self.controller(new_actor).ensure_image("replacement", self.images)
        self.assertEqual(len(self.model.signals), 1)

    def test_process_exit_before_and_after_signal_dispatch_is_recoverable(self):
        for event in ("signal:intent", "signal:dispatched", "signal:recorded"):
            with self.subTest(event=event):
                # Reset only this test's private record; no production state.
                self.record.complete(self.token)
                self.model = Model(self.policy)
                self.record.begin(self.token, self.model.hosts)

                def die(boundary):
                    if boundary == event:
                        raise SystemExit(77)

                with self.assertRaises(SystemExit):
                    self.controller(checkpoint=die).ensure_image("replacement", self.images)
                count = len(self.model.signals)
                self.controller(process(702, self.root / "updater")).ensure_image(
                    "replacement", self.images
                )
                self.assertEqual(len(self.model.signals), 1 if count == 0 else count)

    def test_record_survives_reconstruction_and_rejects_another_transaction(self):
        self.controller().ensure_image("replacement", self.images)
        with DriverRecord(self.folder, self.policy) as reconstructed:
            reconstructed.load()
            self.assertEqual(reconstructed.value, self.record.value)
            reconstructed.bind(self.token)
            with self.assertRaisesRegex(RuntimeError, "another file transaction"):
                reconstructed.bind("b" * 32)

    def test_admission_and_unknown_actor_liveness_block_all_signals(self):
        self.model.admit = Mock(side_effect=RuntimeError("graph is active"))
        with self.assertRaisesRegex(RuntimeError, "graph is active"):
            self.controller().ensure_image("replacement", self.images)
        self.assertEqual(self.model.signals, [])

    def test_unavailable_old_actor_identity_is_not_treated_as_actor_exit(self):
        self.record.intent(self.model.hosts[0], self.actor, "replacement")
        with self.assertRaisesRegex(RuntimeError, "kernel inspection denied"):
            self.controller(
                process(702, self.root / "updater"),
                ended=Mock(side_effect=RuntimeError("kernel inspection denied")),
            ).ensure_image("replacement", self.images)
        self.assertEqual(self.model.signals, [])

    def test_duplicate_intent_by_the_same_actor_is_rejected_without_replacing_state(self):
        self.record.intent(self.model.hosts[0], self.actor, "replacement")
        before = (self.folder / NAME).read_bytes()
        with self.assertRaisesRegex(ValueError, "cannot signal.*twice"):
            self.record.intent(self.model.hosts[0], self.actor, "replacement")
        self.assertEqual((self.folder / NAME).read_bytes(), before)

    def test_record_schema_rejects_forged_owners_generations_labels_and_commands(self):
        original = copy.deepcopy(self.record.value)
        host_key = next(iter(original["processes"]))
        mutations = [
            lambda v: v.update(schema=True),
            lambda v: v.update(command=["/bin/sh"]),
            lambda v: v.update(transaction="../../foreign"),
            lambda v: v["processes"][host_key]["process"].update(uid=999),
            lambda v: v["processes"][host_key]["process"].update(pid=True),
            lambda v: v["processes"][host_key]["process"].update(executable="/usr/sbin/coreaudiod"),
            lambda v: v["processes"][host_key].update(seen=["unknown"]),
        ]
        for mutate in mutations:
            value = copy.deepcopy(original)
            mutate(value)
            with self.subTest(mutate=mutate), self.assertRaises(ValueError):
                self.record.save(value)
        self.assertEqual(self.record.value, original)
        path = self.folder / NAME
        encoded = path.read_text()
        path.write_text(encoded.replace('"schema":2', '"schema":2,"schema":2'))
        with DriverRecord(self.folder, self.policy) as record:
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                record.load()


if __name__ == "__main__":
    unittest.main()
