"""Synthetic report-validator controls; these do not measure Windows audio."""

import copy
import math
from pathlib import Path
import runpy
import unittest

audit = runpy.run_path(
    str(Path(__file__).resolve().parents[1] / "scripts" / "verify_windows_float_matrix.py")
)


def matrix():
    profiles = []
    for rate in audit["RATES"]:
        for channels in range(2, 65, 2):
            profiles.append(
                {
                    "ok": True,
                    "sampleRate": rate,
                    "channels": channels,
                    "probe": "exclusive-float32",
                    "precisionProbe": False,
                    "expectedGain": 0.5,
                    "recordingMs": 1500,
                    "diagnostics": {
                        "bitsPerSample": 32,
                        "captureDiscontinuities": 0,
                        "capturePositionGaps": 0,
                        "captureTimestampErrors": 0,
                        "captureSilentFrames": 0,
                        "longestAllChannelsZeroRun": 0,
                        "maximumSampleStep": 0.0001,
                        "maximumExpectedSampleStep": 2
                        * 0.026
                        * math.sin(math.pi * (443 + (channels - 1) * 37) / rate)
                        + 2 / 32768,
                    },
                    "signal": [
                        {"frames": rate * 3 // 2, "target": 0.025, "other": 0, "peak": 0.025}
                        for _ in range(channels)
                    ],
                    "transportTiming": {"version": 3, "bitsPerSample": 32, "deadlineMisses": 0},
                }
            )
    return {
        "mode": "full",
        "ok": True,
        "cleanup": True,
        "expectedProfiles": 192,
        "recordingMs": 1500,
        "probeMode": "--format-roundtrip-exclusive-float",
        "probeHash": "a" * 64,
        "profiles": profiles,
    }


def stress():
    report = matrix()
    profile = report["profiles"][-1]
    profile["recordingMs"] = 120000
    profile["analysisMode"] = "bounded-streaming"
    for channel in profile["signal"]:
        channel["frames"] = 192000 * 120
    profile["diagnostics"].update(
        probeDroppedFrames=0,
        probeInvalidSamples=0,
        probePendingPackets=0,
        probeAcceptedFrames=192000 * 120,
        probeAnalyzedFrames=192000 * 120,
        inputBufferFrames=1920,
        probeSampleStorageBytes=32 * 4096 * 64 * 4,
        qpcFrequency=10_000_000,
        recordingStartQpcTicks=12_000_000,
        streamEndQpcTicks=1_212_000_000,
    )
    profile["duringProbe"] = []
    for begin in range(16_000_000, 1_210_000_000, 6_000_000):
        profile["duringProbe"].append(
            {
                "beginQpcTicks": begin,
                "endQpcTicks": begin + 100_000,
                "withinRecording": True,
                "renderRate": 192000,
                "publicationRead": {"missingFrames": 0, "invalidBlocks": 0, "graphFailures": 0},
                "transportTiming": {
                    "restartAttempts": 0,
                    "deadlineMisses": 0,
                    "completedIngressZeroRuns": 0,
                    "timestampResets": 0,
                    "threadId": 12,
                    "publicationWaitEnabled": False,
                    "streamingChunksEnabled": False,
                    "publicationBatchFrames": 1920,
                    "driverQueue": {
                        "available": True,
                        "underruns": 0,
                        "leaseSilentFrames": 0,
                        "droppedFrames": 0,
                    },
                },
                "renderTiming": {"threadId": 13, "reanchors": 0, "skippedFrames": 0},
                "sceneTiming": {"enabled": False},
            }
        )
    report.update(
        mode="single",
        profiles=[profile],
        expectedProfiles=1,
        automaticSetup=True,
        recordingMs=120000,
        probeMode="--format-stress-exclusive-float",
        renderRatesBeforeFixture={"saved": 48000},
        renderRatesDuringFixture={"saved": 48000, "fixture": 192000},
        renderRateBeforeProbe=192000,
    )
    return report


class ReportAuditTests(unittest.TestCase):
    @staticmethod
    def native_state(transport, rate):
        transport.update(
            publicationFlowVersion=1,
            publicationWaitEnabled=True,
            streamingChunksEnabled=True,
            publicationBatchFrames=rate // 100,
            publicationWaits=10,
            publicationWaitTimeouts=0,
            maximumPublicationWaitNs=1_000_000,
        )

    def test_native_matrix_requires_versioned_policy(self):
        report = matrix()
        for profile in report["profiles"]:
            self.native_state(profile["transportTiming"], profile["sampleRate"])
        self.assertEqual(
            audit["verify_matrix"](report, "a" * 64, native_flow=True)["profiles"], 192
        )
        for key, value in [
            ("publicationFlowVersion", 0),
            ("publicationFlowVersion", True),
            ("publicationWaitEnabled", False),
            ("publicationWaitTimeouts", 1),
        ]:
            invalid = copy.deepcopy(report)
            invalid["profiles"][0]["transportTiming"][key] = value
            with self.assertRaises(ValueError):
                audit["verify_matrix"](invalid, "a" * 64, native_flow=True)

    def test_native_streaming_is_not_an_experimental_launch(self):
        report = stress()
        profile = report["profiles"][0]
        self.native_state(profile["transportTiming"], 192000)
        for observation in profile["duringProbe"]:
            self.native_state(observation["transportTiming"], 192000)
        result = audit["verify_streaming_recording"](
            report, "a" * 64, 192000, 64, 120000, native_flow=True
        )
        self.assertEqual(result["nativeFlowVersion"], 1)
        self.assertNotIn("diagnosticOnly", result)
        profile["duringProbe"][2]["transportTiming"].pop("publicationFlowVersion")
        with self.assertRaises(ValueError):
            audit["verify_streaming_recording"](
                report, "a" * 64, 192000, 64, 120000, native_flow=True
            )

    def test_complete_fixture(self):
        self.assertEqual(audit["verify_matrix"](matrix(), "a" * 64)["profiles"], 192)

    def test_pcm16_requires_public_width_and_private_float_transport(self):
        report = matrix()
        report["probeMode"] = "--format-roundtrip-exclusive"
        for profile in report["profiles"]:
            profile["probe"] = "exclusive-pcm16"
            profile["diagnostics"]["bitsPerSample"] = 16
        self.assertEqual(
            audit["verify_matrix"](report, "a" * 64, public_format="pcm16")["profiles"], 192
        )
        with self.assertRaises(ValueError):
            audit["verify_matrix"](report, "a" * 64)
        for path, value in [
            (("diagnostics", "bitsPerSample"), 32),
            (("transportTiming", "bitsPerSample"), 16),
            (("diagnostics", "capturePositionGaps"), 1),
        ]:
            invalid = copy.deepcopy(report)
            invalid["profiles"][0][path[0]][path[1]] = value
            with self.assertRaises(ValueError):
                audit["verify_matrix"](invalid, "a" * 64, public_format="pcm16")
        with self.assertRaises(ValueError):
            audit["verify_matrix"](matrix(), "a" * 64, public_format="pcm16")

    def test_duplicate_does_not_prove_coverage(self):
        report = matrix()
        report["profiles"][-1] = copy.deepcopy(report["profiles"][0])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            audit["verify_matrix"](report, "a" * 64)

    def test_capture_gaps_override_native_ok(self):
        for counter in ("capturePositionGaps", "captureTimestampErrors", "captureDiscontinuities"):
            with self.subTest(counter=counter):
                report = matrix()
                report["profiles"][0]["diagnostics"][counter] = 1
                with self.assertRaisesRegex(ValueError, counter):
                    audit["verify_matrix"](report, "a" * 64)

    def test_inflated_threshold_does_not_hide_step(self):
        report = matrix()
        report["profiles"][0]["diagnostics"]["maximumSampleStep"] = 0.5
        report["profiles"][0]["diagnostics"]["maximumExpectedSampleStep"] = 1
        with self.assertRaisesRegex(ValueError, "sample-step"):
            audit["verify_matrix"](report, "a" * 64)

    def test_wrong_probe_and_active_run_rejected(self):
        with self.assertRaisesRegex(ValueError, "hash"):
            audit["verify_matrix"](matrix(), "b" * 64)
        report = matrix()
        report["running"] = {"sampleRate": 192000, "channels": 64}
        with self.assertRaisesRegex(ValueError, "active"):
            audit["verify_matrix"](report, "a" * 64)

    def test_streaming_fixture(self):
        result = audit["verify_streaming_recording"](stress(), "a" * 64, 192000, 64, 120000)
        self.assertEqual(result["sampleStorageBytes"], 32 * 4096 * 64 * 4)

    def test_paired_flow_is_explicit_diagnostic_only(self):
        report = stress()
        profile = report["profiles"][0]
        for index, observation in enumerate(profile["duringProbe"]):
            observation["transportTiming"].update(
                publicationWaitEnabled=True,
                streamingChunksEnabled=True,
                publicationWaits=index + 1,
                publicationWaitTimeouts=0,
                maximumPublicationWaitNs=1_000_000,
            )
        profile["transportTiming"].update(profile["duringProbe"][-1]["transportTiming"])
        result = audit["verify_streaming_recording"](
            report, "a" * 64, 192000, 64, 120000, flow_experiment=True
        )
        self.assertTrue(result["diagnosticOnly"] and result["flowExperiment"])
        with self.assertRaises(ValueError):
            audit["verify_streaming_recording"](report, "a" * 64, 192000, 64, 120000)
        with self.assertRaises(ValueError):
            audit["verify_streaming_recording"](
                stress(), "a" * 64, 192000, 64, 120000, flow_experiment=True
            )
        for key, value in [
            ("streamingChunksEnabled", False),
            ("publicationWaitTimeouts", 1),
            ("maximumPublicationWaitNs", 30_000_001),
            ("publicationWaits", -1),
        ]:
            invalid = copy.deepcopy(report)
            invalid["profiles"][0]["duringProbe"][3]["transportTiming"][key] = value
            with self.assertRaises(ValueError):
                audit["verify_streaming_recording"](
                    invalid, "a" * 64, 192000, 64, 120000, flow_experiment=True
                )
        invalid = copy.deepcopy(report)
        invalid["profiles"][0]["transportTiming"]["publicationWaitTimeouts"] = 1
        with self.assertRaises(ValueError):
            audit["verify_streaming_recording"](
                invalid, "a" * 64, 192000, 64, 120000, flow_experiment=True
            )

    def test_streaming_overflow_and_short_recording_rejected(self):
        for field, value in (
            ("probeDroppedFrames", 1),
            ("probeInvalidSamples", 1),
            ("probePendingPackets", 1),
            ("probeAnalyzedFrames", 192000 * 119),
            ("probeSampleStorageBytes", 1 << 30),
            ("qpcFrequency", 0),
            ("streamEndQpcTicks", 13_000_000),
        ):
            with self.subTest(field=field):
                report = stress()
                report["profiles"][0]["diagnostics"][field] = value
                with self.assertRaises(ValueError):
                    audit["verify_streaming_recording"](report, "a" * 64, 192000, 64, 120000)

    def test_streaming_counter_change_and_observation_gap_rejected(self):
        for fault in (
            "counter",
            "thread",
            "gap",
            "forged-window",
            "rate",
            "driver",
            "experiment",
            "profiling",
        ):
            with self.subTest(fault=fault):
                report = stress()
                observation = report["profiles"][0]["duringProbe"][20]
                if fault == "counter":
                    observation["publicationRead"]["missingFrames"] = 1
                elif fault == "thread":
                    observation["transportTiming"]["threadId"] = 99
                elif fault == "gap":
                    del report["profiles"][0]["duringProbe"][20:30]
                elif fault == "forged-window":
                    observation["withinRecording"] = False
                elif fault == "rate":
                    observation["renderRate"] = 48000
                elif fault == "driver":
                    observation["transportTiming"]["driverQueue"]["available"] = False
                elif fault == "experiment":
                    observation["transportTiming"]["publicationWaitEnabled"] = True
                else:
                    observation["sceneTiming"]["enabled"] = True
                with self.assertRaises(ValueError):
                    audit["verify_streaming_recording"](report, "a" * 64, 192000, 64, 120000)

    def test_streaming_stress_does_not_prove_full_matrix(self):
        with self.assertRaisesRegex(ValueError, "full matrix"):
            audit["verify_matrix"](stress(), "a" * 64, 120000)


if __name__ == "__main__":
    unittest.main()
