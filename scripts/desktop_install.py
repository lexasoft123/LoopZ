"""Signed desktop-pair verification and file-only deployment transaction."""

import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat

from desktop_files import DesktopFileTransaction
from install_transaction import check_ancestors, files

TEAM = "USJ7H3X44X"
NAMES = ("LoopZ.app", "LoopZ Engine.app")
TARGETS = {name: Path("/Applications") / name for name in NAMES}
IDENTIFIERS = {
    "LoopZ.app/Contents/MacOS/LoopZLogin": "com.lexasoft.loopz.login",
    "LoopZ.app": "com.lexasoft.loopz.editor",
    "LoopZ Engine.app": "com.lexasoft.loopz.engine",
}
LOGIN_AGENT = {
    "Label": "com.lexasoft.loopz.login",
    "BundleProgram": "Contents/MacOS/LoopZLogin",
    "RunAtLoad": True,
    "KeepAlive": False,
    "ProcessType": "Background",
}


def requirement(identifier):
    return (
        f'identifier "{identifier}" and anchor apple generic '
        "and certificate 1[field.1.2.840.113635.100.6.2.6] exists "
        "and certificate leaf[field.1.2.840.113635.100.6.1.13] exists "
        f'and certificate leaf[subject.OU] = "{TEAM}"'
    )


def validate_runtime_capabilities(value):
    if (
        not isinstance(value, dict)
        or type(value.get("version")) is not int
        or value["version"] != 1
        or type(value.get("backgroundSession")) is not int
        or value["backgroundSession"] != 1
        or value.get("routingDocument") is not True
        or value.get("editorService") is not True
    ):
        raise RuntimeError("Installed Engine does not support the background desktop")
    return value


def bounded_files(folder):
    check_ancestors([folder])
    total = 0
    for count, entry in enumerate(Path(folder).rglob("*"), 1):
        if count > 1024:
            raise ValueError("Desktop package contains too many entries")
        info = entry.lstat()
        if stat.S_ISDIR(info.st_mode):
            continue
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError("Desktop package requires regular files without links")
        total += info.st_size
        if info.st_size > 64 * 1024 * 1024 or total > 256 * 1024 * 1024:
            raise ValueError("Desktop package exceeds its size limit")
    return files(folder)


def verify_package(package, verify_code, expected_manifest=None):
    package = Path(package)
    actual = bounded_files(package)
    encoded = (package / "manifest.json").read_bytes()
    if len(encoded) > 1024 * 1024:
        raise ValueError("Desktop manifest is too large")
    digest = hashlib.sha256(encoded).hexdigest()
    if expected_manifest and digest != expected_manifest:
        raise ValueError("Desktop package changed after review")
    manifest = json.loads(encoded)
    if not isinstance(manifest, dict):
        raise ValueError("Desktop manifest must be an object")
    if (
        type(manifest.get("schema")) is not int
        or manifest["schema"] != 1
        or manifest.get("kind") != "desktop-review"
        or manifest.get("buildConfiguration") != "Release"
        or manifest.get("installed") is not False
        or manifest.get("loginRegistered") is not False
    ):
        raise ValueError("Deployment requires a staged, optimized desktop pair")
    expected_identities = {
        name: {"identifier": identifier, "team": TEAM, "hardenedRuntime": True}
        for name, identifier in IDENTIFIERS.items()
    }
    identities = manifest.get("identities")
    if identities != expected_identities or any(
        value.get("hardenedRuntime") is not True for value in identities.values()
    ):
        raise ValueError("Desktop component identities differ from the fixed publisher policy")
    hashes = manifest.get("files")
    if not isinstance(hashes, dict) or not hashes or len(hashes) > 512:
        raise ValueError("Invalid desktop file manifest")
    for name, value in hashes.items():
        relative = PurePosixPath(name)
        if (
            relative.is_absolute()
            or not relative.parts
            or relative.parts[0] not in NAMES
            or ".." in relative.parts
            or str(relative) != name
            or not isinstance(value, str)
            or not re.fullmatch(r"[a-f0-9]{64}", value)
        ):
            raise ValueError("Unsupported desktop package path or digest")
    actual.pop("manifest.json", None)
    if actual != hashes:
        raise ValueError("Desktop files differ from the complete manifest")
    for name, identifier in IDENTIFIERS.items():
        verify_code(package / name, identifier, requirement(identifier), name == "LoopZ Engine.app")
    if not (package / "LoopZ.app/Contents/MacOS/LoopZLogin").stat().st_mode & 0o111:
        raise ValueError("The login helper is not executable")
    for name, executable in (("LoopZ.app", "LoopZ"), ("LoopZ Engine.app", "LoopZ Engine")):
        info = plistlib.loads((package / name / "Contents/Info.plist").read_bytes())
        if (
            info.get("CFBundleIdentifier") != IDENTIFIERS[name]
            or info.get("CFBundleExecutable") != executable
        ):
            raise ValueError("Desktop bundle identity or executable is invalid")
        if not (package / name / "Contents/MacOS" / executable).stat().st_mode & 0o111:
            raise ValueError("A desktop executable is not executable")
    agent = plistlib.loads(
        (
            package / "LoopZ.app/Contents/Library/LaunchAgents/com.lexasoft.loopz.login.plist"
        ).read_bytes()
    )
    if (
        agent != LOGIN_AGENT
        or agent.get("RunAtLoad") is not True
        or agent.get("KeepAlive") is not False
    ):
        raise ValueError("The login helper must remain an explicitly registered one-shot agent")
    return manifest, digest


