"""The recovery controller fails closed and reaps only its own diagnostic child."""

import json
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from copy import deepcopy
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from routing_probe import (
    ContinuousProbe,
    continuous_results,
    StableReadiness,
    verify_recording_graph_snapshots,
)
from check_background_routing import Check


class ProtocolTests(unittest.TestCase):
    def recording_snapshot(self):
        return dict(
            revision=1,
            document={"devices": ["owned"]},
            runtime=dict(
                restored=True,
                renderError=0,
                controlError="",
                devices=[
                    dict(
                        id="owned",
                        transport={"generation": "7"},
                        latencyFrames=192,
                        graphFailures="0",
                        sinkFailures="0",
                    )
                ],
            ),
        )

    def test_recording_verifier_separates_reader_shutdown_from_live_timing(self):
        baseline = self.recording_snapshot()
        stopped = deepcopy(baseline)
        stopped["runtime"]["restored"] = False
        stopped["runtime"]["devices"][0]["latencyFrames"] = 1088
        verify_recording_graph_snapshots(baseline, [deepcopy(baseline)], stopped, 1)
        with self.assertRaisesRegex(RuntimeError, "document changed"):
            stopped["revision"] = 2
            verify_recording_graph_snapshots(baseline, [deepcopy(baseline)], stopped, 1)

    def test_recording_verifier_retains_strict_live_invariants(self):
        baseline = self.recording_snapshot()
        for change in ("restored", "latency", "mapping", "device", "graph", "sink"):
            live = deepcopy(baseline)
            runtime = live["runtime"]
            device = runtime["devices"][0]
            if change == "restored":
                runtime["restored"] = False
            elif change == "latency":
                device["latencyFrames"] += 1
            elif change == "mapping":
                device["transport"]["generation"] = "8"
            elif change == "device":
                device["id"] = "replacement"
            elif change == "graph":
                device["graphFailures"] = "1"
            else:
                device["sinkFailures"] = "1"
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                verify_recording_graph_snapshots(baseline, [live], baseline, 1)

    def test_readiness_starts_after_signal_arrives_without_mapping_change(self):
        barrier = StableReadiness()
        signature = [(1, 192), (2, 1088)]
        self.assertFalse(barrier.observe(False, signature, 10))
        self.assertFalse(barrier.observe(True, signature, 11))
        self.assertFalse(barrier.observe(True, signature, 12.999))
        self.assertTrue(barrier.observe(True, signature, 13))

    def test_readiness_requires_two_new_seconds_after_loss_or_timing_change(self):
        barrier = StableReadiness()
        self.assertFalse(barrier.observe(True, [(1, 192)], 10))
        self.assertFalse(barrier.observe(False, [(1, 192)], 11))
        self.assertFalse(barrier.observe(True, [(1, 192)], 12))
        self.assertFalse(barrier.observe(True, [(1, 256)], 13))
        self.assertFalse(barrier.observe(True, [(1, 256)], 14.999))
        self.assertTrue(barrier.observe(True, [(1, 256)], 15))

    def test_startup_barrier_requires_stable_public_timing(self):
        for changing in (False, True):
            with self.subTest(changing=changing):
                check = Check.__new__(Check)
                check.restored_status = lambda: {"runtime": {"restored": True}}
                values = {"time": 0.0, "period": 128}

                def clock():
                    values["time"] += 0.25
                    return values["time"]

                def public():
                    if changing:
                        values["period"] = 256 if values["period"] == 128 else 128
                    return {
                        "routingDevices": [
                            {
                                "uid": "owned",
                                "inputLatencyFrames": values["period"],
                                "inputChannels": 2,
                                "outputChannels": 2,
                                "sampleRate": 48000,
                            }
                        ]
                    }

                check.complete = public
                check.save = lambda *args: None
                probes = [SimpleNamespace(process=SimpleNamespace(poll=lambda: None))]
                with (
                    patch("check_background_routing.time.monotonic", clock),
                    patch("check_background_routing.time.sleep", lambda duration: None),
                ):
                    if changing:
                        with self.assertRaisesRegex(RuntimeError, "did not settle"):
                            check.settle_continuous_clients(probes)
                    else:
                        check.settle_continuous_clients(probes)
                        self.assertGreaterEqual(values["time"], 2)

    def test_runtime_recorder_reports_artifact_failure(self):
        check = Check.__new__(Check)
        check.folder = self.record.parent
        check.package = self.record.parent
        (check.folder / "runtime-observations.jsonl").write_text("retained evidence")
        with self.assertRaisesRegex(RuntimeError, "recorder failed"):
            with check.record_runtime(True):
                pass
        self.assertEqual(
            (check.folder / "runtime-observations.jsonl").read_text(), "retained evidence"
        )

    def test_private_probe_does_not_change_routing_engine(self):
        check = Check.__new__(Check)
        check.engine = Path("/installed/LoopZ Engine")
        check.probe_engine = Path("/private/diagnostic/LoopZ Engine")
        check.devices = [{"id": "source"}, {"id": "destination"}]
        arguments = check.probe_arguments(2, 0, 1, True)
        self.assertEqual(arguments[0], str(check.probe_engine))
        self.assertEqual(str(check.engine), "/installed/LoopZ Engine")
        self.assertEqual(arguments[-2:], ["--recovery-protocol", "stdio-v1"])
        check.coordinated_baseline = True
        self.assertEqual(
            check.probe_arguments(2, 0, 1, True)[-2:], ["--recovery-protocol", "stdio-v2"]
        )

    def test_coordinated_protocol_waits_for_baseline_barrier(self):
        source = """
import json, sys
print(json.dumps({'event':'prepared','protocol':'stdio-v2'}), flush=True)
assert input() == 'begin-baseline'
print(json.dumps({'event':'ready','protocol':'stdio-v2'}), flush=True)
assert input() == 'begin-disconnect'
print(json.dumps({'event':'disconnect-ready','baselinePassed':True}), flush=True)
assert input() == 'restored'
print(json.dumps({'event':'result','passed':True,'clientsReopened':False}), flush=True)
"""
        with self.child(source) as child:
            child.event("prepared")
            self.assertIsNone(child.process.poll())
            child.command("begin-baseline")
            child.event("ready")
            child.command("begin-disconnect")
            self.assertTrue(child.event("disconnect-ready")["baselinePassed"])
            child.command("restored")
            self.assertTrue(child.result()["passed"])

    def test_failed_baseline_retains_structured_measurement(self):
        source = """
import json
print(json.dumps({'event':'baseline-failed','measurement':{'zeroSamples':0,'maximumGain':2}}), flush=True)
"""
        with self.child(source) as child:
            with self.assertRaisesRegex(RuntimeError, "baseline-failed"):
                child.event("disconnect-ready")
        result = json.loads(self.record.read_text())
        self.assertIn('"maximumGain": 2', result["stdout"])

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.record = Path(self.temporary.name) / "probe.json"

    def tearDown(self):
        self.temporary.cleanup()

    def child(self, source):
        return ContinuousProbe([sys.executable, "-u", "-c", source], self.record)

    def test_complete_protocol_preserves_child_until_result(self):
        source = """
import json, sys
print(json.dumps({'event':'ready'}), flush=True)
assert input() == 'begin-disconnect'
print(json.dumps({'event':'disconnect-ready','baselinePassed':True}), flush=True)
assert input() == 'restored'
print(json.dumps({'event':'result','passed':True,'clientsReopened':False}), flush=True)
"""
        with self.child(source) as child:
            child.event("ready")
            child.command("begin-disconnect")
            self.assertTrue(child.event("disconnect-ready")["baselinePassed"])
            self.assertIsNone(child.process.poll())
            child.command("restored")
            self.assertTrue(child.result()["passed"])
        result = json.loads(self.record.read_text())
        self.assertTrue(result["stopped"])
        self.assertEqual(result["exitCode"], 0)
        self.assertEqual(len(result["events"]), 3)

    def test_reopened_client_cannot_pass(self):
        source = 'print(\'{"event":"result","passed":true,"clientsReopened":true}\', flush=True)'
        with self.child(source) as child:
            with self.assertRaises(RuntimeError):
                child.result()

    def test_failure_and_missing_result(self):
        for source in [
            "print('bad json', flush=True)",
            'print(\'{"event":"other"}\', flush=True)',
            "raise SystemExit(2)",
            'print(\'{"event":"result","passed":false,"clientsReopened":false}\', flush=True)',
        ]:
            with self.child(source) as child:
                with self.assertRaises(RuntimeError):
                    child.result()
            self.assertTrue(json.loads(self.record.read_text())["stopped"])

    def test_hung_child_is_reaped_on_controller_failure(self):
        with self.child("import time; time.sleep(60)") as child:
            process = child.process
            with self.assertRaises(RuntimeError):
                child.event("ready", timeout=0.05)
        self.assertIsNotNone(process.poll())
        self.assertTrue(json.loads(self.record.read_text())["stopped"])

    def test_oversized_response_is_bounded_and_reaped(self):
        with self.child("import time; print('x'*70000, flush=True); time.sleep(60)") as child:
            with self.assertRaises(RuntimeError):
                child.event("ready")
        result = json.loads(self.record.read_text())
        self.assertTrue(result["stopped"])
        self.assertLessEqual(len(result["stdout"]), 65536 + 4096)

    def test_failed_first_client_preserves_later_clients_results(self):
        records = [self.record.with_name(f"client-{index}.json") for index in range(3)]
        with ExitStack() as stack:
            probes = []
            for index, record in enumerate(records):
                source = f"""
import json, time
time.sleep({index * 0.05})
print(json.dumps({{'event':'result','passed':{index != 0},'clientsReopened':False}}), flush=True)
raise SystemExit({int(index == 0)})
"""
                probes.append(
                    stack.enter_context(
                        ContinuousProbe([sys.executable, "-u", "-c", source], record)
                    )
                )
            result = continuous_results(probes)
            self.assertFalse(result["passed"])
            self.assertEqual(len(result["failures"]), 1)
            self.assertEqual(len(result["measurements"]), 2)
            self.assertTrue(all(probe.process.poll() is not None for probe in probes))
        for record in records:
            saved = json.loads(record.read_text())
            self.assertTrue(saved["stopped"])
            self.assertEqual(saved["events"][-1]["event"], "result")


