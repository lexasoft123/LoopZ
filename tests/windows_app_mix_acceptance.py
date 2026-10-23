"""Opt-in silent selected-app -> installed mixer -> public microphone acceptance.

Uses the existing automatic setup service; no elevated helper or global device
changes. Adds/removes only one unique temporary mix. Marker apps open a pinned
virtual Feed, and the saved document must have no hardware monitors.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


def main():
    app, marker, recorder, feed, destination = sys.argv[1:]
    app, marker, recorder, destination = map(Path, (app, marker, recorder, destination))
    destination.mkdir(exist_ok=False, parents=True)
    control = app / "loopz-windows-control.exe"
    document_path = Path(os.environ["LOCALAPPDATA"]) / "LoopZ" / "routing.json"
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()

    def invoke(arguments, timeout=30):
        result = subprocess.run(arguments, capture_output=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).decode("utf-8", errors="replace"))
        return json.loads(result.stdout.decode("utf-8-sig"))

    def state():
        return invoke([str(control), "--state"])

    def await_state(predicate, timeout=45):
        deadline = time.monotonic() + timeout
        while True:
            current = state()
            if predicate(current):
                return current
            if time.monotonic() > deadline:
                (destination / "timeout-state.json").write_text(json.dumps(current, indent=2))
                raise RuntimeError("Installed app acceptance timed out")
            time.sleep(0.2)

    original = state()
    original_document = original["snapshot"]["document"]
    if any(device["monitors"] for device in original_document["devices"]):
        raise RuntimeError("Silent acceptance refuses saved hardware monitors")
    if not any(
        d["uid"] == feed and d.get("virtualRole") == "loopz-feed"
        for d in original["inventory"]["devices"]
    ):
        raise RuntimeError("Pinned source endpoint is not an active LoopZ Feed")
    original_hash = digest(document_path)
    original_defaults = invoke([str(recorder), "--defaults"])
    original_uids = {
        d["id"]: d.get("microphone", {}).get("uid")
        for d in original["snapshot"]["runtime"]["devices"]
    }
    mix = "app-mix-acceptance-" + str(uuid.uuid4())
    children = []
    report = {
        "ok": False,
        "mix": mix,
        "beforeDocumentHash": original_hash,
        "controlHash": digest(control),
        "markerHash": digest(marker),
        "recorderHash": digest(recorder),
        "cleanupComplete": False,
    }
    with tempfile.TemporaryDirectory(prefix="LoopZ-app-mix-") as temporary:
        temporary = Path(temporary)

        def edit(commands):
            current = state()
            request = temporary / "request.json"
            request.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "operation": "edit",
                        "request": {
                            "version": 1,
                            "expected": current["snapshot"]["revision"],
                            "commands": commands,
                        },
                    }
                )
            )
            reply = invoke([str(control), "--request", str(request)])
            if not reply.get("ok"):
                raise RuntimeError(json.dumps(reply))

        def row(current):
            return next(d for d in current["snapshot"]["runtime"]["devices"] if d["id"] == mix)

        def publication(current):
            return next(
                m
                for m in current["snapshot"]["runtime"]["monitors"]
                if m["device"] == mix and m["publication"]
            )

        def counters(current):
            device = row(current)
            source = next(s for s in device["sources"] if s["id"] == "selected")
            output = publication(current)
            return {
                "sourceMissing": source["missingFrames"],
                "publicationMissing": output["missingFrames"],
                "graphFailures": device["graphFailures"],
                "renderFailures": output["renderFailures"],
                "driverQueue": device["microphone"]["transportTiming"]["driverQueue"],
            }

        try:
            edit(
                [
                    {
                        "type": "createDevice",
                        "id": mix,
                        "name": "App isolation acceptance",
                        "passThrough": False,
                    },
                    {"type": "setDeviceControls", "device": mix, "controls": {"gain": 0.5}},
                ]
            )
            provisioned = await_state(lambda s: row(s)["microphone"].get("connected", False))
            report["mixKey"] = row(provisioned)["microphone"]["mixKey"]
            report["microphoneUid"] = row(provisioned)["microphone"]["uid"]
            for name, mode in (("selected", "--feed-tone"), ("excluded", "--feed-tone-high")):
                executable = temporary / (name + ".exe")
                shutil.copyfile(marker, executable)
                log = open(destination / (name + "-marker.log"), "wb")
                process = subprocess.Popen(
                    [str(executable), mode, "60", feed], stdout=log, stderr=log
                )
                children.append((process, log))
            selected_path = str(temporary / "selected.exe")
            discovered = await_state(
                lambda s: any(
                    os.path.normcase(a["bundle"]) == os.path.normcase(selected_path)
                    for a in s["inventory"]["applications"]
                )
            )
            selector = next(
                a["bundle"]
                for a in discovered["inventory"]["applications"]
                if os.path.normcase(a["bundle"]) == os.path.normcase(selected_path)
            )
            edit(
                [
                    {
                        "type": "addSource",
                        "device": mix,
                        "id": "selected",
                        "kind": "application",
                        "selector": selector,
                        "muteOriginal": False,
                        "channels": 2,
                    }
                ]
            )
            await_state(
                lambda s: any(
                    source["id"] == "selected" and source["online"] for source in row(s)["sources"]
                )
            )
            time.sleep(3)
            before = state()
            report["beforeCounters"] = counters(before)
            key = row(before)["microphone"]["mixKey"]
            diagnostics = []
            capture = subprocess.Popen(
                [str(recorder), "--record-app", key, "30000"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                deadline = time.monotonic() + 45
                while capture.poll() is None:
                    diagnostics.append(counters(state())["driverQueue"])
                    if time.monotonic() > deadline:
                        raise TimeoutError("Public microphone recording exceeded its deadline")
                    time.sleep(0.2)
                output, errors = capture.communicate(timeout=5)
                recording = json.loads(output.decode("utf-8"))
            finally:
                if capture.poll() is None:
                    capture.terminate()
                    capture.wait(timeout=5)
            report["driverSamples"] = diagnostics
            report["recording"] = recording
            after = state()
            diagnostics.append(counters(after)["driverQueue"])
            # Driver diagnostics use interrupt time with system-tick precision.
            # Choose samples at least 50 ms outside the native collection window.
            start, end = recording["collectionStart100ns"], recording["collectionEnd100ns"]
            before_samples = [
                d for d in diagnostics if d["available"] and d["sampleTime100ns"] < start - 500000
            ]
            after_samples = [
                d for d in diagnostics if d["available"] and d["sampleTime100ns"] > end + 500000
            ]
            if not before_samples or not after_samples:
                raise RuntimeError("Driver diagnostics do not bracket the recording window")
            boundary_before = max(before_samples, key=lambda d: d["sampleTime100ns"])
            boundary_after = min(after_samples, key=lambda d: d["sampleTime100ns"])
            report["driverBoundaryBefore"] = boundary_before
            report["driverBoundaryAfter"] = boundary_after
            report["driverWindowDeltas"] = {
                name: boundary_after[name] - boundary_before[name]
                for name in ("missingFrames", "droppedFrames", "underruns", "leaseSilentFrames")
            }
            if any(report["driverWindowDeltas"].values()):
                raise RuntimeError(
                    "Driver audio loss counters increased across the recording window"
                )
            report["afterCounters"] = counters(after)
            (destination / "after-state.json").write_text(json.dumps(after, indent=2))
            for name in ("sourceMissing", "publicationMissing", "graphFailures", "renderFailures"):
                if report["afterCounters"][name] != report["beforeCounters"][name]:
                    raise RuntimeError(name + " increased during recording")
            if not recording["ok"] or any(process.poll() is not None for process, _ in children):
                raise RuntimeError("Recording failed or a marker exited early")
            report["ok"] = True
        except Exception as error:
            report["error"] = str(error)
            raise
        finally:
            errors = []
            try:
                if any(d["id"] == mix for d in state()["snapshot"]["document"]["devices"]):
                    edit([{"type": "deleteDevice", "device": mix}])
                final = await_state(
                    lambda s: mix not in s.get("microphoneSetup", {}).get("devices", {})
                    and all(d["id"] != mix for d in s["snapshot"]["runtime"]["devices"])
                    and all(
                        d["uid"] != report.get("microphoneUid") for d in s["inventory"]["devices"]
                    )
                )
                if final["snapshot"]["document"] != original_document:
                    raise RuntimeError("Existing document changed during acceptance")
                final_uids = {
                    d["id"]: d.get("microphone", {}).get("uid")
                    for d in final["snapshot"]["runtime"]["devices"]
                }
                if final_uids != original_uids:
                    raise RuntimeError("Existing microphone selections changed")
                report["afterDocumentHash"] = digest(document_path)
                report["defaultsAfter"] = invoke([str(recorder), "--defaults"])
                report["defaultsBefore"] = original_defaults
                if (
                    report["afterDocumentHash"] != original_hash
                    or report["defaultsAfter"] != original_defaults
                ):
                    raise RuntimeError("Saved bytes or default audio roles changed")
                report["originalMicrophonesPreserved"] = True
                report["cleanupComplete"] = True
            except Exception as error:
                errors.append(str(error))
            for process, log in children:
                try:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=10)
                    log.close()
                except Exception as error:
                    errors.append(str(error))
            report["cleanupErrors"] = errors
            if errors:
                report["ok"] = False
            (destination / "report.json").write_text(json.dumps(report, indent=2))
            if errors:
                raise RuntimeError("Acceptance cleanup failed: " + "; ".join(errors))
    print(json.dumps(report))


if __name__ == "__main__":
    main()
