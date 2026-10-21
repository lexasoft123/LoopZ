#!/usr/bin/env python3
"""Opt-in real Engine crash recovery using a temporary capture-free graph."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import uuid

from routing_probe import ContinuousProbe, continuous_results

ROOT = Path(__file__).resolve().parents[1]
CHECKER = "/Library/Application Support/LoopZ/loopz-routing-install-check"
PIN = (
    'identifier "com.lexasoft.loopz.engine" and anchor apple generic '
    "and certificate 1[field.1.2.840.113635.100.6.2.6] exists "
    "and certificate leaf[field.1.2.840.113635.100.6.1.13] exists "
    'and certificate leaf[subject.OU] = "USJ7H3X44X"'
)


def run(arguments, **options):
    return subprocess.run(arguments, capture_output=True, text=True, timeout=20, **options)


def catalog():
    # A device can disappear between catalog enumeration and its property
    # reads. The protected checker fails that whole snapshot with empty exit 1.
    # Retry the complete observation; never substitute an empty catalog or
    # ignore a diagnostic/permission error when checking safe withdrawal.
    for attempt in range(5):
        result = run([CHECKER, "--snapshot"])
        if result.returncode == 0:
            return json.loads(result.stdout)
        if result.returncode != 1 or result.stdout or result.stderr or attempt == 4:
            result.check_returncode()
        time.sleep(0.1)
    raise RuntimeError("The routing catalog could not be observed")


def core_audio():
    found = run(["/usr/bin/pgrep", "-x", "coreaudiod"])
    found.check_returncode()
    pids = found.stdout.split()
    if len(pids) != 1:
        raise RuntimeError("Expected one Core Audio server")
    result = run(["/bin/ps", "-p", pids[0], "-o", "pid=,lstart=,command="])
    result.check_returncode()
    return result.stdout.strip()


def wait_for(predicate):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.2)
    raise RuntimeError("The temporary background graph did not reach its expected state")


def definition(channels):
    source = {
        "id": "pass-through",
        "channels": channels,
        "enabled": True,
        "muted": False,
        "gain": 1,
        "connections": [
            {"sourceChannel": i, "outputChannel": i, "gain": 1} for i in range(channels)
        ],
    }
    return {
        "id": str(uuid.uuid4()),
        "name": f"LoopZ Background Review {channels}",
        "sampleRate": 48000,
        "formatRequestReceipt": "",
        "monitors": [],
        "bindings": [
            {"id": "pass-through", "kind": "pass-through", "selector": "", "muteOriginal": False}
        ],
        "routing": {
            "enabled": True,
            "muted": False,
            "gain": 1,
            "safetyLimiter": True,
            "outputChannels": channels,
            "sources": [source],
        },
    }


class Check:
    def __init__(self, package, build, *, probe_engine=None, coordinated_baseline=False):
        if os.geteuid() == 0:
            raise RuntimeError("Run as the console user")
        if probe_engine is not None:
            probe_engine = Path(probe_engine).resolve(strict=True)
            run(
                ["/usr/bin/codesign", "--verify", "--strict", "-R=" + PIN, str(probe_engine)]
            ).check_returncode()
            permission = run([str(probe_engine), "--permission-status"])
            permission.check_returncode()
            if json.loads(permission.stdout).get("microphone") != "authorized":
                raise RuntimeError("The diagnostic requires existing access and never requests it")
        self.before = catalog()
        if self.before["routingDevices"]:
            raise RuntimeError("Stop the current routing graph before this isolated check")
        engines = run(["/usr/bin/pgrep", "-fl", r"/LoopZ Engine[.]app/Contents/MacOS/LoopZ Engine"])
        if engines.returncode not in (0, 1):
            raise RuntimeError("The existing Engine processes could not be inspected")
        if any(
            " --runtime-capabilities" not in line and " --permission-status" not in line
            for line in engines.stdout.splitlines()
        ):
            raise RuntimeError("Stop the existing Engine session before this isolated check")
        self.core_before = core_audio()
        self.metadata_path = ROOT / "artifacts/background-check-session.json"
        if self.metadata_path.exists() and json.loads(self.metadata_path.read_text()).get(
            "cleanupRequired"
        ):
            raise RuntimeError("The previous background check needs cleanup before another run")
        self.package = package.resolve()
        self.build = build.resolve()
        self.engine = self.package / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine"
        self.probe_engine = probe_engine or self.engine
        self.coordinated_baseline = coordinated_baseline
        self.folder = ROOT / "artifacts" / ("background-check-" + uuid.uuid4().hex[:8])
        self.folder.mkdir(mode=0o700)
        self.private = self.folder / "Private"
        self.private.mkdir(mode=0o700)
        self.document = self.private / "routes.json"
        self.devices = [definition(channels) for channels in (2, 8, 64, 8)]
        chain = self.devices[3]
        chain["bindings"].append(
            {
                "id": "bus",
                "kind": "virtual-device",
                "selector": self.devices[1]["id"],
                "muteOriginal": False,
            }
        )
        route = json.loads(json.dumps(chain["routing"]["sources"][0]))
        route["id"] = "bus"
        chain["routing"]["sources"].append(route)
        self.original = json.dumps({"version": 2, "devices": self.devices}, indent=2) + "\n"
        self.document.write_text(self.original)
        self.document.chmod(0o600)
        (self.folder / "routes-tested.json").write_text(self.original)
        self.label = "com.lexasoft.loopz.background-check." + uuid.uuid4().hex
        self.domain = f"gui/{os.getuid()}"
        self.target = self.domain + "/" + self.label
        self.arguments = [
            str(self.build / "loopz-session-host-runner"),
            str(self.engine.parents[2]),
            str(self.document),
            str(self.private / "Session"),
            self.label,
            "com.lexasoft.loopz.editor.control",
            PIN,
        ]
        self.observations = []
        self.status_observations = []
        self.metadata = {
            "folder": str(self.folder),
            "label": self.label,
            "domain": self.domain,
            "document": str(self.document),
            "engine": str(self.engine),
            "cleanupRequired": True,
        }
        self.save_metadata()
        self.save("before.json", {"catalog": self.before, "coreAudio": self.core_before})
        self.save(
            "probe-provenance.json",
            {
                "path": str(self.probe_engine),
                "sha256": hashlib.sha256(self.probe_engine.read_bytes()).hexdigest(),
                "routingEngine": str(self.engine),
                "permissionRequested": False,
            },
        )

    def save(self, name, data):
        (self.folder / name).write_text(json.dumps(data, indent=2) + "\n")

    def save_metadata(self):
        self.metadata_path.write_text(json.dumps(self.metadata, indent=2) + "\n")

    @contextmanager
    def record_runtime(self, enabled=False):
        """Bounded, read-only status samples kept independently of test verdicts."""
        if not enabled:
            yield
            return
        stopped = threading.Event()
        errors = []

        def collect():
            with (self.folder / "runtime-observations.jsonl").open("x") as stream:
                for _ in range(480):
                    if stopped.is_set():
                        break
                    observation = {"monotonic": time.monotonic()}
                    try:
                        response = run(
                            [
                                str(self.package / "LoopZ.app/Contents/MacOS/LoopZ"),
                                "--routing-status",
                            ]
                        )
                        observation.update(code=response.returncode, stderr=response.stderr[:4096])
                        if len(response.stdout) > 1024 * 1024:
                            raise RuntimeError("Runtime status exceeded its diagnostic limit")
                        if response.returncode == 0:
                            observation["snapshot"] = json.loads(response.stdout)
                    except Exception as error:
                        observation["inspectionError"] = str(error)
                    stream.write(json.dumps(observation) + "\n")
                    stream.flush()
                    if stopped.wait(0.25):
                        break

        def run_collector():
            try:
                collect()
            except Exception as error:
                errors.append(str(error))

        thread = threading.Thread(target=run_collector, daemon=True)
        thread.start()
        try:
            yield
        finally:
            stopped.set()
            thread.join(timeout=21)
            if thread.is_alive():
                raise RuntimeError("The read-only runtime recorder did not stop within its bound")
            if errors:
                raise RuntimeError("The read-only runtime recorder failed: " + "; ".join(errors))

    def record_failure(self, error):
        self.save("failure.json", {"type": type(error).__name__, "error": str(error)})
        observations = {}
        for name, read in (
            ("catalog", catalog),
            ("engine", self.restored_status),
            ("job", self.job),
        ):
            try:
                value = read()
                observations[name] = (
                    {"code": value.returncode, "stdout": value.stdout, "stderr": value.stderr}
                    if name == "job"
                    else value
                )
            except Exception as failure:
                observations[name] = {"inspectionError": str(failure)}
        self.save("failure-state.json", observations)

    def job(self):
        return run(["/bin/launchctl", "print", self.target])

    def ensure(self):
        result = run(self.arguments)
        self.observations.append(
            {
                "code": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
                "job": self.job().stdout,
            }
        )
        self.save("observations.json", self.observations)
        return result.returncode == 0

    def complete(self):
        actual = catalog()
        expected = {
            "com.lexasoft.loopz.device." + d["id"]: d["routing"]["outputChannels"]
            for d in self.devices
        }
        published = {d["uid"]: d["inputChannels"] for d in actual["routingDevices"]}
        return (
            actual
            if published == expected and all(d["alive"] for d in actual["routingDevices"])
            else None
        )

    def pid(self):
        result = self.job()
        if result.returncode:
            return None
        match = re.search(r"^\s*pid = (\d+)\s*$", result.stdout, re.MULTILINE)
        return int(match.group(1)) if match else None

    def restored_status(self):
        result = run([str(self.package / "LoopZ.app/Contents/MacOS/LoopZ"), "--routing-status"])
        snapshot = json.loads(result.stdout) if result.returncode == 0 else None
        self.status_observations.append(
            {
                "observedMonotonic": time.monotonic(),
                "code": result.returncode,
                "stderr": result.stderr,
                "snapshot": snapshot,
            }
        )
        self.save("engine-status-observations.json", self.status_observations)
        if result.returncode == 3:
            raise RuntimeError("The signed desktop interface seal failed validation")
        if snapshot is None:
            return None
        if snapshot["document"] != self.expected_document:
            raise RuntimeError("The authenticated Engine reported another routing document")
        restoration, runtime = snapshot["restoration"], snapshot["runtime"]
        return (
            snapshot
            if (
                restoration["phase"] == "restored"
                and restoration["activeGeneration"] == restoration["desiredGeneration"]
                and restoration["activeGeneration"] != "0"
                and runtime["activeGeneration"] == restoration["activeGeneration"]
                and runtime["started"]
                and runtime["restored"]
                and runtime["rendering"]
                and not runtime["documentError"]
                and not runtime["controlError"]
                and not runtime["renderError"]
            )
            else None
        )

    def probe_arguments(self, channels, source, destination, continuous=False):
        arguments = [
            str(self.probe_engine),
            "--routing-io-check",
            "--write-device",
            "com.lexasoft.loopz.device." + self.devices[source]["id"],
            "--read-device",
            "com.lexasoft.loopz.device." + self.devices[destination]["id"],
            "--channels",
            str(channels),
            "--buffer-frames",
            "64",
        ]
        return arguments + (
            [
                "--recovery-protocol",
                "stdio-v2" if getattr(self, "coordinated_baseline", False) else "stdio-v1",
            ]
            if continuous
            else ["--seconds", "5"]
        )

    def settle_continuous_clients(self, probes):
        deadline = time.monotonic() + 15
        previous, stable_since, observations = None, None, []
        while time.monotonic() < deadline:
            if any(probe.process.poll() is not None for probe in probes):
                raise RuntimeError("A prepared diagnostic client ended before baseline")
            snapshot = self.restored_status()
            public = self.complete()
            key = None
            if snapshot is not None and public is not None:
                key = sorted(
                    (
                        d["uid"],
                        d["inputLatencyFrames"],
                        d["inputChannels"],
                        d["outputChannels"],
                        d["sampleRate"],
                    )
                    for d in public["routingDevices"]
                )
            now = time.monotonic()
            if key is None or key != previous:
                stable_since = now if key is not None else None
            previous = key
            observations.append({"monotonic": now, "timing": key})
            self.save("client-startup.json", observations)
            if stable_since is not None and now - stable_since >= 2:
                return
            time.sleep(0.2)
        raise RuntimeError("Public IO timing did not settle before the measured baseline")

    def prepare_clients(self):
        """Optional owned source fixtures, started after private devices exist."""

    def continuous_cases(self):
        return [("2", 2, 0, 0), ("8", 8, 1, 3), ("64", 64, 2, 2)]

    def before_disconnect(self):
        """Optional healthy source/monitor observations before the guarded fault."""

    def after_restoration(self):
        """Optional source/monitor readiness before client signal validation."""

    def after_audio_checks(self):
        """Optional final source/monitor continuity observations."""

    def probe(self, channels, source, destination, phase):
        arguments = self.probe_arguments(channels, source, destination)
        result = run(arguments)
        measured = json.loads(result.stdout) if result.stdout else None
        self.save(
            f"{phase}-io-{channels}.json",
            {
                "arguments": arguments,
                "code": result.returncode,
                "measurement": measured,
                "stderr": result.stderr,
            },
        )
        print(f"{phase}: {channels} channels, exit {result.returncode}", flush=True)
        return result.returncode == 0 and measured["passed"]

    def exercise(self, continuous=False, settled_session=False):
        if continuous:
            capability = run([str(self.probe_engine), "--runtime-capabilities"])
            capability.check_returncode()
            capabilities = json.loads(capability.stdout)
            if capabilities.get("continuousRoutingProbe") != 1:
                raise RuntimeError("This signed Engine lacks the continuous-client diagnostic")
            if self.coordinated_baseline and capabilities.get("coordinatedRoutingProbe") != 1:
                raise RuntimeError("This signed diagnostic lacks coordinated client startup")
        validated = run([str(self.build / "loopz-config"), "validate", str(self.document)])
        self.save(
            "preflight.json",
            {"code": validated.returncode, "stdout": validated.stdout, "stderr": validated.stderr},
        )
        if validated.returncode:
            raise RuntimeError("The capture-free test document is invalid: " + validated.stderr)
        canonical = run([str(self.build / "loopz-config"), "read", str(self.document)])
        canonical.check_returncode()
        self.expected_document = json.loads(canonical.stdout)
        wait_for(self.ensure)
        ready_not_before = time.monotonic() + 12 if settled_session else 0
        first = wait_for(self.complete)
        old_pid = wait_for(self.pid)
        self.prepare_clients()
        if continuous:
            self.save("initial-engine-status.json", wait_for(self.restored_status))
        if settled_session:
            while time.monotonic() < ready_not_before:
                if self.pid() != old_pid:
                    raise RuntimeError("The private Engine changed during the settling interval")
                time.sleep(0.2)
            self.save("settled-session.json", {"passed": True, "minimumAgeBeforeClients": 12})
        self.save(
            "first-restoration.json", {"catalog": first, "job": self.job().stdout, "pid": old_pid}
        )
        with ExitStack() as stack:
            probes = []
            if continuous:
                for label, channels, source, destination in self.continuous_cases():
                    arguments = self.probe_arguments(channels, source, destination, True)
                    probe = stack.enter_context(
                        ContinuousProbe(arguments, self.folder / f"continuous-io-{label}.json")
                    )
                    probes.append(probe)
                if self.coordinated_baseline:
                    for probe in probes:
                        probe.event("prepared", timeout=15)
                    self.settle_continuous_clients(probes)
                    for probe in probes:
                        probe.command("begin-baseline")
                for probe in probes:
                    probe.event("ready")
                for probe in probes:
                    probe.command("begin-disconnect")
                for probe in probes:
                    if probe.event("disconnect-ready").get("baselinePassed") is not True:
                        raise RuntimeError("An existing-client baseline failed before the crash")
            elif not self.probe(2, 0, 0, "before"):
                raise RuntimeError("Audio failed before the crash test")
            self.before_disconnect()
            verified = self.ensure()
            signature = run(
                [
                    "/usr/bin/codesign",
                    "--verify",
                    "--strict",
                    "--verbose=2",
                    "-R=" + PIN,
                    str(old_pid),
                ]
            )
            self.save(
                "pre-crash-identity.json",
                {
                    "managed": verified,
                    "job": self.job().stdout,
                    "signatureCode": signature.returncode,
                    "signatureOutput": signature.stdout + signature.stderr,
                },
            )
            if not verified or signature.returncode or self.pid() != old_pid:
                raise RuntimeError(
                    "The original test Engine could not be reverified before the crash"
                )
            # Ask launchd to signal this verified unique job. It owns the current
            # process identity, avoiding a separate cached-PID signal race.
            crash_requested = time.monotonic()
            result = run(["/bin/launchctl", "kill", "SIGKILL", self.target])
            result.check_returncode()
            new_pid = wait_for(
                lambda: value if (value := self.pid()) and value != old_pid else None
            )
            replacement_observed = time.monotonic()
            wait_for(self.ensure)
            restored = wait_for(self.complete)
            acknowledgement = wait_for(self.restored_status) if continuous else None
            acknowledgement_observed = time.monotonic()
            if self.document.read_text() != self.original:
                raise RuntimeError("The saved routing document changed during recovery")
            self.save(
                "restoration.json",
                {
                    "oldPID": old_pid,
                    "newPID": new_pid,
                    "catalog": restored,
                    "helperAcknowledgement": acknowledgement,
                    "replacementObservedMs": (replacement_observed - crash_requested) * 1000,
                    "acknowledgementObservedMs": (acknowledgement_observed - crash_requested)
                    * 1000,
                    "documentSHA256": hashlib.sha256(self.original.encode()).hexdigest(),
                },
            )
            self.after_restoration()
            if continuous:
                for probe in probes:
                    probe.command("restored")
                result = continuous_results(probes)
                self.save("continuous-result.json", result)
                if not result["passed"]:
                    raise RuntimeError(
                        "Existing-client recovery failed: " + str(result["failures"])
                    )
            else:
                with ThreadPoolExecutor(max_workers=3) as pool:
                    results = list(
                        pool.map(
                            lambda value: self.probe(*value, "after"),
                            [(2, 0, 0), (8, 1, 3), (64, 2, 2)],
                        )
                    )
                if not all(results):
                    raise RuntimeError("Audio did not recover on every tested route")
            self.after_audio_checks()
        self.save(
            "result.json",
            {
                "passed": True,
                "oldPID": old_pid,
                "newPID": new_pid,
                "restoredDevices": len(self.devices),
                "continuousClientsValidated": continuous,
                "documentUnchanged": True,
                "loginRegistered": False,
                "candidateInstalled": False,
            },
        )

    def cleanup(self):
        errors = []
        withdrawn = False
        job_removed = False
        after = None
        defaults_unchanged = None
        core_unchanged = None
        try:
            result = run(
                [str(self.build / "loopz-config"), "write", str(self.document)],
                input='{"version":2,"devices":[]}',
            )
            result.check_returncode()
            wait_for(lambda: not catalog()["routingDevices"])
            withdrawn = True
        except BaseException as error:
            errors.append("Graph withdrawal: " + str(error))
        try:
            job = self.job()
            if job.returncode == 0:
                if str(self.engine) not in job.stdout:
                    raise RuntimeError("Refusing to stop an unrecognized job during cleanup")
                run(["/bin/launchctl", "bootout", self.target])

            def gone():
                state = self.job()
                return state.returncode in (3, 113) and "Could not find service" in (
                    state.stdout + state.stderr
                )

            wait_for(gone)
            job_removed = True
        except BaseException as error:
            errors.append("Job removal: " + str(error))
        try:
            after = catalog()
            withdrawn = not after["routingDevices"]
            defaults_unchanged = all(
                after[key] == self.before[key]
                for key in ("defaultInput", "defaultOutput", "defaultSystemOutput")
            )
            core_unchanged = core_audio() == self.core_before
            if not withdrawn or not defaults_unchanged or not core_unchanged:
                raise RuntimeError("Routing cleanup, audio service or defaults did not match")
        except BaseException as error:
            errors.append("Final audio state: " + str(error))
        self.save(
            "cleanup.json",
            {
                "withdrawn": withdrawn,
                "jobRemoved": job_removed,
                "defaultsUnchanged": defaults_unchanged,
                "coreAudioUnchanged": core_unchanged,
                "after": after,
                "errors": errors,
            },
        )
        complete = (
            not errors and withdrawn and job_removed and defaults_unchanged and core_unchanged
        )
        self.metadata.update(cleanupRequired=not complete, bootoutVerified=job_removed)
        self.save_metadata()
        if not complete:
            raise RuntimeError("Background check cleanup needs attention: " + "; ".join(errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build/editor-macos")
    parser.add_argument(
        "--continuous-clients",
        action="store_true",
        help="Keep 2/8/64-channel clients open across the verified Engine crash",
    )
    parser.add_argument(
        "--settled-session",
        action="store_true",
        help="Keep the private Engine alive for at least 12 seconds before opening test clients",
    )
    args = parser.parse_args()
    check = Check(args.package, args.build)
    print(check.folder, flush=True)
    try:
        check.exercise(args.continuous_clients, args.settled_session)
    except BaseException as error:
        check.save("failure.json", {"type": type(error).__name__, "error": str(error)})
        raise
    finally:
        check.cleanup()


if __name__ == "__main__":
    main()
