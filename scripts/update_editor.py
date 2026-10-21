#!/usr/bin/env python3
"""Update only the user-owned interface without stopping the audio Engine."""

import argparse
from functools import partial
import json
import os
from pathlib import Path
import re
import signal
import subprocess

from desktop_install import TARGETS, verify_package
from app_recovery_context import update_context
from app_recovery_agent import AppRecoveryLease
from app_update_admission import require_no_pending_app_update
from app_update_journal import AppUpdateJournal
from editor_update import EDITOR, EditorUpdateHost, deploy_editor, require_same_engine
from install_desktop import private_snapshot, verify_code
from install_transaction import blocked_signals, exclusive_lock
from routing_install import SUPPORT
from update_desktop import (
    MigrationRequired,
    private_updates,
    require_no_pending_maintenance,
    require_user_apps,
    save_result,
    shared_backend_lock,
)
from user_app_transaction import UserAppTransaction


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest-sha", required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--reopen-interface", action="store_true")
    options = parser.parse_args(arguments)
    if not os.getuid() or os.getuid() != os.geteuid():
        parser.error("Run interface updates as your account, without sudo")
    require_no_pending_app_update(os.getuid())
    if not re.fullmatch(r"[a-f0-9]{64}", options.manifest_sha):
        parser.error("Use the reviewed SHA-256 package digest")
    package = options.package.absolute()
    _, digest = verify_package(package, verify_code, options.manifest_sha)
    host = EditorUpdateHost(package)
    if host.uid != os.getuid():
        raise RuntimeError("Only the console account may update the interface")
    require_user_apps({EDITOR: TARGETS[EDITOR]}, host.uid)
    host.existing_apps(package, False, True)
    host.require_idle(host.snapshot())
    host.protected_state()
    require_no_pending_maintenance(SUPPORT)
    if options.check:
        with shared_backend_lock(SUPPORT / ".install.lock", inspect_only=True):
            pass
        print(
            json.dumps(
                {
                    "status": "ready",
                    "manifestSHA256": digest,
                    "requiresAdministrator": False,
                    "engineReplaced": False,
                    "engineSessionStopped": False,
                    "deploymentVerified": False,
                }
            )
        )
        return 0
    folder = Path(host.user.pw_dir) / "Library/Application Support/LoopZ/Updates"
    private_updates(folder, host.uid)
    with exclusive_lock(folder / ".update.lock"):
        with shared_backend_lock(SUPPORT / ".install.lock"):
            require_no_pending_app_update(host.uid)
            require_no_pending_maintenance(SUPPORT)
            require_user_apps({EDITOR: TARGETS[EDITOR]}, host.uid)
            with AppRecoveryLease(host, folder) as recovery:
                work = recovery.work
                private = private_snapshot(package, work)
                verify_package(private, verify_code, options.manifest_sha)
                require_same_engine(private)
                host.existing_apps(private, False, True)
                context = update_context(
                    host, digest, interface_was_open=options.reopen_interface, interface_only=True
                )
                with AppUpdateJournal(folder, TARGETS, owner=host.uid) as journal:
                    factory = partial(
                        UserAppTransaction, journal=journal, context=context, recovery=recovery,
                    )
                    result = deploy_editor(
                        private, TARGETS, host, work, transaction_factory=factory
                    )
                    result["durableRecoveryPending"] = journal.pending()
                result.update(
                    manifestSHA256=digest, applicationOwnerUID=host.uid,
                    requiresAdministrator=False, automaticRecoveryArmed=True,
                )
                with blocked_signals():
                    save_result(folder, result)
                    print(json.dumps(result), flush=True)
                return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":

    def interrupted(signum, frame):
        raise KeyboardInterrupt("Interface update interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        raise SystemExit(main())
    except MigrationRequired as error:
        print(json.dumps({"status": "administratorMigrationRequired", "error": str(error)}))
        raise SystemExit(2) from None
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(
            json.dumps({"status": "failed", "mode": "user-interface-update", "error": str(error)})
        )
        raise SystemExit(1) from None
