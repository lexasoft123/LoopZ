#!/usr/bin/env python3
"""Opt-in Chrome output/app-capture checks using owned, silent virtual endpoints."""

import argparse
import json
from pathlib import Path
import subprocess
import time

from check_background_routing import Check, ROOT, PIN, definition, run, wait_for
from check_mixed_routing import add_source
from routing_probe import ContinuousProbe


def capture_scope(selected, owned, sink_uid, allow_existing=False):
    """Reject application capture beyond the permitted process/output scope."""
    if not selected["success"] or not selected["producing"] or not selected["processes"]:
        return False
    processes = {item["pid"] for item in selected["processes"]}
    if not allow_existing and not processes.issubset(owned):
        raise RuntimeError("Chrome selection includes a process outside the owned browser")
    if {item["uid"] for item in selected["outputs"]} != {sink_uid}:
        raise RuntimeError("Chrome selection includes an output outside the owned virtual sink")
    return selected


class ChromeFixture(ContinuousProbe):
    ready = False

    def __init__(self, directory, target, record):
        super().__init__(
            [
                "node",
                str(ROOT / "interface/tests/chrome-audio-fixture.mjs"),
                str(directory),
                target,
            ],
            record,
        )
        self.deadline = time.monotonic() + 180

    def request(self, operation):
        if operation not in ("start", "status", "stop", "quit"):
            raise ValueError("Unknown Chrome operation")
        self.process.stdin.write((json.dumps({"operation": operation}) + "\n").encode())
        self.process.stdin.flush()
        events = {
            "start": "started", "status": "status", "stop": "stopped", "quit": "complete"
        }
        return self.event(events[operation], 15)

    def close(self):
        try:
            if self.process is not None and self.process.poll() is None:
                if self.ready:
                    if not self.request("quit")["closed"]:
                        raise RuntimeError("The owned browser did not close")
                self.process.wait(timeout=8)
        finally:
            super().close()


