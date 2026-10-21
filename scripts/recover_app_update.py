#!/usr/bin/env python3
"""Explicitly recover an interrupted user-app update and the current saved mix."""

import argparse
import json
import os
import subprocess

from active_session import RoutingSession
from app_recovery_context import digest, engine_fingerprint, validate_context
from app_update_admission import updates_directory
from app_update_journal import AppUpdateJournal
from desktop_files import identity
from desktop_install import TARGETS
from editor_update import ENGINE, login_state
from install_desktop import DesktopHost
from install_transaction import exclusive_lock
from routing_install import SUPPORT
from update_desktop import (
    private_updates, require_no_pending_maintenance, save_result, shared_backend_lock,
)
from user_app_transaction import verify_component


class RecoveryHost(DesktopHost):
    allow_pending_app_update = True


def recover(
    journal, host, *, verify=verify_component, session_factory=RoutingSession,
    restore_runtime=True, reopen_interface=True, trace=lambda phase: None,
):
    value = journal.load()
    context = validate_context(value["context"], value["names"])
    paired = context["mode"] == "routing"
    committed = value["phase"] in ("committed", "committed_cleanup")
    before = host.snapshot()
    protected = host.protected_state()
    core_audio = host.service_identity()
    session = session_factory(host)
    if paired and restore_runtime:
        session.read_intent()
        if digest(session.definition) != context["sessionDefinitionSHA256"]:
            raise RuntimeError("The canonical session definition changed; no job will be loaded")
    elif not paired and engine_fingerprint(TARGETS[ENGINE], host.uid) != context["unchangedEngine"]:
        raise RuntimeError("Interface-only recovery cannot replace or change the Engine")
    # Verify the complete chosen outcome before closing any running application.
    trace("verify-apps")
    desired = "new" if committed else "old"
    for name in value["names"]:
        node = value[desired][name]
        path = journal.targets[name]
        if desired == "old" and (
            not path.exists() or identity(path.lstat()) != (node["device"], node["inode"])
        ):
            path = journal.slot(name, "old")
        journal.require_match(path, node)
        verify(name, path)
    trace("apps-verified")
    if not committed:
        session.close_interface(recheck=False)
        if paired and restore_runtime:
            # Closing a preview may release captures, but never overwrites the mix.
            session.document_bytes = None
            session.read_intent()
            if value["phase"] in ("activating", "validating", "restoring"):
                session.stop_for_rollback()
        journal.restore_files(verify)
    trace("app-files-restored")
    acknowledgement = session.restore_current() if paired and restore_runtime else None
    trace("runtime-restored")
    if login_state(host.app_status()) != context["login"]:
        raise RuntimeError("App recovery did not retain the existing login startup choice")
    after = host.snapshot()
    if any(
        before.get(key) != after.get(key)
        for key in ("defaultInput", "defaultOutput", "defaultSystemOutput")
    ) or core_audio != host.service_identity():
        raise RuntimeError("Audio defaults or the Core Audio service changed during recovery")
    if protected != host.protected_state():
        raise RuntimeError("System components or saved routing intent changed during recovery")
    if not paired and engine_fingerprint(TARGETS[ENGINE], host.uid) != context["unchangedEngine"]:
        raise RuntimeError("The Engine changed during interface-only recovery")
    journal.finish(verify, committed=committed)
    trace("app-backups-cleaned")
    should_reopen = restore_runtime and (context["interfaceWasOpen"] or session.ui_closed)
    if should_reopen and reopen_interface:
        session.ui_closed = True
        session.reopen_interface()
    return {
        "status": "passed",
        "mode": "user-app-recovery",
        "outcome": "committed-update-retained" if committed else "original-apps-restored",
        "requiresAdministrator": False,
        "engineSessionStopped": (
            paired and restore_runtime and value["phase"] in ("activating", "validating", "restoring")
        ),
        "interfaceReopenRequested": should_reopen,
        "runtimeRestorationAllowed": restore_runtime,
        "currentRoutingAcknowledgement": acknowledgement,
        "restorationPending": (
            acknowledgement is not None and acknowledgement["runtime"]["restored"] is not True
        ),
        "driverPreserved": True,
        "brokerPreserved": True,
        "userDocumentModified": False,
        "manifestSHA256": context["manifestSHA256"],
    }


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.parse_args(arguments)
    if not os.getuid() or os.getuid() != os.geteuid():
        parser.error("Run recovery as your account, without sudo")
    host = RecoveryHost()
    if host.uid != os.getuid():
        raise RuntimeError("Only the active console account may recover its applications")
    folder = updates_directory(host.uid)
    private_updates(folder, host.uid)
    with exclusive_lock(folder / ".update.lock"):
        with shared_backend_lock(SUPPORT / ".install.lock"):
            require_no_pending_maintenance(SUPPORT)
            from app_login_session import login_session
            from app_recovery_agent import cleanup_agent, cleanup_orphan_permits
            from app_recovery_state import AppRecoveryState
            from watch_app_update import recover_owned

            with AppRecoveryState(folder) as state:
                if state.pending():
                    value = state.load()
                    result = recover_owned(state, host, current_session=login_session(), force_runtime=True)
                    cleanup_agent(folder, value["token"], value["tools"])
                    if result.get("interfaceReopenRequested"):
                        host.user_command("/usr/bin/open", TARGETS["LoopZ.app"])
                else:
                    cleanup_orphan_permits(folder)
                    with AppUpdateJournal(folder, TARGETS, owner=host.uid) as journal:
                        if journal.pending():
                            result = recover(journal, host)
                        else:
                            result = {
                                "status": "not-needed", "mode": "user-app-recovery", "appsChanged": False
                            }
            save_result(folder, result)
            print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "failed", "mode": "user-app-recovery", "error": str(error)}))
        raise SystemExit(1) from None
