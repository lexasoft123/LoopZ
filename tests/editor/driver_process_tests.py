"""Explicit private loaded-library/host recovery, never the real audio driver."""

import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from broker_host import BrokerInspector, BrokerPolicy
from broker_lifecycle import BrokerLifecycle
from driver_runtime import DriverInspector, DriverPolicy
from driver_record import DriverRecord
from driver_lifecycle import DriverLifecycle
from install_routing import command
from install_transaction import files
from process_code import process_generation, same_generation
from suite_files import SuiteFiles
from suite_update import NAMES, BROKER

HOST_ID = "com.lexasoft.loopz.driver-host-fixture"
IMAGE_ID = "com.lexasoft.loopz.driver-image-fixture"
MARKER = b"private fixture; never production routing or audio"


def verify_private(path, identifier):
    command(
        "/usr/bin/codesign", "--verify", "--strict", "-R", '=identifier "' + identifier + '"', path
    )


def private_policy(root):
    if (
        os.geteuid() == 0
        or root.parent != Path("/private/tmp")
        or not root.name.startswith("loopz-driver-process-")
    ):
        raise RuntimeError("The driver fixture requires its private non-root namespace")
    info = root.lstat()
    if root.is_symlink() or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise RuntimeError("Unprotected private driver workspace")
    label = plistlib.loads((root / "driver-host.plist").read_bytes())["Label"]
    if not re.fullmatch(r"com\.lexasoft\.loopz\.test\.driver-process\.[a-f0-9]{32}", label):
        raise RuntimeError("A non-fixture job cannot be controlled by this test")
    policy = BrokerPolicy(
        program=root / "driver-host",
        definition=root / "driver-host.plist",
        domain=f"gui/{os.geteuid()}",
        owner=os.geteuid(),
        label=label,
        identifier=HOST_ID,
        requirement=f'identifier "{HOST_ID}"',
        mach_service="",
        arguments=(
            str(root / "HAL/LoopZAudio.driver/Contents/MacOS/LoopZAudio"),
            str(root / "events.jsonl"),
        ),
    )
    driver = DriverPolicy(
        plugin_root=root / "HAL",
        file_owner=os.geteuid(),
        host=policy.program,
        host_owner=os.geteuid(),
        host_requirement=policy.requirement,
    )
    job = BrokerInspector(
        policy,
        static_code=lambda path, identifier, requirement, engine: verify_private(path, identifier),
    )

    def candidates():
        state = job.job()
        return [state["pid"]] if state and state["ready"] else []

    inspect = DriverInspector(
        driver,
        candidates=candidates,
        verify_bundle=lambda path: verify_private(path, IMAGE_ID),
    )
    return policy, job, driver, inspect


def admission(root):
    if (root / "admission").read_bytes() != MARKER:
        raise RuntimeError("Private driver admission changed")


def signaller(root, inspector, images):
    def checked(process, *, force):
        if not force or process["pid"] not in inspector.candidates():
            raise RuntimeError("Signal target is not this fixture's current private job")
        audited = inspector.inspect(process["pid"], images)
        if audited["process"] != process:
            raise RuntimeError("Fixture process generation changed before signal")
        with (root / "signals.jsonl").open("a") as log:
            log.write(
                json.dumps({"actor": process_generation(os.getpid()), "target": process}) + "\n"
            )
            log.flush()
            os.fsync(log.fileno())
        if process_generation(process["pid"], os.geteuid()) != process:
            raise RuntimeError("Fixture PID was reused before its one-process signal")
        os.kill(process["pid"], signal.SIGKILL)

    return checked