class ChromeCheck(Check):
    def __init__(self, package, build, allow_existing=False):
        engine = package.resolve() / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine"
        run(["/usr/bin/codesign", "--verify", "--strict", "-R=" + PIN, str(engine)]).check_returncode()
        caps = json.loads(run([str(engine), "--runtime-capabilities"]).stdout)
        permission = json.loads(run([str(engine), "--permission-status"]).stdout)
        if (
            caps.get("virtualInputSineProbe") != 1
            or permission.get("microphone") != "authorized"
        ):
            raise RuntimeError("A signed sine-input diagnostic with existing access is required")
        self.browser = None
        self.probes = []
        self.allow_existing = allow_existing
        super().__init__(package, build)
        self.devices = [definition(2) for _ in range(3)]
        for index, device in enumerate(self.devices):
            device["name"] = "LoopZ Chrome Review " + device["id"]
            if index:
                device["routing"]["sources"][0]["gain"] = 0
                add_source(device, "chrome", "application", "com.google.Chrome", 2)
        self.original = json.dumps({"version": 2, "devices": self.devices}, indent=2) + "\n"
        self.document.write_text(self.original)
        self.save("routes-tested.json", json.loads(self.original))

    def uid(self, index):
        return "com.lexasoft.loopz.device." + self.devices[index]["id"]

    def browser_status(self):
        state = self.browser.request("status")
        audio = state["audio"]
        if (
            not audio["sinkMatches"] or audio["failure"]
            or audio["state"] != "running" or audio["rate"] != 48000
        ):
            raise RuntimeError("The owned browser lost its explicit virtual output")
        return state

    def inspect_scope(self):
        state = self.browser_status()
        owned = {int(process["id"]) for process in state["processInfo"]}
        response = run([
            str(self.build / "loopz-application-ownership-tests"),
            "--inspect", "com.google.Chrome",
        ])
        response.check_returncode()
        selected = json.loads(response.stdout)
        self.save("chrome-latest-ownership.json", {"browser": state, "selection": selected})
        return capture_scope(selected, owned, self.uid(0), self.allow_existing)

    def reader(self, index, frames, seconds, label):
        command = [
            str(self.engine), "--virtual-input-check", "--read-device", self.uid(index),
            "--signal", "sine", "--buffer-frames", str(frames), "--seconds", str(seconds),
        ]
        output = (self.folder / (label + ".jsonl")).open("w")
        error = (self.folder / (label + ".stderr.log")).open("w")
        process = subprocess.Popen(command, stdout=output, stderr=error, text=True)
        self.probes.append((process, output, error))
        return process

    def result(self, process, label):
        code = process.wait(timeout=15)
        lines = (self.folder / (label + ".jsonl")).read_text().splitlines()
        result = json.loads(lines[-1]) if lines else None
        self.save(label + "-result.json", {"exitCode": code, "measurement": result})
        if (
            code or not result or not result["passed"]
            or result["markerChecked"] or result["delayMs"] is not None
        ):
            raise RuntimeError("Chrome signal validation failed: " + label)
        return result

    def exercise_browser(self, output_only=False):
        run([str(self.build / "loopz-config"), "validate", str(self.document)]).check_returncode()
        canonical = run([str(self.build / "loopz-config"), "read", str(self.document)])
        canonical.check_returncode()
        self.expected_document = json.loads(canonical.stdout)
        wait_for(self.ensure)
        wait_for(self.complete)
        self.save("initial-status.json", wait_for(self.restored_status))
        chrome = self.folder / "Chrome"
        chrome.mkdir(mode=0o700)
        self.browser = ChromeFixture(chrome, self.devices[0]["name"], self.folder / "browser.json")
        ready = self.browser.event("ready", timeout=30)
        self.browser.ready = True
        self.save("browser-ready.json", ready)
        if not ready["isolatedProfile"] or ready["mediaInputRequested"]:
            raise RuntimeError("The browser fixture did not establish output-only isolation")
        self.browser.request("start")
        print("Chrome is writing only to its owned virtual sink", flush=True)
        measurements = {}
        for frames in (512, 64):
            label = "passthrough-" + str(frames)
            process = self.reader(0, frames, 8, label)
            while process.poll() is None:
                self.browser_status()
                time.sleep(0.5)
            measurements[label] = self.result(process, label)
            print(label + " passed", flush=True)
        output_result = {
            "passed": True,
            "mode": "chrome-output-only",
            "browserVersion": ready["browserVersion"],
            "measurements": measurements,
            "applicationCaptureValidated": False,
            "physicalMicrophoneOpened": False,
            "physicalOutputSelected": False,
            "audioSamplesRecorded": False,
            "latencyMeasured": False,
        }
        self.save("passthrough-result.json", output_result)
        if output_only:
            self.save("result.json", output_result)
            return
        selected = wait_for(self.inspect_scope)
        self.save("chrome-capture-scope.json", selected)
        first = self.reader(1, 64, 10, "application")
        second = self.reader(2, 64, 10, "fanout")
        before = None
        started = time.monotonic()
        while first.poll() is None or second.poll() is None:
            if not self.inspect_scope():
                raise RuntimeError("Chrome source ownership or output activity is unavailable")
            if time.monotonic() - started >= 3:
                snapshot = self.restored_status()
                if snapshot:
                    providers = snapshot["runtime"]["sources"]
                    if len(providers) != 1 or not all(
                        source["connected"] and source["complete"] for source in providers
                    ):
                        raise RuntimeError("Shared Chrome capture did not become one complete provider")
                    value = {
                        "epoch": snapshot["revision"]["epoch"],
                        "attempts": providers[0]["attempts"],
                    }
                    if before is None:
                        before = value
                    elif before != value:
                        raise RuntimeError("A healthy browser interval unexpectedly reacquired its capture")
            time.sleep(0.5)
        if before is None:
            raise RuntimeError("No steady shared-browser interval was observed")
        measurements["application"] = self.result(first, "application")
        measurements["fanout"] = self.result(second, "fanout")
        self.save("result.json", {
            "passed": True,
            "browserVersion": ready["browserVersion"],
            "measurements": measurements,
            "sharedProvider": before,
            "captureScope": selected,
            "physicalMicrophoneOpened": False,
            "physicalOutputSelected": False,
            "audioSamplesRecorded": False,
            "latencyMeasured": False,
            "limits": [
                "Owned headless Chrome profile, 48 kHz stereo only",
                "No browser/renderer fault or Engine replacement in this run",
            ],
        })

    def cleanup(self):
        errors = []
        for process, output, error in self.probes:
            try:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
            finally:
                output.close()
                error.close()
        if self.browser is not None:
            try:
                self.browser.close()
            except BaseException as error:
                errors.append(str(error))
        try:
            super().cleanup()
        except BaseException as error:
            errors.append(str(error))
        self.save("chrome-cleanup.json", {"passed": not errors, "errors": errors})
        if errors:
            self.metadata["cleanupRequired"] = True
            self.save_metadata()
            raise RuntimeError("Chrome cleanup failed: " + "; ".join(errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-release")
    parser.add_argument("--output-only", action="store_true")
    parser.add_argument(
        "--allow-existing-chrome-helpers", action="store_true",
        help="Requires explicit user authorization for application capture across Chrome profiles",
    )
    options = parser.parse_args()
    check = ChromeCheck(options.package, options.build, options.allow_existing_chrome_helpers)
    print(check.folder, flush=True)
    try:
        check.exercise_browser(options.output_only)
    except BaseException as error:
        check.save("failure.json", {"error": str(error), "type": type(error).__name__})
        raise
    finally:
        check.cleanup()


if __name__ == "__main__":
    main()
