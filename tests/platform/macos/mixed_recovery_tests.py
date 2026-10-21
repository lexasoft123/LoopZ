"""Mixed recovery rejects missing acquisition, stale data and hidden underruns."""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from check_background_routing import definition
from check_mixed_routing import (
    mixed_devices,
    quality_counts,
    require_steady,
    require_physical_continuity,
)


class MixedRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.devices = mixed_devices(
            [definition(n) for n in (2, 8, 64, 8)],
            "app",
            "microphone",
            "wide-input",
            "wide-output",
            "speakers",
        )
        self.providers = {
            ("application", "app"),
            ("input-device", "microphone"),
            ("input-device", "wide-input"),
        }

    def snapshot(self):
        meter = {"valid": True, "frames": 64, "peak": [0, 0], "rms": [0, 0]}
        devices = []
        for index, device in enumerate(self.devices):
            sources = []
            if index in (5, 9, 6, 8):
                sources.append(
                    {
                        "id": "application" if index in (5, 9) else "physical",
                        "online": True,
                        "missingFrames": "12",
                        "meter": meter.copy(),
                    }
                )
            devices.append(
                {
                    "id": device["id"],
                    "channels": device["routing"]["outputChannels"],
                    "active": True,
                    "renderedFrames": "1024",
                    "sources": sources,
                }
            )
        monitors = []
        for name in ("microphone-monitor", "wide-monitor"):
            monitors.append(
                {
                    "monitor": name,
                    "connected": True,
                    "complete": True,
                    "error": "",
                    "meter": meter.copy(),
                    "audio": {
                        "callbacks": "100",
                        "missingFrames": "64",
                        "sourceOnline": True,
                        "graphFailures": "0",
                        "invalidBlocks": "0",
                    },
                }
            )
        sources = [
            {
                "kind": kind,
                "selector": selector,
                "demanded": True,
                "connected": True,
                "complete": True,
                "error": "",
                "attempts": "1",
            }
            for kind, selector in self.providers
        ]
        return {
            "revision": {"epoch": "same-engine"},
            "runtime": {
                "restored": True,
                "rendering": True,
                "metersFresh": True,
                "devices": devices,
                "sources": sources,
                "monitors": monitors,
            },
        }

    def count(self, snapshot, physical_only=False):
        return quality_counts(snapshot, self.devices, self.providers, physical_only)

    def test_physical_outputs_are_muted_and_shared_app_intent_survives(self):
        self.assertEqual(len(self.devices), 10)
        for index in (6, 8):
            device = self.devices[index]
            self.assertTrue(device["routing"]["muted"])
            self.assertTrue(all(m["muted"] and m["gain"] == 0 for m in device["monitors"]))
        bindings = [self.devices[index]["bindings"][-1] for index in (5, 9)]
        self.assertEqual(bindings[0], bindings[1])
        self.assertTrue(bindings[0]["muteOriginal"])

    def test_live_streams_advance_without_new_underruns(self):
        snapshot = self.snapshot()
        before = self.count(snapshot)
        for device in snapshot["runtime"]["devices"]:
            device["renderedFrames"] = "2048"
        for monitor in snapshot["runtime"]["monitors"]:
            monitor["audio"]["callbacks"] = "110"
        require_steady(before, self.count(snapshot))

    def test_connected_flags_cannot_hide_stopped_or_audible_monitor(self):
        for mutate in (
            lambda monitor: monitor["audio"].update(callbacks="0"),
            lambda monitor: monitor["audio"].update(sourceOnline=False),
            lambda monitor: monitor["audio"].update(invalidBlocks="1"),
            lambda monitor: monitor["meter"].update(valid=False),
            lambda monitor: monitor["meter"].update(peak=[0, 0.1]),
        ):
            snapshot = self.snapshot()
            mutate(snapshot["runtime"]["monitors"][0])
            with self.assertRaises(RuntimeError):
                self.count(snapshot)

    def test_one_missing_or_duplicate_capture_owner_fails(self):
        for mutate in (
            lambda runtime: runtime["sources"].pop(),
            lambda runtime: runtime["sources"].append(copy.deepcopy(runtime["sources"][0])),
            lambda runtime: runtime["sources"][0].update(complete=False),
            lambda runtime: runtime["devices"][8]["sources"][0].update(online=False),
        ):
            snapshot = self.snapshot()
            mutate(snapshot["runtime"])
            with self.assertRaises(RuntimeError):
                self.count(snapshot)

    def test_stale_counters_new_epoch_and_new_missing_frames_fail(self):
        before = self.count(self.snapshot())
        with self.assertRaises(RuntimeError):
            require_steady(before, copy.deepcopy(before))
        for mutate in (
            lambda result: result.update(epoch="replacement-engine"),
            lambda result: result["sourceMissing"].update({self.devices[8]["id"]: 13}),
            lambda result: result["monitorMissing"].update({"wide-monitor": 65}),
            lambda result: result["providerAttempts"].update({"new-acquisition": 2}),
        ):
            after = copy.deepcopy(before)
            for key in ("renderedFrames", "monitorCallbacks"):
                after[key] = {identity: value + 1 for identity, value in after[key].items()}
            mutate(after)
            with self.assertRaises(RuntimeError):
                require_steady(before, after)

    def test_final_physical_check_allows_app_demand_to_end(self):
        snapshot = self.snapshot()
        for index, device in enumerate(snapshot["runtime"]["devices"]):
            device["active"] = index in (6, 8)
        for source in snapshot["runtime"]["sources"]:
            if source["kind"] == "application":
                source.update(demanded=False, connected=False)
        self.count(snapshot, physical_only=True)
        with self.assertRaises(RuntimeError):
            self.count(snapshot)

    def test_physical_continuity_survives_only_scene_metadata_replacement(self):
        snapshot = self.snapshot()
        before = self.count(snapshot)
        snapshot["runtime"]["restored"] = False
        for device in snapshot["runtime"]["devices"]:
            device["renderedFrames"] = "0"
        for monitor in snapshot["runtime"]["monitors"]:
            monitor["audio"]["callbacks"] = "110"
        require_physical_continuity(snapshot, before, self.providers)
        with self.assertRaises(RuntimeError):
            self.count(snapshot, physical_only=True)

    def test_timing_reconciliation_cannot_hide_real_monitor_or_input_loss(self):
        before = self.count(self.snapshot())
        for mutate in (
            lambda s: s["revision"].update(epoch="another-engine"),
            lambda s: s["runtime"].update(rendering=False),
            lambda s: s["runtime"].update(metersFresh=False),
            lambda s: s["runtime"]["monitors"][0]["audio"].update(callbacks="99"),
            lambda s: s["runtime"]["monitors"][0]["audio"].update(missingFrames="65"),
            lambda s: s["runtime"]["monitors"].append(copy.deepcopy(s["runtime"]["monitors"][0])),
            lambda s: next(
                p for p in s["runtime"]["sources"] if p["kind"] == "input-device"
            ).update(attempts="2"),
            lambda s: next(
                p for p in s["runtime"]["sources"] if p["kind"] == "input-device"
            ).update(connected=False),
        ):
            snapshot = self.snapshot()
            snapshot["runtime"]["restored"] = False
            mutate(snapshot)
            with self.assertRaises(RuntimeError):
                require_physical_continuity(snapshot, before, self.providers)

    def test_writer_only_devices_need_no_input_consumer(self):
        snapshot = self.snapshot()
        for index in (4, 7):
            snapshot["runtime"]["devices"][index].update(active=False, renderedFrames="0")
        self.count(snapshot)
        snapshot["runtime"]["devices"][9]["active"] = False
        with self.assertRaises(RuntimeError):
            self.count(snapshot)


if __name__ == "__main__":
    unittest.main()
