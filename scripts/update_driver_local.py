#!/usr/bin/env python3
"""OS-specific isolated-driver reload experiment. Never resets Core Audio."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import driver_host
import install_local as installer
from install_transaction import (
    NAMES,
    FileTransaction,
    files,
    check_ancestors,
    exclusive_lock,
    validate_after,
    blocked_signals,
)


def snapshot(checker):
    return json.loads(installer.command(checker, "--snapshot").stdout)


def bridge(checker):
    return json.loads(installer.command(checker, "--bridge-status").stdout)


def experiment(stage, work, recovery_probe=False, force=False):
    checker = stage / "loopz-install-check"
    driver = installer.TARGETS["LoopZAudio.driver"]
    binary = driver / "Contents/MacOS/LoopZAudio"
    before = snapshot(checker)
    driver_host.require_idle(before)
    original_host = driver_host.discover(binary)
    original_service = installer.service_identity()
    helper = installer.TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine"
    old_bridge = json.loads(installer.command(helper, "--bridge-status").stdout)
    if old_bridge.get("helperAttached"):
        raise RuntimeError("Stop LoopZ capture before a driver update")
    original_files = {name: files(installer.TARGETS[name]) for name in NAMES}
    names = [
        name
        for name in NAMES
        if files(stage / name) != original_files[name]
        and (not recovery_probe or name != "LoopZAudio.driver")
    ]
    if not recovery_probe and "LoopZAudio.driver" not in names:
        raise RuntimeError("This experiment requires a changed driver")
    tx = FileTransaction(stage, installer.TARGETS, names)
    report, stop = work / "guard.json", work / "guard.stop"
    guard_log = (work / "guard.stderr").open("w")
    guard = subprocess.Popen(
        [
            str(checker),
            "--guard",
            before["defaultOutput"],
            "--report",
            str(report),
            "--stop-file",
            str(stop),
            "--seconds",
            "90",
        ],
        stdout=subprocess.DEVNULL,
        stderr=guard_log,
    )
    result = {
        "status": "failed",
        "mode": "isolated-host-recovery-probe" if recovery_probe else "isolated-driver-experiment",
        "forceIsolatedHost": force,
        "before": before,
        "originalHost": original_host,
        "coreAudioBefore": original_service,
    }
    touched = False
    applied = False
    signalled = False

    def guard_alive():
        if guard.poll() is not None:
            raise RuntimeError("Independent hardware continuity guard stopped")

    def verify_activated(expected_timing, expected_period, excluded_pid):
        deadline = time.monotonic() + 25
        last = None
        while time.monotonic() < deadline:
            guard_alive()
            try:
                state = snapshot(checker)
                peer = bridge(checker)
                pid = peer.get("driverPID", 0)
                if (
                    not pid
                    or pid == excluded_pid
                    or not peer.get("driverAttached")
                    or peer.get("helperAttached")
                ):
                    raise RuntimeError("Replacement driver has not attached")
                host = driver_host.inspect(pid, binary)
                if any(
                    state["driverTiming"].get(key) != value
                    for key, value in expected_timing.items()
                ):
                    raise RuntimeError("Loaded driver build mismatch")
                if state["clockPeriodFrames"] != expected_period:
                    raise RuntimeError("Loaded driver clock period mismatch")
                validate_after(before, state, original_service, installer.service_identity())
                return state, peer, host
            except Exception as error:
                last = error
            time.sleep(0.25)
        raise RuntimeError("Isolated driver did not reactivate: " + str(last))

    try:
        for attempt in range(100):
            guard_alive()
            if report.exists() and json.loads(report.read_text()).get("state") == "running":
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Hardware continuity guard did not start")
        tx.prepare()
        driver_host.require_idle(snapshot(checker))
        if driver_host.inspect(original_host["pid"], binary) != original_host:
            raise RuntimeError("Original host changed before promotion")
        touched = True
        installer.stop_broker()
        applied = True
        tx.apply()
        installer.start_broker()
        # Authenticate the resident host through the newly pinned root checker.
        deadline = time.monotonic() + 15
        peer = {}
        while time.monotonic() < deadline:
            try:
                peer = bridge(checker)
            except Exception:
                pass
            if (
                peer.get("driverPID") == original_host["pid"]
                and peer.get("driverAttached")
                and not peer.get("helperAttached")
            ):
                break
            time.sleep(0.25)
        else:
            raise RuntimeError("Resident host did not authenticate before targeted reload")
        old_binary = (
            binary
            if recovery_probe
            else tx.backups["LoopZAudio.driver"] / "Contents/MacOS/LoopZAudio"
        )
        allowed = (
            {driver.name} if recovery_probe else {driver.name, tx.backups["LoopZAudio.driver"].name}
        )
        audited = driver_host.inspect(original_host["pid"], old_binary, allowed)
        if any(
            audited[key] != original_host[key]
            for key in [
                "pid",
                "uid",
                "startSeconds",
                "startMicroseconds",
                "executable",
                "device",
                "inode",
            ]
        ):
            raise RuntimeError("Resident host generation changed")
        driver_host.require_idle(snapshot(checker))
        guard_alive()
        if installer.service_identity() != original_service:
            raise RuntimeError("Core Audio changed before targeted reload")
        # No name-based or system-audio signal is ever issued.
        with blocked_signals():
            driver_host.signal_checked(original_host, force=force)
            signalled = True
        expected = (
            before["driverTiming"]
            if recovery_probe
            else {
                "build": "loopz-timing-3",
                "periodFrames": 128,
                "queueFrames": 1024,
                "sourceFrames": 2048,
            }
        )
        period = before["clockPeriodFrames"] if recovery_probe else 128
        after, peer, new_host = verify_activated(expected, period, original_host["pid"])
        if recovery_probe and files(driver) != original_files["LoopZAudio.driver"]:
            raise RuntimeError("Known-good driver changed during recovery probe")
        if after["inputLatencyFrames"] != (
            before["inputLatencyFrames"] if recovery_probe else 3072
        ):
            raise RuntimeError("New timing metadata mismatch")
        stop.touch()
        guard.wait(timeout=5)
        continuity = json.loads(report.read_text())
        if (
            guard.returncode
            or not continuity.get("passed")
            or not continuity.get("stoppedByRequest")
        ):
            raise RuntimeError("Hardware continuity validation failed")
        result.update(
            status="passed",
            after=after,
            bridge=peer,
            newHost=new_host,
            hardwareContinuity=continuity,
            coreAudioAfter=installer.service_identity(),
        )
    except BaseException as error:
        result["error"] = str(error)
        repairs = []
        rollback_host = None
        if signalled:
            try:
                peer = bridge(checker)
                pid = peer.get("driverPID", 0)
                if pid and pid != original_host["pid"]:
                    rollback_host = driver_host.inspect(pid, binary)
                elif pid == original_host["pid"]:
                    driver_host.inspect(
                        pid,
                        (
                            binary
                            if recovery_probe
                            else tx.backups["LoopZAudio.driver"] / "Contents/MacOS/LoopZAudio"
                        ),
                        (
                            {driver.name}
                            if recovery_probe
                            else {driver.name, tx.backups["LoopZAudio.driver"].name}
                        ),
                    )
                else:
                    raise RuntimeError("No authenticated host is available for rollback reload")
            except Exception as issue:
                repairs.append("authenticate rollback host: " + str(issue))
        if touched:
            try:
                installer.stop_broker()
            except Exception as issue:
                repairs.append("stop broker: " + str(issue))
        if applied:
            try:
                tx.rollback()
            except Exception as issue:
                repairs.append("restore files: " + str(issue))
        if touched:
            try:
                installer.start_broker()
            except Exception as issue:
                repairs.append("restore broker: " + str(issue))
        if rollback_host and not recovery_probe:
            try:
                with blocked_signals():
                    driver_host.signal_checked(rollback_host, force=force)
            except Exception as issue:
                repairs.append("reload restored host: " + str(issue))
        try:
            if not all(files(installer.TARGETS[name]) == original_files[name] for name in NAMES):
                raise RuntimeError("Original file hashes were not restored")
            deadline = time.monotonic() + 20
            while True:
                try:
                    restored = snapshot(checker)
                except Exception:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.25)
                    continue
                if (
                    restored.get("clockPeriodFrames") == before["clockPeriodFrames"]
                    and restored.get("driverTiming") == before["driverTiming"]
                ):
                    restored_host = driver_host.discover(binary)
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("Original driver did not reactivate")
                time.sleep(0.25)
            validate_after(before, restored, original_service, installer.service_identity())
            result.update(afterRollback=restored, restoredHost=restored_host, filesRestored=True)
        except Exception as issue:
            repairs.append("verify rollback: " + str(issue))
        result["rolledBack"] = applied and not repairs
        if repairs:
            result.update(
                repairRequired=True,
                rollbackErrors=repairs,
                backupsRetained=[str(p) for p in tx.backups.values() if p.exists()],
            )
    finally:
        stop.touch()
        try:
            guard.wait(timeout=5)
        except subprocess.TimeoutExpired:
            guard.terminate()
            guard.wait(timeout=5)
        guard_log.close()
        if report.exists():
            try:
                result["hardwareContinuity"] = json.loads(report.read_text())
            except Exception as issue:
                result["hardwareContinuityError"] = str(issue)
        if not result.get("repairRequired"):
            tx.finish()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--probe-host-recovery", action="store_true")
    parser.add_argument("--force-isolated-host", action="store_true")
    args = parser.parse_args()
    if not args.experiment:
        parser.error("--experiment is required; no supported reload API is claimed")
    if args.probe_host_recovery and not args.force_isolated_host:
        parser.error("Recovery probe requires explicit --force-isolated-host")
    stage = installer.STAGE
    installer.verify_package(stage)
    check_ancestors(installer.TARGETS.values())
    before = snapshot(stage / "loopz-install-check")
    driver_host.require_idle(before)
    for name in NAMES:
        files(installer.TARGETS[name])
        if name in installer.IDENTITIES:
            installer.verify_code(installer.TARGETS[name], installer.IDENTITIES[name])
    if args.check:
        print(
            json.dumps(
                {
                    "status": "preflight-passed",
                    "before": before,
                    "hostAudit": "requires privileged execution",
                    "activationVerified": False,
                }
            )
        )
        return
    if os.geteuid() != 0:
        parser.error("Administrator authorization is required")
    with exclusive_lock(installer.TARGETS["loopz-broker"].parent / ".install.lock"):
        with tempfile.TemporaryDirectory(prefix="loopz-driver-") as temporary:
            work = Path(temporary)
            private = work / "package"
            private.mkdir()
            for name in [*NAMES, "manifest.json", "com.lexasoft.loopz.engine.plist"]:
                source = stage / name
                if source.is_dir():
                    shutil.copytree(source, private / name, symlinks=True)
                else:
                    shutil.copy2(source, private / name, follow_symlinks=False)
            installer.verify_package(private)
            result = experiment(
                private,
                work,
                recovery_probe=args.probe_host_recovery,
                force=args.force_isolated_host,
            )
            destination = installer.TARGETS["loopz-broker"].parent / "driver-update-result.json"
            destination.write_text(json.dumps(result, indent=2) + "\n")
            os.chmod(destination, 0o644)
            print(json.dumps(result), flush=True)
            if result["status"] != "passed":
                raise SystemExit(1)


def interrupted(signum, frame):
    signal.signal(signum, signal.SIG_IGN)
    raise KeyboardInterrupt("Driver update interrupted; restoring original components")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    main()
