#!/usr/bin/env python3
"""Update user-owned apps while retaining the saved graph and existing clients."""

import argparse
from contextlib import contextmanager
from functools import partial
import json
import os
from pathlib import Path
import re
import signal
import subprocess

from active_app_update import ActiveAppHost, deploy_active
from app_recovery_context import update_context
from app_recovery_agent import AppRecoveryLease
from app_update_admission import require_no_pending_app_update
from app_update_journal import AppUpdateJournal
from desktop_install import TARGETS, validate_runtime_capabilities, verify_package
from install_desktop import private_snapshot, verify_code
from install_transaction import blocked_signals, exclusive_lock, files
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


def candidate_compatible(package, host):
    value = json.loads(
        host.user_command(
            package / "LoopZ Engine.app/Contents/MacOS/LoopZ Engine", "--runtime-capabilities"
        ).stdout
    )
    validate_runtime_capabilities(value)
    expected = host.session.expected
    if expected["buffers"] and value.get("hardwareBufferPersistence") != 1:
        raise RuntimeError("The candidate Engine cannot restore saved hardware buffer choices")
    supported = value.get("sampleRatesHz", [44100, 48000])
    if any(device.get("sampleRate") not in supported for device in expected["document"]["devices"]):
        raise RuntimeError("The candidate Engine cannot restore a saved device sample rate")


@contextmanager
def restore_desktop(session):
    try:
        yield
    finally:
        # Reopen UI only; the transaction owns Engine restoration and rollback.
        session.reopen_interface()


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest-sha", required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--guard-uid")
    options = parser.parse_args(arguments)
    if not os.getuid() or os.getuid() != os.geteuid():
        parser.error("Run app updates as your account, without sudo")
    require_no_pending_app_update(os.getuid())
    if not re.fullmatch(r"[a-f0-9]{64}", options.manifest_sha):
        parser.error("Use the reviewed SHA-256 package digest")
    package = options.package.absolute()
    _, digest = verify_package(package, verify_code, options.manifest_sha)
    host = ActiveAppHost(options.guard_uid)
    if host.uid != os.getuid():
        raise RuntimeError("Only the console account may update the shared application pair")
    require_user_apps(TARGETS, host.uid)
    host.existing_apps(package, True, True)
    if files(package / "LoopZ Engine.app") == files(TARGETS["LoopZ Engine.app"]):
        # An interface-only update must not interrupt the audio Engine.
        from manage_editor_update import main as update_interface

        forwarded = ["--package", str(package), "--manifest-sha", digest]
        if options.check:
            forwarded.append("--check")
        return update_interface(forwarded)
    host.session.inspect()
    candidate_compatible(package, host)
    require_no_pending_maintenance(SUPPORT)
    if options.check:
        with shared_backend_lock(SUPPORT / ".install.lock", inspect_only=True):
            pass
        print(
            json.dumps(
                {
                    "status": "ready",
                    "readOnly": True,
                    "requiresAdministrator": False,
                    "activeRoutingAllowed": True,
                    "savedDevices": len(host.session.expected["document"]["devices"]),
                    "engineSessionStopped": False,
                    "manifestSHA256": digest,
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
            require_user_apps(TARGETS, host.uid)
            with restore_desktop(host.session), AppRecoveryLease(host, folder) as recovery:
                host.recovery = recovery
                work = recovery.work
                private = private_snapshot(package, work)
                verify_package(private, verify_code, options.manifest_sha)
                host.existing_apps(private, True, True)
                host.session.inspect()
                candidate_compatible(private, host)
                host.session.close_interface()
                context = update_context(host, digest, interface_was_open=host.session.ui_closed)
                with AppUpdateJournal(folder, TARGETS, owner=host.uid) as journal:
                    factory = partial(
                        UserAppTransaction, journal=journal, context=context, recovery=recovery,
                    )
                    result = deploy_active(private, TARGETS, host, work, transaction_factory=factory)
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
        raise KeyboardInterrupt("Active app update interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        raise SystemExit(main())
    except MigrationRequired as error:
        print(json.dumps({"status": "administratorMigrationRequired", "error": str(error)}))
        raise SystemExit(2) from None
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(
            json.dumps({"status": "failed", "mode": "user-active-app-update", "error": str(error)})
        )
        raise SystemExit(1) from None
