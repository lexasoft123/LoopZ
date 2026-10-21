#!/usr/bin/env python3
"""Repeat shared-writer recovery with a third prepared, nonrunning IO client."""

import argparse
import copy
import json
from pathlib import Path

from check_background_routing import Check, ROOT, definition
from check_mixed_routing import MarkerFixture, add_source


class SharedClientsCheck(Check):
    def __init__(self, package, build):
        self.fixture = None
        super().__init__(package, build)
        self.devices = [definition(2) for _ in range(4)]
        for device in self.devices[1:3]:
            device["routing"]["sources"][0]["gain"] = 0
            add_source(device, "shared", "virtual-device", self.devices[0]["id"], 2)
            # Two probes mix the same time-coded marker into device 0. Keep
            # their summed signal at the diagnostic's unit-gain reference.
            device["routing"]["sources"][-1]["gain"] = 0.5
        self.original = json.dumps({"version": 2, "devices": self.devices}, indent=2) + "\n"
        self.document.write_text(self.original)
        self.save("routes-tested.json", json.loads(self.original))

    def prepare_clients(self):
        # A streams to an unrelated device. B opens the shared writer's device
        # without starting it. The two probes subsequently start their own IO
        # leases on that device and must survive B's unused lease and recovery.
        arguments = [
            str(self.build / "LoopZ Test Source.app/Contents/MacOS/LoopZ Test Source"),
            "--multi-output-source",
            "--device-a",
            "com.lexasoft.loopz.device." + self.devices[3]["id"],
            "--device-b",
            "com.lexasoft.loopz.device." + self.devices[0]["id"],
            "--signal",
            "marker",
            "--buffer-frames",
            "64",
            "--seconds",
            "150",
        ]
        self.fixture = MarkerFixture(arguments, self.folder / "prepared-source.json")
        self.save("prepared-source-arguments.json", arguments)
        event = self.fixture.event("a-running-b-prepared")
        self.save("prepared-source-ready.json", copy.deepcopy(event))
        if event["signal"] != "marker" or event["bRunning"]:
            raise RuntimeError("The third client must remain prepared without running")

    def continuous_cases(self):
        return [("shared-1", 2, 0, 1), ("shared-2", 2, 0, 2)]

    def cleanup(self):
        try:
            if self.fixture is not None:
                self.save("prepared-source-final.json", self.fixture.finish())
        finally:
            super().cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    options = parser.parse_args()
    check = SharedClientsCheck(options.package, options.build)
    print(check.folder, flush=True)
    try:
        check.exercise(continuous=True, settled_session=True)
    except BaseException as error:
        check.record_failure(error)
        raise
    finally:
        check.cleanup()


if __name__ == "__main__":
    main()