class CleanupTests(unittest.TestCase):
    def exercise(self, withdrawal_failure=False, foreign_job=False, inspection_failure=False):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            check = Check.__new__(Check)
            check.folder = check.build = folder
            check.document = folder / "routes.json"
            check.engine = folder / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine"
            check.target = "gui/501/com.lexasoft.loopz.background-check.fixture"
            check.metadata_path = folder / "session.json"
            check.metadata = {"cleanupRequired": True}
            check.before = {
                "defaultInput": "input",
                "defaultOutput": "output",
                "defaultSystemOutput": "system",
            }
            check.core_before = "same"
            loaded = True
            commands = []

            def run(arguments, **_):
                nonlocal loaded
                commands.append(arguments)
                if "write" in arguments and withdrawal_failure:
                    raise RuntimeError("Injected graph withdrawal failure")
                if "bootout" in arguments:
                    loaded = False
                return SimpleNamespace(returncode=0, check_returncode=lambda: None)

            def job():
                if inspection_failure:
                    return SimpleNamespace(returncode=1, stdout="", stderr="Access denied")
                if loaded:
                    return SimpleNamespace(
                        returncode=0,
                        stdout="foreign" if foreign_job else str(check.engine),
                        stderr="",
                    )
                return SimpleNamespace(returncode=113, stdout="", stderr="Could not find service")

            def wait_for(predicate):
                if not predicate():
                    raise RuntimeError("Expected cleanup state was not observed")

            check.job = job
            with (
                patch("check_background_routing.run", side_effect=run),
                patch(
                    "check_background_routing.catalog",
                    return_value={**check.before, "routingDevices": []},
                ),
                patch("check_background_routing.core_audio", return_value="same"),
                patch("check_background_routing.wait_for", side_effect=wait_for),
            ):
                if withdrawal_failure or foreign_job or inspection_failure:
                    with self.assertRaises(RuntimeError):
                        check.cleanup()
                else:
                    check.cleanup()
            return (
                json.loads(check.metadata_path.read_text()),
                json.loads((folder / "cleanup.json").read_text()),
                commands,
            )

    def test_success_requires_observed_job_absence_and_audio_preservation(self):
        metadata, cleanup, _ = self.exercise()
        self.assertFalse(metadata["cleanupRequired"])
        self.assertTrue(cleanup["jobRemoved"] and cleanup["withdrawn"])

    def test_withdrawal_error_cannot_skip_own_job_removal(self):
        metadata, cleanup, commands = self.exercise(withdrawal_failure=True)
        self.assertTrue(metadata["cleanupRequired"])
        self.assertTrue(cleanup["jobRemoved"])
        self.assertTrue(any("bootout" in command for command in commands))

    def test_foreign_or_uninspectable_job_is_never_stopped_or_claimed_absent(self):
        for options in [{"foreign_job": True}, {"inspection_failure": True}]:
            metadata, cleanup, commands = self.exercise(**options)
            self.assertTrue(metadata["cleanupRequired"])
            self.assertFalse(cleanup["jobRemoved"])
            self.assertFalse(any("bootout" in command for command in commands))


