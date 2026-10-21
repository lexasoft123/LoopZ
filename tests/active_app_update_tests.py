"""Active application replacement; real files remain in private temporary targets."""

import copy
import json
import os
import plistlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from active_app_update import RoutingFileTransaction, deploy_active
from active_session import contract, restored, RoutingSession, session_definition
from desktop_install_tests import Host
from install_transaction import files
from update_active_apps import candidate_compatible, restore_desktop


def snapshot():
    return {
        "version": 1,
        "document": {"version": 2, "devices": [{"id": "mix", "sampleRate": 48000}]},
        "revision": {"epoch": "old-process", "generation": "7"},
        "restoration": {"phase": "restored", "activeGeneration": "7", "desiredGeneration": "7"},
        "runtime": {
            "started": True,
            "rendering": True,
            "restored": True,
            "activeGeneration": "7",
            "documentError": "",
            "controlError": "",
            "renderError": 0,
            "devices": [{"id": "mix"}],
            "permissions": {"microphone": "authorized"},
            "sources": [
                {
                    "kind": "input-device",
                    "selector": "mic",
                    "demanded": True,
                    "connected": True,
                    "complete": True,
                }
            ],
            "monitors": [
                {"device": "mix", "monitor": "speakers", "connected": True, "complete": True}
            ],
        },
        "hardware": {
            "bufferPreferences": [{"uid": "mic", "frames": 32, "applied": True}],
            "bufferSettingsError": "",
        },
    }


class ContractTests(unittest.TestCase):
    def setUp(self):
        self.before = snapshot()
        self.expected = contract(self.before, self.before["document"])
        self.after = copy.deepcopy(self.before)
        self.after["revision"] = {"epoch": "replacement", "generation": "1"}
        self.after["restoration"].update(activeGeneration="1", desiredGeneration="1")
        self.after["runtime"]["activeGeneration"] = "1"

    def test_requires_entire_document_new_epoch_and_running_components(self):
        self.assertTrue(restored(self.after, self.expected))
        for modify in (
            lambda s: s["document"]["devices"].append({"id": "extra"}),
            lambda s: s["revision"].update(epoch="old-process"),
            lambda s: s["runtime"].update(restored=False),
            lambda s: s["runtime"].update(devices=[]),
            lambda s: s["runtime"].update(renderError=1),
            lambda s: s["runtime"]["sources"][0].update(connected=False),
            lambda s: s["runtime"]["monitors"][0].update(complete=False),
            lambda s: s["hardware"]["bufferPreferences"][0].update(frames=512),
            lambda s: s["hardware"]["bufferPreferences"][0].update(applied=False),
            lambda s: s["runtime"]["permissions"].update(microphone="denied"),
            lambda s: s["restoration"].update(desiredGeneration="2"),
        ):
            with self.subTest(modify=modify):
                changed = copy.deepcopy(self.after)
                modify(changed)
                self.assertFalse(restored(changed, self.expected))

    def test_driver_receipt_refresh_preserves_intent_but_rate_change_does_not(self):
        self.expected["document"]["devices"][0]["formatRequestReceipt"] = "driver/4"
        self.after["document"]["devices"][0]["formatRequestReceipt"] = "driver/5"
        self.assertTrue(restored(self.after, self.expected))
        self.assertEqual(
            self.expected["document"]["devices"][0]["formatRequestReceipt"], "driver/4"
        )
        self.after["document"]["devices"][0]["sampleRate"] = 44100
        self.assertFalse(restored(self.after, self.expected))

    def test_preexisting_disconnected_routes_are_retained_without_faking_readiness(self):
        self.before["runtime"]["restored"] = False
        self.before["runtime"]["sources"][0].update(connected=False, complete=False)
        self.before["runtime"]["monitors"][0].update(connected=False, complete=False)
        self.before["hardware"]["bufferPreferences"][0]["applied"] = False
        expected = contract(self.before, self.before["document"])
        after = copy.deepcopy(self.before)
        after["revision"]["epoch"] = "replacement"
        self.assertTrue(restored(after, expected))
        self.assertFalse(after["runtime"]["restored"])

    def test_saved_intent_and_configuration_errors_refuse_preflight(self):
        for modify in (
            lambda s: s["runtime"].update(documentError="broken"),
            lambda s: s["hardware"].update(bufferSettingsError="corrupt"),
            lambda s: s.update(document={"version": 2, "devices": []}),
        ):
            changed = copy.deepcopy(self.before)
            modify(changed)
            with self.assertRaises(RuntimeError):
                contract(changed, self.before["document"])

    def test_candidate_must_restore_saved_buffers_and_rates(self):
        capabilities = {
            "version": 1,
            "backgroundSession": 1,
            "routingDocument": True,
            "editorService": True,
            "hardwareBufferPersistence": 1,
            "sampleRatesHz": [44100, 48000],
        }
        host = SimpleNamespace(
            session=SimpleNamespace(expected=self.expected),
            user_command=lambda *args: SimpleNamespace(stdout=json.dumps(capabilities)),
        )
        candidate_compatible(Path("/private/tmp/unused"), host)
        capabilities["hardwareBufferPersistence"] = 0
        with self.assertRaises(RuntimeError):
            candidate_compatible(Path("/private/tmp/unused"), host)
        capabilities["hardwareBufferPersistence"] = 1
        capabilities["sampleRatesHz"] = [44100]
        with self.assertRaises(RuntimeError):
            candidate_compatible(Path("/private/tmp/unused"), host)


