#!/usr/bin/env python3
"""Keep an input-only client open while its device's playback endpoint changes."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from check_background_routing import PIN, ROOT, run, wait_for
from check_device_lifecycle import LifecycleCheck
from check_mixed_routing import add_source
from routing_probe import ContinuousProbe


class ActiveInputCheck(LifecycleCheck):
    def __init__(self, package, build, reader_engine=None):
        self.source = None
        self.reader_engine = (
            reader_engine or package.resolve() / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine"
        )
        run(
            ["/usr/bin/codesign", "--verify", "--strict", "-R=" + PIN, str(self.reader_engine)]
        ).check_returncode()
        permission = run([str(self.reader_engine), "--permission-status"])
        permission.check_returncode()
        if json.loads(permission.stdout).get("microphone") != "authorized":
            raise RuntimeError(
                "The diagnostic reader requires existing access and never requests it"
            )
        super().__init__(package, build)
        self.save(
            "reader-provenance.json",
            {
                "path": str(self.reader_engine),
                "sha256": hashlib.sha256(self.reader_engine.read_bytes()).hexdigest(),
                "routingEngine": str(self.engine),
                "permissionRequested": False,
            },
        )
        # The tested input receives audio from an independent virtual route.
        # Its own pass-through stays silent, so hiding playback cannot remove
        # the test signal or make a legitimate source mute look like a dropout.
        edited = self.document_value["devices"][1]
        edited["routing"]["sources"][0]["gain"] = 0
        add_source(edited, "upstream", "virtual-device", self.devices[0]["id"], 2)
        self.document.write_text(json.dumps(self.document_value, indent=2) + "\n")
        self.save("routes-tested.json", self.document_value)
        self.expected_document = self.canonical()

    def reader_arguments(self, seconds):
        return [
            str(self.reader_engine),
            "--virtual-input-check",
            "--read-device",
            self.edited_uid,
            "--signal",
            "marker",
            "--buffer-frames",
            "64",
            "--seconds",
            str(seconds),
        ]

    def probe_input(self, seconds, label):
        output = (self.folder / (label + ".jsonl")).open("w")
        error = (self.folder / (label + ".stderr.log")).open("w")
        self.client_files.extend([output, error])
        self.client = subprocess.Popen(self.reader_arguments(seconds), stdout=output, stderr=error)
        self.save(
            label + "-client.json",
            {
                "pid": self.client.pid,
                "arguments": self.reader_arguments(seconds),
                "startedMonotonic": time.monotonic(),
            },
        )
        return self.client

    def input_result(self, process, label, timeout):
        code = process.wait(timeout=timeout)
        for stream in self.client_files:
            stream.flush()
        lines = (self.folder / (label + ".jsonl")).read_text().splitlines()
        value = json.loads(lines[-1]) if lines else None
        report = {"pid": process.pid, "exitCode": code, "measurement": value}
        self.save(label + "-result.json", report)
        return report

    def active_input(self):
        self.require_client()
        if self.source.process.poll() is not None:
            raise RuntimeError("The owned virtual signal generator ended early")
        snapshot = self.settled()
        if snapshot is None:
            return None
        edited = next(
            device
            for device in snapshot["runtime"]["devices"]
            if device["id"] == self.devices[1]["id"]
        )
        upstream = next(source for source in edited["sources"] if source["id"] == "upstream")
        return (
            edited["active"]
            and int(edited["renderedFrames"]) >= 96000
            and upstream["online"]
            and upstream["meter"]["valid"]
        )

    def exercise_active_input(self):
        wait_for(self.ensure)
        wait_for(self.settled)
        initial = wait_for(self.catalog_matches)
        self.save("initial-catalog.json", initial)
        self.epoch = self.settled()["revision"]["epoch"]
        self.stable_identity = {
            key: initial[self.stable_uid][key] for key in ("object", "inputs", "outputs")
        }
        generator = self.build / "LoopZ Test Source.app/Contents/MacOS/LoopZ Test Source"
        self.source = ContinuousProbe(
            [
                str(generator),
                "--send-tone",
                "--timestamped",
                "--device-uid",
                self.stable_uid,
                "--frequency-left",
                "1100",
                "--frequency-right",
                "2200",
                "--seconds",
                "90",
            ],
            self.folder / "source.json",
        )
        ready = self.source.event("source-started")
        if ready["rate"] != 48000:
            raise RuntimeError("The virtual fixture started at an unexpected rate")

        baseline = self.input_result(self.probe_input(5, "baseline"), "baseline", 10)
        if baseline["exitCode"] or not baseline["measurement"]["passed"]:
            raise RuntimeError("The active-input baseline failed before any visibility edit")

        client = self.probe_input(28, "continuous")
        wait_for(self.active_input)
        # Render counters can include the baseline. Observe real elapsed time
        # as well, so every edit lies after this reader's one-second warmup.
        time.sleep(2)
        events = []
        for cycle in range(4):
            for enabled in (False, True):
                phase = f"visibility-{cycle + 1}-{'show' if enabled else 'hide'}"

                def change(document):
                    document["devices"][1]["routing"]["sources"][0]["enabled"] = enabled

                self.apply(phase, change)
                observed = self.read_catalog()
                if observed[self.edited_uid]["object"] != initial[self.edited_uid]["object"]:
                    raise RuntimeError("Playback visibility replaced the active input's device")
                events.append(
                    {
                        "phase": phase,
                        "catalog": observed,
                        "readerPID": client.pid,
                        "readerStillRunning": client.poll() is None,
                    }
                )
                self.save("visibility-events.json", events)
                time.sleep(0.5)
                self.require_client()

        continuous = self.input_result(client, "continuous", 35)
        # A new reader is a distinct recovery observation and never replaces
        # the verdict for the client that actually spanned the edits.
        after = self.input_result(self.probe_input(5, "after"), "after", 10)
        passed = all(
            not report["exitCode"] and report["measurement"] and report["measurement"]["passed"]
            for report in (baseline, continuous, after)
        )
        self.save(
            "result.json",
            {
                "passed": passed,
                "visibilityEdits": 8,
                "sampleRate": 48000,
                "channels": 2,
                "clientBufferFrames": 64,
                "continuousSeconds": 28,
                "oneOpenInputOnlyClientAcrossEdits": True,
                "baseline": baseline,
                "continuous": continuous,
                "freshReaderAfterEdits": after,
                "events": events,
                "physicalDevicesOpened": False,
                "permissionRequested": False,
                "signalSource": "Independent owned virtual route; tested pass-through gain is zero",
                "limits": [
                    "Input-only AUHAL diagnostic; other clients and format/disable changes remain open",
                    "Private document reload path; no app capture or desktop gesture",
                ],
            },
        )
        if not passed:
            raise RuntimeError("An input client failed while playback visibility changed")

    def cleanup(self):
        try:
            if self.source is not None:
                self.save(
                    "source-stop.json",
                    {
                        "requestedByController": True,
                        "wasRunning": self.source.process.poll() is None,
                    },
                )
                self.source.close()
        finally:
            super().cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    parser.add_argument("--reader-engine", type=Path)
    options = parser.parse_args()
    check = ActiveInputCheck(options.package, options.build, options.reader_engine)
    print(check.folder, flush=True)
    try:
        check.exercise_active_input()
    except BaseException as error:
        check.record_failure(error)
        raise
    finally:
        check.cleanup()


if __name__ == "__main__":
    main()
