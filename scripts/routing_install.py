"""Verified routing-package and guarded initial-activation transaction primitives."""

import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib

from install_transaction import FileTransaction, check_ancestors, compatible_driver, files
from prepare_routing_install import COMPONENTS, SERVICE, stable_requirement, component_identities

TEAM = "USJ7H3X44X"
SUPPORT = Path("/Library/Application Support/LoopZ")
DAEMON = "com.lexasoft.loopz.routing.plist"
AGENT = "com.lexasoft.loopz.engine.plist"
NAMES = (*COMPONENTS, "routing-service.json", DAEMON, AGENT)
TARGETS = {name: SUPPORT / name for name in NAMES}
TARGETS.update(
    {
        "LoopZ Engine.app": Path("/Applications/LoopZ Engine.app"),
        "LoopZRoutingAudio.driver": Path("/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver"),
        "loopz-probe": SUPPORT / "routing/loopz-probe",
        DAEMON: Path("/Library/LaunchDaemons") / DAEMON,
        AGENT: SUPPORT / "routing" / AGENT,
    }
)


def verify_package(stage, verify_code, expected_manifest=None, *, legacy_slot=False):
    stage = Path(stage)
    manifest_bytes = (stage / "manifest.json").read_bytes()
    digest = hashlib.sha256(manifest_bytes).hexdigest()
    if expected_manifest and digest != expected_manifest:
        raise ValueError("Routing manifest changed after review")
    if len(manifest_bytes) > 1024 * 1024:
        raise ValueError("Routing manifest is too large")
    manifest = json.loads(manifest_bytes)
    for field, expected in {
        "schema": 1,
        "audioMappingProtocol": 2,
        "snapshotSchema": 3,
        "serviceEnvelopeProtocol": 2,
    }.items():
        if type(manifest.get(field)) is not int or manifest[field] != expected:
            raise ValueError("Unsupported routing package protocol")
    kind = "loopz-routing-migration" if legacy_slot else "loopz-routing-install"
    if manifest.get("kind") != kind:
        raise ValueError("This is not a routing-runtime package")
    actual = files(stage)
    actual.pop("manifest.json", None)
    hashes = manifest.get("files")
    if not isinstance(hashes, dict) or not hashes or len(hashes) > 512:
        raise ValueError("Invalid routing manifest file list")
    for name, value in hashes.items():
        path = PurePosixPath(name)
        if (
            path.is_absolute()
            or ".." in path.parts
            or not path.parts
            or path.parts[0] not in NAMES
            or not isinstance(value, str)
            or len(value) != 64
        ):
            raise ValueError("Unsupported routing installation path or hash")
    if hashes != actual:
        raise ValueError("Routing payload differs from its complete manifest")
    components = component_identities(legacy_slot)
    if set(manifest.get("signatures", {})) != set(components):
        raise ValueError("Incomplete routing signature identities")
    for name, identifier in components.items():
        requirement = stable_requirement(identifier, TEAM)
        if manifest["signatures"][name] != {
            "identifier": identifier,
            "team": TEAM,
            "requirement": requirement,
        }:
            raise ValueError("Routing package has an unexpected signing identity")
        verify_code(stage / name, identifier, requirement, name == "LoopZ Engine.app")
    pin_bytes = (stage / "routing-service.json").read_bytes()
    if (
        pin_bytes
        != (stage / "LoopZRoutingAudio.driver/Contents/Resources/routing-service.json").read_bytes()
    ):
        raise ValueError("Helper and driver broker trust pins differ")
    if json.loads(pin_bytes) != {
        "protocol": 2,
        "brokerRequirement": stable_requirement(COMPONENTS["loopz-routing-broker"], TEAM),
    }:
        raise ValueError("Routing broker pin does not match the expected publisher")
    document = manifest.get("documentPath")
    if not isinstance(document, str) or not Path(document).is_absolute() or "\0" in document:
        raise ValueError("Invalid routing document location")
    daemon = plistlib.loads((stage / DAEMON).read_bytes())
    expected = {
        "Label": "com.lexasoft.loopz.routing",
        "ProgramArguments": [
            str(TARGETS["loopz-routing-broker"]),
            "--engine-requirement",
            stable_requirement(COMPONENTS["LoopZ Engine.app"], TEAM),
        ],
        "MachServices": {SERVICE: True},
        "KeepAlive": True,
        "RunAtLoad": True,
        "ProcessType": "Background",
        "ThrottleInterval": 3,
    }
    if daemon != expected:
        raise ValueError("Routing root job differs from the fixed signed-component policy")
    agent = plistlib.loads((stage / AGENT).read_bytes())
    if agent != {
        "Label": "com.lexasoft.loopz.engine",
        "KeepAlive": True,
        "RunAtLoad": True,
        "ProcessType": "Interactive",
        "ThrottleInterval": 3,
        "ProgramArguments": [
            str(TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine"),
            "--routing-document",
            document,
        ],
    }:
        raise ValueError("Routing user job differs from its declared document policy")
    info = plistlib.loads((stage / "LoopZRoutingAudio.driver/Contents/Info.plist").read_bytes())
    if info.get("AudioServerPlugIn_MachServices") != [SERVICE]:
        raise ValueError("Routing driver does not declare its broker Mach service")
    if legacy_slot and (
        info.get("CFBundleIdentifier") != "com.lexasoft.loopz.driver"
        or info.get("CFBundleExecutable") != "LoopZAudio"
        or info.get("CFPlugInFactories")
        != {"F2F8AC9D-AE84-4550-955C-E35A027F56B2": "LoopZAudio_Create"}
        or info.get("CFPlugInTypes")
        != {"443ABAB8-E7B3-491A-B985-BEB9187030DB": ["F2F8AC9D-AE84-4550-955C-E35A027F56B2"]}
    ):
        raise ValueError("Migration driver must preserve the existing HAL slot identity")
    return manifest, digest


def preflight(stage, targets, replace_engine, *, legacy_slot=False):
    check_ancestors(targets.values())
    driver = targets["LoopZRoutingAudio.driver"]
    if legacy_slot and (driver.name != "LoopZAudio.driver" or not driver.is_dir()):
        raise ValueError("Existing-slot migration requires the installed legacy LoopZ driver")
    if (
        not legacy_slot
        and driver.exists()
        and not compatible_driver(Path(stage) / driver.name, driver)
    ):
        raise ValueError(
            "Changed routing driver needs a separately verified isolated-host reload; no live replacement is attempted"
        )
    app = targets["LoopZ Engine.app"]
    if app.exists() and files(app) != files(Path(stage) / app.name) and not replace_engine:
        raise ValueError("Replacing the current Engine requires --replace-engine")
    for path in targets.values():
        if path.exists():
            files(path)
    return [name for name in NAMES if legacy_slot or name != driver.name or not driver.exists()]


def unchanged_defaults(before, after):
    for name in ("defaultInput", "defaultOutput", "defaultSystemOutput"):
        if before.get(name) != after.get(name):
            raise RuntimeError("A default audio device changed: " + name)


def activate(stage, targets, names, host, work):
    """No success without native publication, clean withdrawal and continuity.

    This initial migration path requires an idle, empty routing catalog. Active
    production graph update/supervision remains a separate acceptance gate.
    """
    before = host.snapshot()
    host.require_idle(before)
    identity = host.service_identity()
    protected = host.protected_state()
    original = {name: files(targets[name]) for name in names}
    was_loaded = host.daemon_loaded()
    transaction = FileTransaction(stage, targets, names)
    guard = host.start_guard(work, before)
    touched, applied = False, False
    result = {
        "status": "failed",
        "mode": getattr(host, "activation_mode", "initial-routing-activation"),
        "before": before,
        "coreAudioBefore": identity,
        "driverPreserved": "LoopZRoutingAudio.driver" not in names,
        "loginRegistered": False,
        "userDocumentModified": False,
    }
    try:
        guard.ready()
        transaction.prepare()
        host.require_idle(host.snapshot())
        guard.check()
        touched = True
        host.stop_broker()
        applied = True
        transaction.apply()
        if any(files(targets[name]) != files(Path(stage) / name) for name in NAMES):
            raise RuntimeError("Installed routing files differ from the verified snapshot")
        host.start_broker()
        guard.check()
        host.transition_driver(transaction, guard)
        result["activation"] = host.validate_activation(work)
        host.cleanup_validation()
        after = host.snapshot()
        unchanged_defaults(before, after)
        if after.get("routingDevices") != before.get("routingDevices"):
            raise RuntimeError("Validation devices were not withdrawn")
        if host.service_identity() != identity:
            raise RuntimeError("Core Audio process identity changed")
        if host.protected_state() != protected:
            raise RuntimeError("The resident legacy installation changed")
        guard.check()
        result["hardwareContinuity"] = guard.stop()
        if not result["hardwareContinuity"].get("passed"):
            raise RuntimeError("Physical audio continuity check failed")
        result.update(
            status="passed", after=after, coreAudioAfter=identity, legacyInstallationPreserved=True
        )
    except BaseException as error:
        result["error"] = str(error)
        try:
            # Capture evidence before withdrawal removes the temporary helper
            # and rollback removes the broker and newly installed bundle.
            result["activationDiagnostics"] = host.activation_diagnostics()
        except Exception as failure:
            result["diagnosticError"] = str(failure)
        repair = []
        try:
            host.cleanup_validation()
        except Exception as failure:
            repair.append("withdraw validation: " + str(failure))
        if touched:
            try:
                host.stop_broker()
            except Exception as failure:
                repair.append("stop routing broker: " + str(failure))
        if applied:
            try:
                host.prepare_driver_rollback(transaction)
            except Exception as failure:
                repair.append("inspect driver before rollback: " + str(failure))
            try:
                transaction.rollback()
                if any(files(targets[name]) != value for name, value in original.items()):
                    raise RuntimeError("Restored file hashes differ")
                result["filesRestored"] = True
            except Exception as failure:
                repair.append("restore files: " + str(failure))
            try:
                host.restore_driver(transaction)
            except Exception as failure:
                repair.append("restore resident driver: " + str(failure))
        if touched and was_loaded:
            try:
                host.start_broker()
            except Exception as failure:
                repair.append("restore prior routing broker: " + str(failure))
        try:
            after = host.snapshot()
            unchanged_defaults(before, after)
            if (
                after.get("routingDevices") != before.get("routingDevices")
                or host.service_identity() != identity
                or host.protected_state() != protected
            ):
                raise RuntimeError("Audio state differs after rollback")
            result["afterRollback"] = after
        except Exception as failure:
            repair.append("verify rollback: " + str(failure))
        result["rolledBack"] = applied and result.get("filesRestored", False) and not repair
        result["residualDriverImagePossible"] = applied and "LoopZRoutingAudio.driver" in names
        if repair:
            result.update(
                repairRequired=True,
                rollbackErrors=repair,
                backupsRetained=[str(p) for p in transaction.backups.values() if p.exists()],
            )
    finally:
        if "hardwareContinuity" not in result:
            try:
                result["hardwareContinuity"] = guard.stop()
            except Exception as failure:
                result["continuityError"] = str(failure)
                if applied:
                    result.update(rolledBack=False, repairRequired=True)
        try:
            guard.close()
        except Exception as failure:
            result.update(status="failed", repairRequired=True, guardCleanupError=str(failure))
            result["backupsRetained"] = [str(p) for p in transaction.backups.values() if p.exists()]
        if not result.get("repairRequired"):
            transaction.finish()
    return result