class Session:
    def __init__(self, targets, events):
        self.targets, self.events = targets, events
        self.paused = False
        self.last_acknowledgement = None
        self.fail_restore = False

    def pause_engine(self):
        self.events.append("pause")
        assert all(
            (target / "binary").read_text().startswith("old") for target in self.targets.values()
        )
        self.paused = True

    def stop_for_rollback(self):
        self.events.append("stop candidate")
        self.paused = True

    def restore(self):
        self.events.append("restore")
        if self.fail_restore and any(
            (p / "binary").read_text().startswith("new") for p in self.targets.values()
        ):
            raise RuntimeError("candidate did not restore graph")
        self.paused = False
        self.last_acknowledgement = {
            "document": {"version": 2, "devices": [{"id": "mix"}]},
            "runtime": {"restored": True},
        }
        return self.last_acknowledgement


class ActiveHost(Host):
    def __init__(self, targets, document, events):
        super().__init__()
        self.session = Session(targets, events)
        self.document = document
        self.state["routingDevices"] = [{"uid": "saved-device", "running": True}]

    def require_idle(self, snapshot):
        pass

    def protected_state(self):
        return {**super().protected_state(), "document": self.document.read_bytes()}

    def validate_apps(self):
        if self.fail_activation:
            raise RuntimeError("signature validation failed")
        return {
            "desktop": {
                "version": 1,
                "login": {
                    "available": True,
                    "enabled": False,
                    "registered": False,
                    "requiresApproval": False,
                },
            }
        }

    def validate_activation(self, work):
        return self.session.restore()

    def cleanup_validation(self):
        pass


class ActiveTransactions(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.stage = self.root / "package"
        self.stage.mkdir(mode=0o700)
        self.targets = {
            name: self.root / "Applications" / name for name in ("LoopZ.app", "LoopZ Engine.app")
        }
        for name, p in self.targets.items():
            p.mkdir(parents=True)
            (p / "binary").write_text("old " + name)
            (self.stage / name).mkdir()
            (self.stage / name / "binary").write_text("new " + name)
        self.document = self.root / "routes.json"
        self.document.write_text('{"version":2,"devices":[{"id":"mix"}]}')
        self.original = {name: files(p) for name, p in self.targets.items()}
        self.original_document = self.document.read_bytes()
        self.events = []
        self.host = ActiveHost(self.targets, self.document, self.events)

    def deploy(self, factory=RoutingFileTransaction):
        return deploy_active(
            self.stage, self.targets, self.host, self.root, transaction_factory=factory
        )

    def test_prepares_files_while_running_and_restores_same_graph(self):
        events = self.events

        class Observed(RoutingFileTransaction):
            def prepare(inner):
                self.assertFalse(inner.session.paused)
                events.append("prepare")
                super().prepare()

        result = self.deploy(Observed)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(events, ["prepare", "pause", "restore"])
        self.assertEqual(self.document.read_bytes(), self.original_document)
        self.assertFalse(self.host.session.paused)
        self.assertEqual(
            self.host.state["routingDevices"], [{"uid": "saved-device", "running": True}]
        )

    def assert_rolled_back(self, result):
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["filesRestored"] and result["rolledBack"])
        self.assertEqual({name: files(p) for name, p in self.targets.items()}, self.original)
        self.assertFalse(self.host.session.paused)
        self.assertEqual(self.document.read_bytes(), self.original_document)

    def test_signature_failure_restores_old_apps_then_old_engine(self):
        self.host.fail_activation = True
        self.assert_rolled_back(self.deploy())
        self.assertEqual(self.events, ["pause", "stop candidate", "restore"])

    def test_candidate_graph_failure_stops_it_before_restoring_files(self):
        self.host.session.fail_restore = True
        self.assert_rolled_back(self.deploy())
        self.assertEqual(self.events, ["pause", "restore", "stop candidate", "restore"])

    def test_partial_promotion_restores_both_apps_and_engine(self):
        class Partial(RoutingFileTransaction):
            def _promote(inner, name):
                super()._promote(name)
                if len(inner.changed) == 1:
                    raise OSError("injected promotion failure")

        self.assert_rolled_back(self.deploy(Partial))

    def test_prepare_failure_never_pauses_engine(self):
        class Broken(RoutingFileTransaction):
            def prepare(inner):
                raise OSError("no storage")

        result = self.deploy(Broken)
        self.assertEqual(result["status"], "failed")
        self.assertFalse(self.events)
        self.assertEqual({name: files(p) for name, p in self.targets.items()}, self.original)

    def test_rollback_runtime_failure_reports_repair_with_original_files_present(self):
        self.host.fail_activation = True
        self.host.session.restore = lambda: (_ for _ in ()).throw(RuntimeError("old engine failed"))
        result = self.deploy()
        self.assertFalse(result["rolledBack"])
        self.assertTrue(result["repairRequired"])
        self.assertEqual({name: files(p) for name, p in self.targets.items()}, self.original)
        self.assertTrue(any("old engine failed" in error for error in result["repairRequired"]))


