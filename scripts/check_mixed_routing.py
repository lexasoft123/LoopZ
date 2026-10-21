#!/usr/bin/env python3
"""Opt-in mixed-source Engine recovery with private routes and muted physical outputs."""

import argparse
import copy
import json
from pathlib import Path
import plistlib
import shutil
import time
import uuid

from check_background_routing import Check, ROOT, definition, run
from routing_probe import ContinuousProbe


def add_source(device, source_id, kind, selector, channels, mute_original=False):
    outputs = device["routing"]["outputChannels"]
    connections = [
        {"sourceChannel": index, "outputChannel": index, "gain": 1}
        for index in range(min(channels, outputs))
    ]
    if channels == 1 and outputs == 2:
        connections.append({"sourceChannel": 0, "outputChannel": 1, "gain": 1})
    device["routing"]["sources"].append(
        {
            "id": source_id,
            "channels": channels,
            "enabled": True,
            "muted": False,
            "gain": 1,
            "connections": connections,
        }
    )
    device["bindings"].append(
        {
            "id": source_id,
            "kind": kind,
            "selector": selector,
            "muteOriginal": mute_original,
        }
    )


def muted_input(channels, source_channels, selector, output, monitor_id):
    device = definition(channels)
    device["name"] = f"LoopZ Private Muted Input {channels}"
    device["routing"]["muted"] = True
    device["routing"]["sources"][0]["gain"] = 0
    add_source(device, "physical", "input-device", selector, source_channels)
    device["monitors"] = [
        {
            "id": monitor_id,
            "deviceSelector": output,
            "channels": channels,
            "enabled": True,
            "muted": True,
            "gain": 0,
            "connections": [
                {"sourceChannel": index, "outputChannel": index, "gain": 1}
                for index in range(channels)
            ],
        }
    ]
    return device


def mixed_devices(base, identity, microphone, wide_input, wide_output, monitor_output):
    devices = copy.deepcopy(base)
    for name in (
        "Marker App Output",
        "App Capture",
        "Microphone",
        "Unused Writer",
        "Wide Physical Input",
        "Shared App Capture",
    ):
        device = definition(2)
        device["name"] = "LoopZ Private " + name
        device["routing"]["sources"][0]["gain"] = 0
        devices.append(device)
    for index in (5, 9):
        add_source(devices[index], "application", "application", identity, 2, True)
    devices[6] = muted_input(2, 1, microphone, monitor_output, "microphone-monitor")
    devices[8] = muted_input(16, 16, wide_input, wide_output, "wide-monitor")
    return devices


def monitor_quality(runtime):
    monitors = {monitor["monitor"]: monitor for monitor in runtime["monitors"]}
    if len(monitors) != len(runtime["monitors"]) or set(monitors) != {
        "microphone-monitor",
        "wide-monitor",
    }:
        raise RuntimeError("A saved physical monitor is missing or duplicated")
    for monitor in monitors.values():
        audio, meter = monitor["audio"], monitor["meter"]
        if not monitor["connected"] or not monitor["complete"] or monitor["error"]:
            raise RuntimeError("A physical monitor did not reconnect")
        if not audio["sourceOnline"] or int(audio["callbacks"]) < 10:
            raise RuntimeError("A physical monitor is not consuming the restored graph")
        if int(audio["graphFailures"]) or int(audio["invalidBlocks"]):
            raise RuntimeError("A physical monitor reported an invalid render")
        if not meter["valid"] or not meter["frames"] or any(meter["peak"]) or any(meter["rms"]):
            raise RuntimeError("The physical monitor is not verifiably muted")
    return {
        "monitorMissing": {
            key: int(value["audio"]["missingFrames"]) for key, value in monitors.items()
        },
        "monitorCallbacks": {
            key: int(value["audio"]["callbacks"]) for key, value in monitors.items()
        },
    }


def require_physical_continuity(snapshot, before, expected_providers):
    """Check persistent IO owners while per-scene timing metadata is replaced."""
    if not isinstance(snapshot, dict) or not isinstance(snapshot.get("runtime"), dict):
        raise RuntimeError("The native runtime is unavailable during physical monitor continuity")
    runtime = snapshot["runtime"]
    if snapshot["revision"]["epoch"] != before["epoch"]:
        raise RuntimeError("The Engine changed during physical monitor continuity")
    if (
        not runtime["rendering"]
        or not runtime["metersFresh"]
        or runtime.get("renderError", 0)
        or runtime.get("documentError", "")
        or runtime.get("controlError", "")
    ):
        raise RuntimeError("The physical monitor graph stopped rendering fresh audio")
    providers = {(s["kind"], s["selector"]): s for s in runtime["sources"]}
    if len(providers) != len(runtime["sources"]) or set(providers) != expected_providers:
        raise RuntimeError("Physical acquisition ownership changed during client teardown")
    for key, source in providers.items():
        if key[0] != "input-device":
            continue
        if (
            not source["demanded"]
            or not source["connected"]
            or not source["complete"]
            or source["error"]
            or int(source["attempts"]) != before["providerAttempts"][str(key)]
        ):
            raise RuntimeError("A physical input stopped or reopened during client teardown")
    monitors = monitor_quality(runtime)
    if monitors["monitorMissing"] != before["monitorMissing"]:
        raise RuntimeError("Physical monitor audio lost frames during client teardown")
    if any(
        count < before["monitorCallbacks"][key]
        for key, count in monitors["monitorCallbacks"].items()
    ):
        raise RuntimeError("A physical monitor reader reset during client teardown")
    return monitors