class RestorationStatusTests(unittest.TestCase):
    def snapshot(self):
        return {
            "document": {"version": 2, "devices": [{"id": "private-route"}]},
            "restoration": {
                "phase": "restored",
                "activeGeneration": "1",
                "desiredGeneration": "1",
            },
            "runtime": {
                "started": True,
                "restored": True,
                "rendering": True,
                "activeGeneration": "1",
                "documentError": "",
                "controlError": "",
                "renderError": 0,
            },
        }

    def observe(self, snapshot=None, code=0):
        check = Check.__new__(Check)
        check.package = Path("/private/test-package")
        check.expected_document = self.snapshot()["document"]
        check.status_observations = []
        check.save = lambda *_: None
        response = SimpleNamespace(returncode=code, stderr="", stdout=json.dumps(snapshot))
        with patch("check_background_routing.run", return_value=response):
            return check.restored_status()

    def test_full_matching_document_and_runtime_acknowledgement(self):
        snapshot = self.snapshot()
        self.assertEqual(self.observe(snapshot), snapshot)

    def test_visible_devices_or_stale_generations_cannot_substitute_for_ack(self):
        for field, value in (("restored", False), ("rendering", False), ("activeGeneration", "0")):
            snapshot = self.snapshot()
            snapshot["runtime"][field] = value
            self.assertIsNone(self.observe(snapshot))
        for field, value in (("phase", "applying"), ("desiredGeneration", "2")):
            snapshot = self.snapshot()
            snapshot["restoration"][field] = value
            self.assertIsNone(self.observe(snapshot))

    def test_foreign_document_or_invalid_seal_cannot_pass(self):
        snapshot = self.snapshot()
        snapshot["document"]["devices"] = []
        with self.assertRaises(RuntimeError):
            self.observe(snapshot)
        with self.assertRaises(RuntimeError):
            self.observe(code=3)

    def test_disconnected_or_failed_runtime_does_not_pass(self):
        self.assertIsNone(self.observe(code=1))
        for field, value in (
            ("controlError", "disconnected"),
            ("documentError", "invalid"),
            ("renderError", -1),
        ):
            snapshot = self.snapshot()
            snapshot["runtime"][field] = value
            self.assertIsNone(self.observe(snapshot))


if __name__ == "__main__":
    unittest.main()
