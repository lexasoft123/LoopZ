"""Fixed macOS product inventory and removal admission policy."""

import hashlib
import json
from pathlib import Path

from uninstall_sessions import validate_session_preservation

SUPPORT = Path("/Library/Application Support/LoopZ")
TARGETS = {
    "desktop": Path("/Applications/LoopZ.app"),
    "engine": Path("/Applications/LoopZ Engine.app"),
    "driver": Path("/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver"),
    "routingDriver": Path("/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver"),
    "routingDaemon": Path("/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist"),
    "legacyDaemon": Path("/Library/LaunchDaemons/com.lexasoft.loopz.bridge.plist"),
}
for _name in (
    "loopz-routing-broker",
    "loopz-config",
    "loopz-routing-install-check",
    "loopz-routing-pin-check",
    "routing-service.json",
    "loopz-broker",
    "loopz-probe",
    "loopz-install-check",
    "install-result.json",
    "routing-install-result.json",
    "desktop-install-result.json",
    "driver-update-result.json",
    "routing/loopz-probe",
    "routing/com.lexasoft.loopz.engine.plist",
):
    TARGETS[_name] = SUPPORT / _name

DEFAULTS = ("defaultInput", "defaultOutput", "defaultSystemOutput")


def digest(plan):
    return hashlib.sha256(
        json.dumps(plan, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def require_idle(catalog):
    for key in DEFAULTS:
        uid = catalog.get(key)
        if not isinstance(uid, str) or uid.startswith("com.lexasoft.loopz."):
            raise RuntimeError("Choose non-LoopZ audio defaults before removal")
    for key in ("routingDevices", "routingBlockingRoutes", "loopzBlockingRoutes"):
        if catalog.get(key) != []:
            raise RuntimeError("Removal requires a complete empty catalog without dependent routes")
    if catalog.get("loopzVisible") is not False or catalog.get("loopzRunning") is not False:
        raise RuntimeError("Stop routing before removal; prototype teardown requires migration")


def validate_preserved(before, after):
    require_idle(after["catalog"])
    if any(before["catalog"][key] != after["catalog"][key] for key in DEFAULTS):
        raise RuntimeError("Audio defaults changed during removal")
    for key in ("coreAudio", "documents"):
        if not before.get(key) or before[key] != after.get(key):
            raise RuntimeError(f"Removal changed preserved state: {key}")
    if "sessions" in before or "sessions" in after:
        validate_session_preservation(before.get("sessions"), after.get("sessions"))


def preflight(removal, host):
    inventory = removal.prepare()
    host.verify_components(inventory)
    state = host.state()
    require_idle(state["catalog"])
    host.require_stopped(state)
    if not any(value is not None for value in inventory.values()):
        raise RuntimeError("No LoopZ components are installed")
    return {
        "version": 1,
        "mode": "loopz-uninstall",
        "targets": {name: str(path) for name, path in removal.targets.items()},
        "inventory": inventory,
        "before": state,
        "preserve": [
            "user routing documents",
            "audio defaults",
            "Core Audio",
            "unknown files",
            "installer lock",
        ],
    }


def remove_product(removal, host, work, expected_digest, *, journal=None, finalize_workspace=None):
    """Keep all file originals until runtime and output continuity pass."""
    plan = preflight(removal, host)
    if digest(plan) != expected_digest:
        raise RuntimeError("Removal plan changed after review; run preflight again")
    before = plan["before"]
    result = {
        "status": "failed",
        "mode": "loopz-uninstall",
        "planSHA256": expected_digest,
        "before": before,
        "committed": False,
        "userDocumentsPreserved": False,
    }
    guard = transition = None
    services_attempted = False
    try:
        if journal:
            journal.begin(plan, removal, work)
        host.prepare_tools(work)
        transition = host.driver_transition(removal)
        guard = host.start_guard(work, before["catalog"])
        guard.ready()
        current = host.state()
        host.require_stopped(current)
        validate_preserved(before, current)
        require_idle(host.snapshot())
        services_attempted = True
        host.stop_services(before)
        removal.apply()
        current = host.state()
        host.require_stopped(current)
        validate_preserved(before, current)
        transition.deactivate(removal)
        host.wait_removed()
        after = host.state()
        host.require_stopped(after)
        validate_preserved(before, after)
        guard.check()
        result["hardwareContinuity"] = guard.stop()
        if result["hardwareContinuity"].get("passed") is not True:
            raise RuntimeError("Output continuity did not pass during removal")
        guard.close()
        guard = None
        result.update(after=after, userDocumentsPreserved=True)
        # Purge is the irreversible boundary. Never describe a cleanup failure
        # beyond this point as a successful rollback.
        cleanup = removal.purge()
        result.update(committed=True, status="cleanupRequired" if cleanup else "passed")
        if cleanup:
            result["cleanupRequired"] = cleanup
    except BaseException as error:
        result["error"] = str(error) or type(error).__name__
        repair = []
        if removal.committed:
            result.update(
                committed=None if removal.commit_uncertain else True,
                status="recoveryRequired" if removal.commit_uncertain else "cleanupRequired",
            )
            repair.append(
                "Commit could not be confirmed; retain originals for recovery"
                if removal.commit_uncertain
                else "Removal is committed; recover retained quarantine cleanup"
            )
        else:
            try:
                removal.rollback()
            except BaseException as failure:
                repair.append("restore files: " + str(failure))
            if services_attempted:
                try:
                    host.restore_services(before)
                    if transition:
                        transition.restore()
                except BaseException as failure:
                    repair.append("restore services: " + str(failure))
            try:
                restored = host.state()
                validate_preserved(before, restored)
                result["afterRollback"] = restored
            except BaseException as failure:
                repair.append("verify restored state: " + str(failure))
            result["rolledBack"] = not repair
        if repair:
            result["repairRequired"] = repair
    finally:
        if guard:
            try:
                guard.close()
            except BaseException as error:
                result["status"] = "failed"
                if not result["committed"]:
                    result["rolledBack"] = False
                result.setdefault("repairRequired", []).append("close output guard: " + str(error))
    if removal.moved:
        result["retainedQuarantines"] = {
            name: str(removal.quarantined(name)) for name in removal.moved
        }
    if journal and journal.created:
        if result["status"] == "passed" or result.get("rolledBack") is True:
            try:
                if finalize_workspace:
                    finalize_workspace(journal.value["workspace"])
                journal.finish("removed" if result["committed"] else "restored")
            except (OSError, RuntimeError, ValueError) as error:
                result["status"] = "recoveryRequired"
                if result.get("rolledBack") is True:
                    result["filesAndServicesRestored"] = True
                    result["rolledBack"] = False
                result.setdefault("repairRequired", []).append(
                    "finalize recovery journal: " + str(error)
                )
        try:
            pending = journal.pending()
        except (OSError, ValueError) as error:
            pending = True
            result["status"] = "recoveryRequired"
            result.setdefault("repairRequired", []).append(
                "inspect recovery journal: " + str(error)
            )
        if pending:
            result["recoveryJournal"] = str(journal.folder / ".uninstall-state.json")
            result["retainedWorkspace"] = str(work)
    return result