def quality_counts(snapshot, devices, expected_providers, physical_only=False):
    """Require live acquisition and muted output before comparing steady counters."""
    runtime = snapshot["runtime"]
    if not runtime["restored"] or not runtime["rendering"] or not runtime["metersFresh"]:
        raise RuntimeError("The mixed graph has no fresh restored runtime")
    actual = {device["id"]: device for device in runtime["devices"]}
    # The source app's original output and the diagnostic-only writer have no
    # input consumer. They may idle while the actual capture routes stay live.
    demanded = (6, 8) if physical_only else (0, 1, 2, 3, 5, 6, 8, 9)
    required_devices = {devices[index]["id"] for index in demanded}
    if set(actual) != {device["id"] for device in devices}:
        raise RuntimeError("The mixed runtime did not restore every saved device")
    for device in devices:
        observed = actual[device["id"]]
        if (device["id"] in required_devices and not observed["active"]) or observed[
            "channels"
        ] != device["routing"]["outputChannels"]:
            raise RuntimeError("A mixed device is inactive or has the wrong channel count")
    providers = {(source["kind"], source["selector"]): source for source in runtime["sources"]}
    if len(providers) != len(runtime["sources"]) or set(providers) != expected_providers:
        raise RuntimeError("Shared capture ownership does not match the saved sources")
    required_providers = {
        key: value
        for key, value in providers.items()
        if not physical_only or key[0] == "input-device"
    }
    if any(
        not source["demanded"]
        or not source["connected"]
        or not source["complete"]
        or source["error"]
        for source in required_providers.values()
    ):
        raise RuntimeError("A mixed capture provider is not connected and complete")
    sources = {}
    for index, source_id in (
        (5, "application"),
        (9, "application"),
        (6, "physical"),
        (8, "physical"),
    ):
        if physical_only and index in (5, 9):
            continue
        source = next(
            item for item in actual[devices[index]["id"]]["sources"] if item["id"] == source_id
        )
        if not source["online"] or not source["meter"]["valid"]:
            raise RuntimeError("A restored capture source has no live history")
        sources[devices[index]["id"]] = int(source["missingFrames"])
    return {
        "epoch": snapshot["revision"]["epoch"],
        "sourceMissing": sources,
        **monitor_quality(runtime),
        "renderedFrames": {
            key: int(value["renderedFrames"])
            for key, value in actual.items()
            if key in required_devices
        },
        "providerAttempts": {
            str(key): int(value["attempts"]) for key, value in required_providers.items()
        },
    }


def require_steady(before, after):
    for key in ("epoch", "sourceMissing", "monitorMissing", "providerAttempts"):
        if before[key] != after[key]:
            raise RuntimeError("The healthy mixed interval changed " + key)
    for key in ("monitorCallbacks", "renderedFrames"):
        if set(before[key]) != set(after[key]) or any(
            after[key][identity] <= count for identity, count in before[key].items()
        ):
            raise RuntimeError("The healthy mixed interval stopped advancing " + key)


class MarkerFixture(ContinuousProbe):
    def __init__(self, arguments, record):
        super().__init__(arguments, record)
        self.deadline = time.monotonic() + 150

    def finish(self):
        try:
            if self.process.poll() is not None:
                raise RuntimeError("The owned marker application ended prematurely")
            self.process.stdin.write(b"quit\n")
            self.process.stdin.flush()
            result = self.event("complete", timeout=5)
            code = self.process.wait(timeout=3)
            output = result["a"]
            if (
                code
                or result["signal"] != "marker"
                or result["bRunning"]
                or not output["frames"]
                or output["timestampErrors"]
                or output["renderFailures"]
                or result["b"]["frames"]
            ):
                raise RuntimeError("The original marker application did not remain healthy")
            return result
        finally:
            self.close()


