#!/usr/bin/env python3
"""Install/update LoopZ transactionally. Never restart a system audio service."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import tempfile
import time
from install_transaction import (
    NAMES,
    FileTransaction,
    compatible_driver,
    files,
    validate_after,
    verify_manifest,
    check_ancestors,
    exclusive_lock,
)

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "artifacts/feasibility"
TARGETS = {
    "LoopZAudio.driver": Path("/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver"),
    "LoopZ Engine.app": Path("/Applications/LoopZ Engine.app"),
    "loopz-broker": Path("/Library/Application Support/LoopZ/loopz-broker"),
    "loopz-probe": Path("/Library/Application Support/LoopZ/loopz-probe"),
    "loopz-install-check": Path("/Library/Application Support/LoopZ/loopz-install-check"),
    "com.lexasoft.loopz.bridge.plist": Path(
        "/Library/LaunchDaemons/com.lexasoft.loopz.bridge.plist"
    ),
}
IDENTITIES = {
    "LoopZAudio.driver": "com.lexasoft.loopz.driver",
    "LoopZ Engine.app": "com.lexasoft.loopz.engine",
    "loopz-broker": "com.lexasoft.loopz.broker",
    "loopz-probe": "com.lexasoft.loopz.probe",
    "loopz-install-check": "com.lexasoft.loopz.install-check",
}
LABEL = "system/com.lexasoft.loopz.bridge"


def command(*arguments, check=True, timeout=15):
    return subprocess.run(
        list(map(str, arguments)), capture_output=True, text=True, check=check, timeout=timeout
    )


def verify_code(path, identifier):
    command("/usr/bin/codesign", "--verify", "--strict", path)
    details = command("/usr/bin/codesign", "-d", "--verbose=4", path).stderr
    if f"Identifier={identifier}\n" not in details:
        raise ValueError(f"Unexpected signed identity: {path}")
    return details


def verify_package(stage):
    manifest = json.loads((stage / "manifest.json").read_text())
    verify_manifest(stage, manifest)
    details = {}
    for name, identifier in IDENTITIES.items():
        details[name] = verify_code(stage / name, identifier)
    match = re.search(r"^CDHash=([a-f0-9]+)$", details["LoopZ Engine.app"], re.M)
    if not match:
        raise ValueError("Cannot read helper signature")
    requirement = f'identifier "com.lexasoft.loopz.engine" and cdhash H"{match[1]}"'
    check_hash = re.search(r"^CDHash=([a-f0-9]+)$", details["loopz-install-check"], re.M)
    if not check_hash:
        raise ValueError("Cannot read checker signature")
    control_requirement = (
        f'identifier "com.lexasoft.loopz.install-check" and cdhash H"{check_hash[1]}"'
    )
    config = plistlib.loads((stage / NAMES[-1]).read_bytes())
    if (
        manifest.get("engineRequirement") != requirement
        or manifest.get("controlRequirement") != control_requirement
        or config.get("Label") != "com.lexasoft.loopz.bridge"
        or config.get("ProgramArguments")
        != [
            str(TARGETS["loopz-broker"]),
            "--engine-requirement",
            requirement,
            "--control-requirement",
            control_requirement,
        ]
        or config.get("MachServices") != {"com.lexasoft.loopz.bridge": True}
    ):
        raise ValueError("Broker configuration does not match the signed helper")
    return manifest


def preflight(stage, replace, tools_only):
    check_ancestors(TARGETS.values())
    existing = [name for name, target in TARGETS.items() if target.exists() or target.is_symlink()]
    if existing and not replace:
        raise ValueError("LoopZ is already installed; use --replace for a verified hot update")
    for name in existing:
        target = TARGETS[name]
        files(target)  # no symlinks anywhere in an installed component
        if name in IDENTITIES:
            verify_code(target, IDENTITIES[name])
        for parent in [target, *target.parents]:
            if parent.is_symlink():
                raise ValueError(f"Symlink in installation destination: {parent}")
    driver = TARGETS["LoopZAudio.driver"]
    if driver.exists() and not compatible_driver(stage / driver.name, driver):
        raise ValueError(
            "Resident driver differs from this package. Live driver-binary replacement is not supported; no files or audio services were changed."
        )
    if tools_only and files(stage / "LoopZ Engine.app") != files(TARGETS["LoopZ Engine.app"]):
        raise ValueError("--update-tools requires an identical installed helper")
    names = [name for name in NAMES if name != driver.name or not driver.exists()]
    if tools_only:
        if (stage / NAMES[-1]).read_bytes() != TARGETS[NAMES[-1]].read_bytes():
            raise ValueError("--update-tools requires an identical broker configuration")
        names = ["loopz-broker", "loopz-probe", "loopz-install-check"]
    return names, bool(existing)


def service_identity():
    lines = command("/bin/ps", "-axo", "pid=,lstart=,comm=").stdout.splitlines()
    identity = [line.strip() for line in lines if line.split()[-1:] == ["/usr/sbin/coreaudiod"]]
    if len(identity) != 1:
        raise RuntimeError("Cannot establish a unique Core Audio service identity")
    return identity[0]


def daemon_loaded():
    return command("/bin/launchctl", "print", LABEL, check=False).returncode == 0


def stop_broker():
    if daemon_loaded():
        command("/bin/launchctl", "bootout", LABEL)


def start_broker():
    command("/bin/launchctl", "bootstrap", "system", TARGETS[NAMES[-1]])


def snapshot(checker):
    return json.loads(command(checker, "--snapshot").stdout)


def update(stage, work, names, guard_uid):
    checker = stage / "loopz-install-check"
    before = snapshot(checker)
    if before["loopzRunning"]:
        raise RuntimeError(
            "LoopZ is in use. Stop its source and recording clients before updating. Other sound devices can stay active."
        )
    helper_path = str(TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine")
    if any(
        line.strip().startswith(helper_path)
        for line in command("/bin/ps", "-axo", "command=").stdout.splitlines()
    ):
        raise RuntimeError(
            "Stop the LoopZ engine before updating; its capture settings will not be changed by the installer"
        )
    identity_before = service_identity()
    resident = TARGETS["LoopZAudio.driver"]
    driver_inode = resident.stat().st_ino if resident.exists() else None
    driver_hashes = files(resident)
    report, stop = work / "guard.json", work / "guard.stop"
    was_loaded = daemon_loaded()
    original_files = {name: files(TARGETS[name]) for name in names}
    guard_log = (work / "guard.stderr").open("w")
    guard = subprocess.Popen(
        [
            str(checker),
            "--guard",
            guard_uid or before["defaultOutput"],
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
    transaction = FileTransaction(stage, TARGETS, names)
    broker_touched, applied, verified = False, False, False
    result = {
        "status": "failed",
        "mode": "hot-update" if "LoopZAudio.driver" not in names else "fresh-install",
        "driverPreserved": "LoopZAudio.driver" not in names,
        "before": before,
        "coreAudioBefore": identity_before,
    }
    try:
        for attempt in range(100):
            if guard.poll() is not None:
                raise RuntimeError(
                    "Hardware continuity guard failed: " + (work / "guard.stderr").read_text()
                )
            if report.exists() and json.loads(report.read_text()).get("state") == "running":
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Hardware continuity guard did not start")
        transaction.prepare()
        if guard.poll() is not None:
            raise RuntimeError("Continuity guard ended before promotion")
        broker_touched = True
        stop_broker()
        applied = True
        transaction.apply()
        start_broker()
        # Driver mappings reconnect on their control timer; no Core Audio reset.
        deadline = time.monotonic() + 20
        after, bridge = {}, {}
        while time.monotonic() < deadline:
            after = snapshot(checker)
            state = command(helper_path, "--bridge-status", check=False, timeout=8)
            if not state.returncode:
                bridge = json.loads(state.stdout)
            if (
                after.get("loopzVisible")
                and after.get("loopzAlive")
                and bridge.get("driverAttached")
                and bridge.get("protocol") == 1
            ):
                break
            time.sleep(0.25)
        else:
            raise RuntimeError(
                "LoopZ did not activate and attach within 20 seconds. No restart was attempted."
            )
        identity_after = service_identity()
        validate_after(before, after, identity_before, identity_after)
        if driver_inode is not None and (
            resident.stat().st_ino != driver_inode or files(resident) != driver_hashes
        ):
            raise RuntimeError("Resident driver changed during the update")
        if guard.poll() is not None:
            raise RuntimeError("Continuity guard ended before validation completed")
        stop.touch()
        guard.wait(timeout=5)
        continuity = json.loads(report.read_text())
        if (
            guard.returncode
            or not continuity.get("passed")
            or not continuity.get("stoppedByRequest")
        ):
            raise RuntimeError("Unrelated hardware audio continuity was not preserved")
        verified = True
        result.update(
            status="passed",
            after=after,
            bridge=bridge,
            hardwareContinuity=continuity,
            coreAudioAfter=identity_after,
            driverInodeBefore=driver_inode,
            driverInodeAfter=resident.stat().st_ino,
        )
    except BaseException as error:
        result["status"] = "failed"
        result["error"] = str(error)
        repair_errors = []
        if broker_touched:
            try:
                stop_broker()
            except Exception as problem:
                repair_errors.append(f"stop new broker: {problem}")
        if applied:
            try:
                transaction.rollback()
                result["filesRestored"] = all(
                    files(TARGETS[name]) == original_files[name] for name in names
                )
                if not result["filesRestored"]:
                    raise RuntimeError("Restored files do not match the original hashes")
            except Exception as problem:
                repair_errors.append(f"restore files: {problem}")
        if broker_touched and was_loaded:
            try:
                start_broker()
            except Exception as problem:
                repair_errors.append(f"restart old broker: {problem}")
        try:
            restored = snapshot(checker)
            result["afterRollback"] = restored
            result["coreAudioAfterRollback"] = service_identity()
            result["residualActivation"] = not before["loopzVisible"] and restored["loopzVisible"]
            result["defaultsPreservedAfterRollback"] = all(
                before[key] == restored[key]
                for key in ("defaultInput", "defaultOutput", "defaultSystemOutput")
            )
            if (
                result["residualActivation"]
                or not result["defaultsPreservedAfterRollback"]
                or result["coreAudioAfterRollback"] != identity_before
            ):
                repair_errors.append(
                    "Runtime state is not fully restored; no system audio reset was attempted"
                )
        except Exception as problem:
            repair_errors.append(f"verify rollback runtime: {problem}")
        result["rolledBack"] = applied and result.get("filesRestored", False) and not repair_errors
        if repair_errors:
            result["repairRequired"] = True
            result["rollbackError"] = repair_errors
            result["backupsRetained"] = [
                str(path) for path in transaction.backups.values() if path.exists()
            ]
        raise RuntimeError(json.dumps(result)) from error
    finally:
        stop.touch()
        try:
            guard.wait(timeout=5)
        except subprocess.TimeoutExpired:
            guard.terminate()
            try:
                guard.wait(timeout=3)
            except subprocess.TimeoutExpired:
                guard.kill()
                guard.wait(timeout=3)
        guard_log.close()
        if "rollbackError" not in result:
            transaction.finish()
        if not verified and report.exists():
            result["hardwareContinuity"] = json.loads(report.read_text())
        print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--replace",
        action="store_true",
        help="Hot-update an existing installation; preserve its identical resident driver",
    )
    group.add_argument("--update-tools", action="store_true")
    group.add_argument(
        "--update-engine", action="store_true", help="Alias for a verified hot update"
    )
    parser.add_argument(
        "--check", action="store_true", help="Validate package and compatibility without installing"
    )
    parser.add_argument(
        "--guard-uid",
        help="Physical output UID for the silent continuity guard; default is current output",
    )
    args = parser.parse_args()
    if not args.check and os.geteuid() != 0:
        parser.error("Administrator privileges are required to install")
    verify_package(STAGE)
    replace = args.replace or args.update_tools or args.update_engine
    if args.check:
        names, existing = preflight(STAGE, replace, args.update_tools)
        print(
            json.dumps(
                {
                    "status": "ready",
                    "existingInstallation": existing,
                    "replace": names,
                    "driverPreserved": "LoopZAudio.driver" not in names,
                    "activationVerified": False,
                }
            )
        )
        return
    check_ancestors(TARGETS.values())
    with exclusive_lock(TARGETS["loopz-broker"].parent / ".install.lock"):
        # Work from a private verified snapshot, never from mutable user-owned
        # staging paths once privileged replacement starts.
        with tempfile.TemporaryDirectory(prefix="loopz-install-") as temporary:
            work = Path(temporary)
            private = work / "package"
            private.mkdir()
            for name in [*NAMES, "manifest.json", "com.lexasoft.loopz.engine.plist"]:
                source = STAGE / name
                if source.is_dir():
                    shutil.copytree(source, private / name, symlinks=True)
                else:
                    shutil.copy2(source, private / name, follow_symlinks=False)
            manifest = verify_package(private)
            names, _ = preflight(private, replace, args.update_tools)
            result = update(private, work, names, args.guard_uid)
            result["packageHashes"] = manifest["sha256"]
            destination = TARGETS["loopz-broker"].parent / "install-result.json"
            destination.write_text(json.dumps(result, indent=2) + "\n")
            os.chmod(destination, 0o644)


if __name__ == "__main__":

    def interrupted(signum, frame):
        raise KeyboardInterrupt("Installation interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except Exception as error:
        raise SystemExit(str(error))