def wait_for(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while True:
        try:
            value = predicate()
        except ProcessLookupError:
            value = None
        if value:
            return value
        if time.monotonic() >= deadline:
            raise RuntimeError("The private driver fixture did not reach its expected state")
        time.sleep(0.1)


def events(root):
    path = root / "events.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def abrupt_worker(root, boundary):
    if boundary not in {"signal:intent", "signal:dispatched", "signal:recorded"}:
        raise ValueError("Unknown fixture exit boundary")
    policy, job, driver, inspector = private_policy(root)
    targets = {
        name: root / ("HAL" if name == "LoopZAudio.driver" else "installed") / name
        for name in NAMES
    }
    with (
        SuiteFiles(root / "support", targets) as transaction,
        DriverRecord(root / "support", driver) as record,
    ):
        transaction.load()
        record.bind(transaction.value["token"])
        images = inspector.images(transaction)

        def die(event):
            if event == boundary:
                os._exit(77)

        lifecycle = DriverLifecycle(
            inspector,
            record,
            lambda: admission(root),
            lambda: job.job(),
            signaller=signaller(root, inspector, images),
            checkpoint=die,
        )
        lifecycle.ensure_image("replacement", images)
    raise RuntimeError("The fixture worker did not reach its requested exit boundary")


def run(host, original, replacement, *, crash_at=None, mixed_first=False):
    if os.geteuid() == 0:
        raise RuntimeError("Run this fixture as the logged-in user, never root")
    production = BrokerInspector().job()
    root = Path(tempfile.mkdtemp(prefix="loopz-driver-process-", dir="/private/tmp"))
    (root / "installed").mkdir(mode=0o700)
    (root / "HAL").mkdir(mode=0o700)
    (root / "source").mkdir(mode=0o700)
    (root / "support").mkdir(mode=0o700)
    (root / "admission").write_bytes(MARKER)
    targets = {
        name: root / ("HAL" if name == "LoopZAudio.driver" else "installed") / name
        for name in NAMES
    }
    shutil.copy2(host, root / "driver-host")
    command(
        "/usr/bin/codesign", "--force", "--sign", "-", "--identifier", HOST_ID, root / "driver-host"
    )
    for name in NAMES:
        for path, label in ((targets[name], "old"), (root / "source" / name, "new")):
            if name == "LoopZAudio.driver":
                binary = path / "Contents/MacOS/LoopZAudio"
                binary.parent.mkdir(parents=True)
                shutil.copy2(original if label == "old" else replacement, binary)
                (path / "Contents/Info.plist").write_bytes(
                    plistlib.dumps(
                        {
                            "CFBundleExecutable": "LoopZAudio",
                            "CFBundleIdentifier": IMAGE_ID,
                            "CFBundlePackageType": "BNDL",
                            "CFBundleVersion": "1" if label == "old" else "2",
                        }
                    )
                )
                command(
                    "/usr/bin/codesign", "--force", "--sign", "-", "--identifier", IMAGE_ID, path
                )
            elif name == BROKER:
                path.write_text(label + name)
            else:
                path.mkdir()
                (path / "payload").write_text(label + name)
    label = "com.lexasoft.loopz.test.driver-process." + uuid.uuid4().hex
    definition = BrokerPolicy(
        program=root / "driver-host",
        definition=root / "driver-host.plist",
        domain=f"gui/{os.geteuid()}",
        owner=os.geteuid(),
        label=label,
        identifier=HOST_ID,
        requirement=f'identifier "{HOST_ID}"',
        mach_service="",
        arguments=(
            str(root / "HAL/LoopZAudio.driver/Contents/MacOS/LoopZAudio"),
            str(root / "events.jsonl"),
        ),
    )
    definition.definition.write_bytes(plistlib.dumps(definition.plist()))
    policy, job, driver, inspector = private_policy(root)
    job_lifecycle = BrokerLifecycle(job, lambda: admission(root))
    cleaned = False
    try:
        job_lifecycle.ensure_started(job.inventory(policy.program))
        wait_for(lambda: any(event["version"] == 1 for event in events(root)))
        originals = {name: files(path) for name, path in targets.items()}
        inodes = {name: path.stat().st_ino for name, path in targets.items()}
        with (
            SuiteFiles(root / "support", targets) as transaction,
            DriverRecord(root / "support", driver) as record,
        ):
            transaction.prepare(root / "source")
            images = inspector.images(transaction)
            initial = wait_for(lambda: inspector.observe(images))
            assert len(initial) == 1 and initial[0]["images"] == ["original"]
            record.begin(transaction.value["token"], initial)
            mixed = None
            if mixed_first:
                request = root / "events.jsonl.load"
                request.write_text(str(images["replacement"]["path"] / "Contents/MacOS/LoopZAudio"))
                wait_for(
                    lambda: any(
                        event["event"] == "loaded" and event["version"] == 2
                        for event in events(root)
                    )
                )
                mixed = inspector.observe(images)
                assert mixed[0]["images"] == ["original", "replacement"]
                record.observe(mixed)
                (root / "events.jsonl.unload").write_text("unload only the fixture's extra image")
                wait_for(lambda: any(event["event"] == "unloaded" for event in events(root)))
                after_unload = inspector.observe(images)
                lifecycle = DriverLifecycle(
                    inspector,
                    record,
                    lambda: admission(root),
                    lambda: job.job(),
                    signaller=signaller(root, inspector, images),
                )
                pre_restored = lifecycle.ensure_image("original", images)
                assert not same_generation(pre_restored["host"]["process"], initial[0]["process"])
            transaction.apply()
            images = inspector.images(transaction)
            if crash_at:
                worker = subprocess.run(
                    [sys.executable, "-E", "-s", "-S", __file__, "--worker", str(root), crash_at],
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                assert worker.returncode == 77, worker.stdout + worker.stderr
                # Refresh from the durable record; never reuse pre-worker state.
                record.load()
            lifecycle = DriverLifecycle(
                inspector,
                record,
                lambda: admission(root),
                lambda: job.job(),
                signaller=signaller(root, inspector, images),
            )
            forward = lifecycle.ensure_image("replacement", images)
            wait_for(
                lambda: any(
                    event["pid"] == forward["host"]["process"]["pid"] and event["version"] == 2
                    for event in events(root)
                )
            )
            token = transaction.value["token"]
        # A reconstructed recovery owner restores files before replacing the
        # process still holding the new dylib. Both inodes remain available.
        with (
            SuiteFiles(root / "support", targets) as transaction,
            DriverRecord(root / "support", driver) as record,
        ):
            transaction.load()
            record.bind(token)
            transaction.rollback()
            images = inspector.images(transaction)
            still_new = inspector.observe(images)
            assert still_new[0]["images"] == ["replacement"]
            lifecycle = DriverLifecycle(
                inspector,
                record,
                lambda: admission(root),
                lambda: job.job(),
                signaller=signaller(root, inspector, images),
            )
            restored = lifecycle.ensure_image("original", images)
            wait_for(
                lambda: any(
                    event["pid"] == restored["host"]["process"]["pid"] and event["version"] == 1
                    for event in events(root)
                )
            )
            assert not same_generation(restored["host"]["process"], forward["host"]["process"])
            assert {name: files(path) for name, path in targets.items()} == originals
            assert {name: path.stat().st_ino for name, path in targets.items()} == inodes
            history = record.load()
            transaction.finish()
            record.complete(token)
        current = job.inspect(expected=job.inventory(policy.program))
        job_lifecycle.stop(current)
        assert not list((root / "support").iterdir())
        assert BrokerInspector().job() == production
        requests = [json.loads(line) for line in (root / "signals.jsonl").read_text().splitlines()]
        assert len(requests) == (3 if mixed_first else 2)
        cleaned = True
        return {
            "status": "passed",
            "mode": "private-loaded-image-driver-recovery",
            "crashAt": crash_at,
            "mixedFirst": mixed_first,
            "initial": initial,
            "forward": forward,
            "restored": restored,
            "mixedImages": mixed,
            "imagesAfterUnloadingExtra": after_unload if mixed_first else None,
            "signalRequests": requests,
            "durableHistory": history,
            "versionEvents": events(root),
            "fourOriginalFileInodesRestored": True,
            "productionBrokerJobUnchanged": production,
            "realAppleHostSignalled": False,
            "actualHALOpened": False,
            "audioDevicesOpened": False,
            "permissionsRequested": False,
        }
    finally:
        result = command("/bin/launchctl", "print", policy.target, check=False)
        if result.returncode == 0:
            if (
                f"\tprogram = {policy.program}\n" not in result.stdout
                or f"\tpath = {policy.definition}\n" not in result.stdout
            ):
                raise RuntimeError("Unverified private job retained: " + str(root))
            command("/bin/launchctl", "bootout", policy.target)
        elif (
            result.returncode not in (3, 113)
            or "Could not find service" not in result.stdout + result.stderr
        ):
            raise RuntimeError("Private driver cleanup state is unknown: " + str(root))

        def gone():
            state = command("/bin/launchctl", "print", policy.target, check=False)
            return (
                state.returncode in (3, 113)
                and "Could not find service" in state.stdout + state.stderr
            )

        wait_for(gone)
        if cleaned:
            shutil.rmtree(root)
        else:
            print("Private failure workspace retained: " + str(root), file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--worker":
        abrupt_worker(Path(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) in (4, 5, 6):
        arguments = sys.argv[4:]
        crash = arguments[1] if len(arguments) == 2 and arguments[0] == "--crash-at" else None
        mixed = arguments == ["--mixed-first"]
        if arguments and not (crash or mixed):
            raise SystemExit("Unsupported private fixture option")
        print(
            json.dumps(
                run(
                    *map(lambda path: Path(path).resolve(), sys.argv[1:4]),
                    crash_at=crash,
                    mixed_first=mixed,
                ),
                indent=2,
            )
        )
    else:
        raise SystemExit(
            "usage: driver_process_tests.py <host> <image-1> <image-2> [--crash-at <boundary> | --mixed-first]"
        )