def deploy(
    package,
    targets,
    host,
    work,
    transaction_factory=DesktopFileTransaction,
    *,
    names=NAMES,
    driver_transition=None,
):
    before = host.snapshot()
    host.require_idle(before)
    identity = host.service_identity()
    protected = host.protected_state()
    originals = {name: files(targets[name]) for name in names}
    if driver_transition:
        driver_transition.begin(before, identity)
    transaction = transaction_factory(package, targets, names)
    guard = host.start_guard(work, before)
    result = {
        "status": "failed",
        "mode": "desktop-driver-update" if driver_transition else "desktop-pair-deployment",
        "before": before,
        "coreAudioBefore": identity,
        "driverPreserved": None,
        "brokerPreserved": None,
        "loginRegistered": None,
        "userDocumentModified": None,
        "driverTouchedByInstaller": False,
        "brokerTouchedByInstaller": False,
        "loginRegisteredByInstaller": False,
    }
    applied = False

    def preserved():
        after = host.snapshot()
        if any(
            before.get(key) != after.get(key)
            for key in ("defaultInput", "defaultOutput", "defaultSystemOutput", "routingDevices")
        ):
            raise RuntimeError("Audio defaults or the device catalog changed during deployment")
        if host.service_identity() != identity:
            raise RuntimeError("Core Audio process identity changed")
        if host.protected_state() != protected:
            raise RuntimeError("The driver, broker or user configuration changed")
        result.update(
            driverPreserved=driver_transition is None,
            brokerPreserved=True,
            userDocumentModified=False,
        )
        return after

    try:
        guard.ready()
        transaction.prepare()
        host.require_idle(host.snapshot())
        guard.check()
        applied = True
        transaction.apply()
        if any(files(targets[name]) != files(Path(package) / name) for name in names):
            raise RuntimeError("Installed desktop apps differ from the verified package")
        if driver_transition:
            result["driverTouchedByInstaller"] = True
        result["capabilities"] = host.validate_apps()
        result["loginRegistered"] = False
        if driver_transition:
            driver_transition.activate(transaction, guard)
        result["activation"] = host.validate_activation(work)
        if driver_transition:
            result["driverTransition"] = driver_transition.validate(result["activation"])
        host.cleanup_validation()
        result["after"] = preserved()
        guard.check()
        result["hardwareContinuity"] = guard.stop()
        if not result["hardwareContinuity"].get("passed"):
            raise RuntimeError("Physical output continuity failed")
        result.update(status="passed", coreAudioAfter=identity)
    except BaseException as error:
        result["error"] = str(error)
        repair = []
        try:
            result["activationDiagnostics"] = host.activation_diagnostics()
        except BaseException as failure:
            result["diagnosticError"] = str(failure)
        try:
            host.cleanup_validation()
        except BaseException as failure:
            repair.append("withdraw validation: " + str(failure))
        if applied:
            if driver_transition:
                result["residualDriverImagePossible"] = True
            if driver_transition:
                try:
                    driver_transition.prepare_rollback(transaction)
                except BaseException as failure:
                    repair.append("inspect driver before rollback: " + str(failure))
            try:
                transaction.rollback()
                if any(files(targets[name]) != value for name, value in originals.items()):
                    raise RuntimeError("Original application files were not restored")
                result["filesRestored"] = True
            except BaseException as failure:
                repair.append("restore applications: " + str(failure))
            if driver_transition and result.get("filesRestored"):
                try:
                    result["driverRollback"] = driver_transition.restore(work)
                    result["residualDriverImagePossible"] = False
                except BaseException as failure:
                    repair.append("restore driver runtime: " + str(failure))
        try:
            result["afterRollback"] = preserved()
        except BaseException as failure:
            repair.append("verify protected state: " + str(failure))
        try:
            guard.check()
            result["rollbackHardwareContinuity"] = guard.stop()
        except BaseException as failure:
            repair.append("verify physical continuity: " + str(failure))
        result["rolledBack"] = not repair
        if repair:
            result["repairRequired"] = repair
    finally:
        try:
            guard.close()
        except BaseException as failure:
            result["guardCleanupError"] = str(failure)
            result["status"] = "failed"
            result.setdefault("repairRequired", []).append("close continuity guard")
        if not result.get("repairRequired"):
            try:
                if result["status"] == "passed":
                    commit = getattr(transaction, "commit", None)
                    if commit is not None:
                        commit()
                transaction.finish()
            except BaseException as failure:
                result["status"] = "failed"
                result.setdefault("repairRequired", []).append(
                    "remove deployment recovery files: " + str(failure)
                )
        if result.get("repairRequired"):
            result["retainedRecoveryFiles"] = [
                str(path)
                for path in [*transaction.candidates.values(), *transaction.backups.values()]
                if path.exists()
            ]
        close = getattr(transaction, "close", None)
        if close is not None:
            close()
    return result