class MixedCheck(Check):
    def __init__(self, options):
        self.fixture = None
        self.quality_after = None
        # Validate external inputs and existing permission before creating an
        # owned job or a cleanup-required session record.
        engine = options.package.resolve() / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine"
        permission = run([str(engine), "--permission-status"])
        permission.check_returncode()
        if json.loads(permission.stdout)["microphone"] != "authorized":
            raise RuntimeError(
                "Existing microphone permission is required; this test never requests it"
            )
        self.inventory_before = self.inventory(engine)
        for uid, inputs, outputs in (
            (options.microphone, 1, 0),
            (options.wide_input, 16, 0),
            (options.wide_output, 0, 16),
            (options.monitor_output, 0, 2),
        ):
            device = self.inventory_before.get(uid)
            if (
                not device
                or device["inputChannels"] < inputs
                or device["outputChannels"] < outputs
                or device["transport"] in (0, 2, 3)
            ):
                raise RuntimeError("The explicit physical endpoint is unavailable: " + uid)
        if (
            options.microphone == options.wide_input
            or options.wide_output == options.monitor_output
        ):
            raise RuntimeError("Use two distinct inputs and two distinct monitor outputs")
        self.identity = "com.lexasoft.loopz.mixed-review." + uuid.uuid4().hex
        self.expected_providers = {
            ("application", self.identity),
            ("input-device", options.microphone),
            ("input-device", options.wide_input),
        }
        super().__init__(
            options.package,
            options.build,
            probe_engine=getattr(options, "probe_engine", None),
            coordinated_baseline=getattr(options, "coordinated_baseline", False),
        )
        try:
            self.devices = mixed_devices(
                self.devices,
                self.identity,
                options.microphone,
                options.wide_input,
                options.wide_output,
                options.monitor_output,
            )
            self.original = json.dumps({"version": 2, "devices": self.devices}, indent=2) + "\n"
            self.document.write_text(self.original)
            self.save("routes-tested.json", json.loads(self.original))
            self.save("physical-inventory-before.json", self.inventory_before)
            self.bundle = self.folder / "Marker.app"
            shutil.copytree(self.build / "LoopZ Test Source.app", self.bundle)
            info = self.bundle / "Contents/Info.plist"
            metadata = plistlib.loads(info.read_bytes())
            metadata["CFBundleIdentifier"] = self.identity
            info.write_bytes(plistlib.dumps(metadata))
            run(
                [
                    "/usr/bin/codesign",
                    "--force",
                    "--sign",
                    "-",
                    "--identifier",
                    self.identity,
                    str(self.bundle),
                ]
            ).check_returncode()
            run(["/usr/bin/codesign", "--verify", "--strict", str(self.bundle)]).check_returncode()
            self.metadata.update(mixedSources=True, markerIdentity=self.identity)
            self.save_metadata()
        except BaseException:
            self.cleanup()
            raise

    @staticmethod
    def inventory(engine):
        result = run([str(engine), "--list"])
        result.check_returncode()
        return {device["uid"]: device for device in json.loads(result.stdout)["devices"]}

    def prepare_clients(self):
        arguments = [
            str(self.bundle / "Contents/MacOS/LoopZ Test Source"),
            "--multi-output-source",
            "--device-a",
            "com.lexasoft.loopz.device." + self.devices[4]["id"],
            "--device-b",
            "com.lexasoft.loopz.device." + self.devices[7]["id"],
            "--signal",
            "marker",
            "--buffer-frames",
            "64",
            "--seconds",
            "150",
        ]
        self.fixture = MarkerFixture(arguments, self.folder / "marker-application.json")
        self.save(
            "marker-arguments.json", {"arguments": arguments, "pid": self.fixture.process.pid}
        )
        event = self.fixture.event("a-running-b-prepared")
        if event["signal"] != "marker" or event["bRunning"]:
            raise RuntimeError("The marker source did not start in its requested isolated mode")

    def continuous_cases(self):
        return [*super().continuous_cases(), ("application", 2, 7, 5), ("app-fanout", 2, 7, 9)]

    def observe_quality(self, physical_only=False):
        snapshot = self.restored_status()
        if snapshot is None:
            raise RuntimeError("The mixed Engine lost its restored state")
        return snapshot, quality_counts(
            snapshot, self.devices, self.expected_providers, physical_only
        )

    def steady_quality(self, phase):
        deadline = time.monotonic() + 6
        observations, last_error = [], None
        while time.monotonic() < deadline:
            try:
                first, baseline = self.observe_quality()
                time.sleep(0.5)
                last, current = self.observe_quality()
                require_steady(baseline, current)
                self.save(
                    phase + "-quality.json",
                    {
                        "passed": True,
                        "first": first,
                        "last": last,
                        "baseline": baseline,
                        "current": current,
                    },
                )
                return current
            except (RuntimeError, StopIteration) as error:
                last_error = str(error)
                observations.append({"error": last_error, "time": time.monotonic()})
                time.sleep(0.1)
        self.save(phase + "-quality-failure.json", observations)
        raise RuntimeError("The mixed graph did not produce a healthy interval: " + str(last_error))

    def before_disconnect(self):
        self.steady_quality("before")

    def after_restoration(self):
        self.quality_after = self.steady_quality("after")

    def after_audio_checks(self):
        # The diagnostic clients have now stopped. App capture may legitimately
        # become idle; the two muted physical monitors retain their input demand.
        # Public IO timing can reconcile after lease release. Monitor callbacks,
        # history coverage and acquisition owners must stay continuous through
        # that transition; scene-local render counters can start a new generation.
        started = time.monotonic()
        deadline = started + 5
        previous = self.quality_after["monitorCallbacks"].copy()
        progressed = {key: started for key in previous}
        stable_key, stable_since, baseline, observations = None, None, None, []
        while time.monotonic() < deadline:
            restored = self.restored_status()
            snapshot = self.status_observations[-1]["snapshot"]
            now = time.monotonic()
            observations.append(
                {"monotonic": now, "restored": restored is not None, "snapshot": snapshot}
            )
            self.save("client-teardown-continuity.json", observations)
            monitors = require_physical_continuity(
                snapshot, self.quality_after, self.expected_providers
            )
            for key, count in monitors["monitorCallbacks"].items():
                if count < previous[key]:
                    raise RuntimeError(
                        "A physical monitor reader reset during timing reconciliation"
                    )
                if count > previous[key]:
                    progressed[key] = now
                elif now - progressed[key] >= 0.5:
                    raise RuntimeError("A physical monitor stalled during timing reconciliation")
            previous = monitors["monitorCallbacks"].copy()
            if restored is None:
                stable_key, stable_since = None, None
            else:
                current = quality_counts(
                    snapshot, self.devices, self.expected_providers, physical_only=True
                )
                key = sorted((d["id"], d["latencyFrames"]) for d in snapshot["runtime"]["devices"])
                if key != stable_key:
                    stable_key, stable_since, baseline = key, now, current
                elif now - stable_since >= 0.5:
                    require_steady(baseline, current)
                    if any(
                        count <= self.quality_after["monitorCallbacks"][key]
                        for key, count in monitors["monitorCallbacks"].items()
                    ):
                        raise RuntimeError(
                            "A physical monitor did not advance after client teardown"
                        )
                    self.save(
                        "final-quality.json",
                        {
                            "passed": True,
                            "snapshot": snapshot,
                            "counters": current,
                            "transitionSeconds": now - started,
                            "monitorContinuityChecked": True,
                            "maximumStallSeconds": 0.5,
                            "stableFullGraphSeconds": 0.5,
                        },
                    )
                    return
            time.sleep(0.1)
        raise RuntimeError(
            "The full graph did not settle within five seconds after client teardown"
        )

    def cleanup(self):
        errors = []
        try:
            if self.fixture is not None:
                self.save("marker-final.json", self.fixture.finish())
        except BaseException as error:
            errors.append("Marker application: " + str(error))
        try:
            super().cleanup()
        except BaseException as error:
            errors.append("Private graph: " + str(error))
        try:
            after = self.inventory(self.engine)
            unchanged = all(
                after.get(uid) == profile for uid, profile in self.inventory_before.items()
            )
            self.save("physical-inventory-after.json", after)
            if not unchanged:
                raise RuntimeError("An existing physical device profile changed")
        except BaseException as error:
            errors.append("Physical profiles: " + str(error))
        self.save("mixed-cleanup.json", {"passed": not errors, "errors": errors})
        if errors:
            self.metadata["cleanupRequired"] = True
            self.save_metadata()
            raise RuntimeError("Mixed recovery cleanup failed: " + "; ".join(errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    parser.add_argument("--microphone", required=True)
    parser.add_argument("--wide-input", required=True)
    parser.add_argument("--wide-output", required=True)
    parser.add_argument("--monitor-output", required=True)
    parser.add_argument("--probe-engine", type=Path)
    parser.add_argument("--record-runtime", action="store_true")
    parser.add_argument("--coordinated-baseline", action="store_true")
    options = parser.parse_args()
    check = MixedCheck(options)
    print(check.folder, flush=True)
    try:
        with check.record_runtime(options.record_runtime):
            check.exercise(continuous=True, settled_session=True)
    except BaseException as error:
        check.record_failure(error)
        raise
    finally:
        check.cleanup()


if __name__ == "__main__":
    main()