class DesktopCleanupTests(unittest.TestCase):
    def test_interface_reopens_on_early_failure_without_restarting_an_engine(self):
        calls = []
        session = SimpleNamespace(reopen_interface=lambda: calls.append("reopen UI"))
        with self.assertRaises(OSError):
            with restore_desktop(session):
                raise OSError("copy failed")
        self.assertEqual(calls, ["reopen UI"])


class SessionIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.host = SimpleNamespace(
            uid=os.getuid(),
            domain=f"gui/{os.getuid()}",
            user=SimpleNamespace(pw_dir=str(self.root)),
            protected_state=lambda: {},
        )
        self.session = RoutingSession(self.host)
        self.session.job.parent.mkdir(parents=True)
        self.state = snapshot()
        self.session.routes.write_text(json.dumps(self.state["document"]))
        self.definition = session_definition(self.session.routes)
        self.session.job.write_bytes(plistlib.dumps(self.definition))
        self.calls = []
        self.generation = {
            "pid": 12345,
            "uid": os.getuid(),
            "startSeconds": 100,
            "startMicroseconds": 2,
            "executable": "/Applications/LoopZ Engine.app/Contents/MacOS/LoopZ Engine",
        }

        def run(*args, **kwargs):
            self.calls.append(args)
            if args[:2] == ("/bin/launchctl", "print"):
                return SimpleNamespace(
                    returncode=0,
                    stderr="",
                    stdout=f"path = {self.session.job}\nprogram = {self.generation['executable']}\npid = 12345\n",
                )
            return SimpleNamespace(returncode=0, stdout=json.dumps(self.state))

        self.host.user_command = run

    def inspect(self):
        with (
            patch("active_session.process_generation", return_value=self.generation),
            patch("active_session.require_process_code") as code,
        ):
            result = self.session.inspect()
        self.assertIn("com.lexasoft.loopz.engine", code.call_args.args[1])
        return result

    def test_nonempty_canonical_session_inspection_is_read_only(self):
        self.inspect()
        self.assertEqual(self.session.expected["document"], self.state["document"])
        self.assertFalse(self.session.paused)
        self.assertFalse(any("bootout" in call or "bootstrap" in call for call in self.calls))

    def test_changed_document_prevents_engine_pause(self):
        self.inspect()
        changed = self.session.routes.read_bytes() + b" "
        self.session.routes.write_bytes(changed)
        with self.assertRaisesRegex(RuntimeError, "Routing changed"):
            self.session.pause_engine()
        self.assertEqual(self.session.routes.read_bytes(), changed)
        self.assertFalse(self.session.paused)
        self.assertFalse(any("bootout" in call for call in self.calls))

    def test_extra_saved_engine_arguments_are_never_executed(self):
        self.definition["ProgramArguments"] += ["--routing-document", "/private/tmp/other.json"]
        self.session.job.write_bytes(plistlib.dumps(self.definition))
        with self.assertRaisesRegex(RuntimeError, "unsupported arguments"):
            self.session.inspect()
        self.assertFalse(self.calls)

    def test_launchd_program_or_environment_overrides_are_never_executed(self):
        for key, value in (
            ("Program", "/private/tmp/not-the-engine"),
            ("EnvironmentVariables", {"DYLD_INSERT_LIBRARIES": "/private/tmp/other.dylib"}),
            ("KeepAlive", 1),
        ):
            definition = {**self.definition, key: value}
            self.session.job.write_bytes(plistlib.dumps(definition))
            with self.assertRaisesRegex(RuntimeError, "unsupported arguments"):
                self.session.read_intent()
        self.assertFalse(self.calls)

    def test_different_engine_image_cannot_be_stopped(self):
        self.generation["executable"] = "/private/tmp/not-the-engine"
        # A job pointing at another image is rejected even before checking its signature.
        with (
            patch("active_session.process_generation", return_value=self.generation),
            self.assertRaises(RuntimeError),
        ):
            self.session.inspect()
        self.assertFalse(any("bootout" in call for call in self.calls))

    def test_process_replacement_during_inspection_is_not_accepted(self):
        replacement = {**self.generation, "startMicroseconds": 3}
        with (
            patch("active_session.process_generation", side_effect=[self.generation, replacement]),
            patch("active_session.require_process_code"),
            self.assertRaisesRegex(RuntimeError, "generation changed"),
        ):
            self.session.inspect()


if __name__ == "__main__":
    unittest.main()
