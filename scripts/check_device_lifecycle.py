#!/usr/bin/env python3
"""Opt-in private device edits while one unrelated virtual client keeps streaming."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import time

from check_background_routing import Check, PIN, ROOT, definition, run, wait_for


def same_intent(first, second):
    """Rate-request acknowledgements may change without changing saved audio intent."""
    def intent(document):
        value = copy.deepcopy(document)
        for device in value["devices"]:
            device.pop("formatRequestReceipt", None)
        return value
    return intent(first) == intent(second)


def public_endpoints(document):
    """Disabled devices withdraw; pass-through enablement governs output visibility."""
    result = {}
    for device in document["devices"]:
        routing = device["routing"]
        if not routing["enabled"]:
            continue
        passthrough = {
            binding["id"] for binding in device["bindings"]
            if binding["kind"] == "pass-through"
        }
        output = any(
            source["id"] in passthrough and source["enabled"]
            for source in routing["sources"]
        )
        channels = routing["outputChannels"]
        result["com.lexasoft.loopz.device." + device["id"]] = {
            "name": device["name"], "inputs": channels,
            "outputs": channels if output else 0, "rate": device["sampleRate"],
        }
    return result


class LifecycleCheck(Check):
    def __init__(self, package, build):
        engine = package.resolve() / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine"
        run(["/usr/bin/codesign", "--verify", "--strict", "-R=" + PIN, str(engine)]).check_returncode()
        permission = json.loads(run([str(engine), "--permission-status"]).stdout)
        if permission.get("microphone") != "authorized":
            raise RuntimeError("This virtual-only check requires existing access; it never requests it")
        self.client = None
        self.client_files = []
        self.epoch = None
        self.phase = "initial"
        self.lifecycle = []
        super().__init__(package, build)
        self.devices = [definition(2), definition(2)]
        for index, device in enumerate(self.devices):
            device["name"] = f"LoopZ Lifecycle Review {index} {device['id']}"
        self.stable_uid = "com.lexasoft.loopz.device." + self.devices[0]["id"]
        self.edited_uid = "com.lexasoft.loopz.device." + self.devices[1]["id"]
        self.known_uids = [self.stable_uid, self.edited_uid]
        self.document_value = {"version": 2, "devices": self.devices}
        self.original = json.dumps(self.document_value, indent=2) + "\n"
        self.document.write_text(self.original)
        self.save("routes-tested.json", self.document_value)
        self.expected_document = self.canonical()

    def canonical(self):
        response = run([str(self.build / "loopz-config"), "read", str(self.document)])
        response.check_returncode()
        return json.loads(response.stdout)

    def settled(self):
        response = run([str(self.package / "LoopZ.app/Contents/MacOS/LoopZ"), "--routing-status"])
        if response.returncode == 3:
            raise RuntimeError("The signed desktop interface seal failed")
        if response.returncode:
            return None
        value = json.loads(response.stdout)
        self.lifecycle.append({"phase": self.phase, "snapshot": value})
        self.save("lifecycle-status.json", self.lifecycle)
        if value["document"] != self.expected_document:
            persisted = self.canonical()
            if not same_intent(persisted, self.expected_document):
                raise RuntimeError("The private document changed outside the submitted edit")
            if value["document"] != persisted:
                return None
            self.expected_document = persisted
        runtime, restoration = value["runtime"], value["restoration"]
        if (
            restoration["phase"] != "restored"
            or restoration["activeGeneration"] != restoration["desiredGeneration"]
            or restoration["activeGeneration"] == "0"
            or not runtime["restored"] or not runtime["rendering"]
        ):
            return None
        if self.epoch is not None and value["revision"]["epoch"] != self.epoch:
            raise RuntimeError("An unrelated edit replaced the Engine session")
        return value

    def read_catalog(self):
        response = run([str(self.build / "loopz-device-catalog-probe"), *self.known_uids])
        response.check_returncode()
        value = json.loads(response.stdout)
        if value["devicesOpened"] or value["permissionRequested"]:
            raise RuntimeError("The device observer performed IO or requested access")
        return {device["uid"]: device for device in value["devices"]}

    def catalog_matches(self):
        actual = self.read_catalog()
        expected = public_endpoints(self.expected_document)
        for uid in self.known_uids:
            device = actual[uid]
            if uid not in expected:
                if device["present"]:
                    return None
                continue
            wanted = expected[uid]
            if (
                not device["present"] or not device["alive"]
                or device["name"] != wanted["name"] or device["rate"] != wanted["rate"]
                or any(
                    sum(stream["channels"] for stream in device[key]) != wanted[key]
                    for key in ("inputs", "outputs")
                )
            ):
                return None
        if hasattr(self, "stable_identity"):
            stable = actual[self.stable_uid]
            identity = {key: stable[key] for key in ("object", "inputs", "outputs")}
            if identity != self.stable_identity:
                raise RuntimeError("An unrelated device edit replaced the streaming device or streams")
        return actual

    def require_client(self):
        if self.client is None or self.client.poll() is not None:
            raise RuntimeError("The continuous unrelated client ended before the edit sequence")

    def apply(self, phase, change):
        self.require_client()
        self.phase = phase
        previous_catalog = self.read_catalog()
        candidate = copy.deepcopy(self.document_value)
        change(candidate)
        response = run(
            [str(self.build / "loopz-config"), "write", str(self.document)],
            input=json.dumps(candidate),
        )
        response.check_returncode()
        self.document_value = candidate
        self.expected_document = self.canonical()
        self.save(phase + "-submitted.json", self.expected_document)
        status = wait_for(self.settled)
        metadata = wait_for(self.catalog_matches)
        self.require_client()
        report = {"status": status, "catalog": metadata, "beforeCatalog": previous_catalog}
        self.save(phase + ".json", report)
        if phase in ("rename", "source-mute", "device-mute", "zero-gain"):
            before = previous_catalog[self.edited_uid]
            after = metadata[self.edited_uid]
            if any(before[key] != after[key] for key in ("object", "inputs", "outputs")):
                raise RuntimeError("A control-only edit replaced the device or streams")
        if phase in ("hide-output", "restore-output"):
            if previous_catalog[self.edited_uid]["object"] != metadata[self.edited_uid]["object"]:
                raise RuntimeError("A playback visibility edit replaced the public device")
        print(phase + " passed", flush=True)

    def exercise_lifecycle(self):
        wait_for(self.ensure)
        wait_for(self.settled)
        initial = wait_for(self.catalog_matches)
        self.save("initial-catalog.json", initial)
        self.epoch = self.settled()["revision"]["epoch"]
        self.stable_identity = {
            key: initial[self.stable_uid][key] for key in ("object", "inputs", "outputs")
        }
        for suffix in ("stdout", "stderr"):
            self.client_files.append((self.folder / ("continuous." + suffix)).open("w"))
        arguments = self.probe_arguments(2, 0, 0)
        arguments[-1] = "30"
        self.client = subprocess.Popen(
            arguments, stdout=self.client_files[0], stderr=self.client_files[1]
        )
        began = time.monotonic()

        def active():
            self.require_client()
            status = self.settled()
            if status is None:
                return None
            device = next(
                item for item in status["runtime"]["devices"]
                if item["id"] == self.devices[0]["id"]
            )
            return device["active"] and int(device["renderedFrames"]) >= 96000

        wait_for(active)
        def source_controls(**controls):
            def change(doc):
                doc["devices"][1]["routing"]["sources"][0].update(controls)
            return change

        def device_controls(**controls):
            def change(doc):
                doc["devices"][1]["routing"].update(controls)
            return change

        self.apply("rename", lambda doc: doc["devices"][1].update(name="LoopZ Lifecycle Renamed"))
        self.apply("hide-output", source_controls(enabled=False))
        self.apply("restore-output", source_controls(enabled=True))
        self.apply("source-mute", source_controls(muted=True))
        self.apply("device-mute", device_controls(muted=True))
        self.apply("zero-gain", device_controls(gain=0))
        self.apply("disable", device_controls(enabled=False))
        self.apply("re-enable", device_controls(enabled=True))
        restored = copy.deepcopy(self.document_value["devices"][1])

        def remove_source(doc):
            doc["devices"][1]["bindings"] = []
            doc["devices"][1]["routing"]["sources"] = []

        self.apply("remove-pass-through", remove_source)
        def restore_source(doc):
            doc["devices"][1] = copy.deepcopy(restored)

        self.apply("restore-pass-through", restore_source)
        self.apply("delete", lambda doc: doc["devices"].pop())
        self.apply("restore-saved-identity", lambda doc: doc["devices"].append(copy.deepcopy(self.devices[1])))
        edit_seconds = time.monotonic() - began
        code = self.client.wait(timeout=35)
        for stream in self.client_files:
            stream.flush()
        text = (self.folder / "continuous.stdout").read_text()
        measurement = json.loads(text) if text else None
        self.save("continuous-result.json", {"exitCode": code, "measurement": measurement})
        if code or not measurement or not measurement["passed"]:
            raise RuntimeError("The unrelated route did not remain continuous through the edits")
        if not self.probe(2, 1, 1, "restored"):
            raise RuntimeError("The restored edited device did not pass audio validation")
        final = wait_for(self.catalog_matches)
        self.save("result.json", {
            "passed": True, "edits": 12, "editsCoveredByOneOpenClient": True,
            "editIntervalSeconds": edit_seconds, "continuousSeconds": 30,
            "channels": 2, "sampleRate": 48000, "clientBufferFrames": 64,
            "engineEpoch": self.epoch, "stableIdentity": self.stable_identity,
            "initialCatalog": initial, "finalCatalog": final,
            "continuousMeasurement": measurement,
            "physicalDevicesOpened": False, "permissionRequested": False,
            "limits": [
                "Private document reload path; no desktop UI gesture or physical IO",
                "No active client on the edited device during visibility transitions",
            ],
        })

    def cleanup(self):
        try:
            if self.client is not None and self.client.poll() is None:
                self.client.terminate()
                try:
                    self.client.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    self.client.kill()
                    self.client.wait(timeout=3)
        finally:
            for stream in self.client_files:
                stream.close()
            super().cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    options = parser.parse_args()
    check = LifecycleCheck(options.package, options.build)
    print(check.folder, flush=True)
    try:
        check.exercise_lifecycle()
    except BaseException as error:
        check.save("failure.json", {"error": str(error), "type": type(error).__name__})
        raise
    finally:
        check.cleanup()


if __name__ == "__main__":
    main()
