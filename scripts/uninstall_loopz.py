#!/usr/bin/env python3
"""Review or explicitly remove an idle LoopZ installation without a global reset."""

import argparse
import json
import os
from pathlib import Path
import re
import sys
import subprocess
import stat
import tempfile

from application_ownership import REMOVAL_APPLICATIONS
from install_transaction import exclusive_lock
from uninstall_files import RemovalFiles
from uninstall_journal import RemovalJournal, NAME
from uninstall_host import UninstallHost
from uninstall_policy import SUPPORT, TARGETS, digest, preflight, remove_product
from uninstall_recovery import recover_product
from uninstall_workspace import DiagnosticWorkspaces
from suite_journal import NAME as UPDATE_NAME


def pending_update():
    for name in (UPDATE_NAME, ".suite-driver-state.json"):
        path = SUPPORT / name
        try:
            info = path.lstat()
        except FileNotFoundError:
            continue
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != 0
            or info.st_mode & 0o077
            or info.st_nlink != 1
        ):
            raise ValueError("Update recovery state is not a protected root-owned file")
        return {
            "status": "updateRecoveryRequired",
            "readOnly": True,
            "journal": str(path),
            "detail": "Complete coordinated update recovery before removing LoopZ.",
        }
    return None


def pending_recovery():
    path = SUPPORT / NAME
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != 0
        or info.st_mode & 0o077
        or info.st_nlink != 1
    ):
        raise ValueError("Uninstall recovery state is not a protected root-owned file")
    return {
        "status": "recoveryRequired",
        "readOnly": True,
        "journal": str(path),
        "detail": "Complete administrator recovery before another uninstall or installation review.",
    }


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--preflight", action="store_true", help="Read-only removal review (default)")
    mode.add_argument(
        "--remove", action="store_true", help="Explicitly remove the reviewed installation"
    )
    mode.add_argument(
        "--recover",
        action="store_true",
        help="Recover the recorded uninstall decision (administrator)",
    )
    parser.add_argument("--expect-plan-sha", help="SHA-256 from a fresh preflight review")
    options = parser.parse_args(arguments)
    if sys.platform != "darwin":
        parser.error("This system uninstaller is for macOS")
    if options.remove and (
        os.geteuid() != 0
        or not options.expect_plan_sha
        or not re.fullmatch(r"[a-f0-9]{64}", options.expect_plan_sha)
    ):
        parser.error("Removal requires administrator privileges and --expect-plan-sha from review")
    if not options.remove and options.expect_plan_sha:
        parser.error("--expect-plan-sha is used only with --remove")
    if options.recover and os.geteuid() != 0:
        parser.error("Recovery requires administrator privileges")
    try:
        update = pending_update()
        if update:
            print(json.dumps(update, indent=2))
            return 1
        pending = pending_recovery()
        if options.recover:
            if not pending:
                result = {"status": "nothingToRecover", "readOnly": True}
            else:
                with exclusive_lock(SUPPORT / ".install.lock"):
                    if pending_update():
                        raise RuntimeError(
                            "Coordinated update recovery appeared before lock acquisition"
                        )
                    with RemovalJournal(
                        SUPPORT, TARGETS, owner=0, user_components=REMOVAL_APPLICATIONS
                    ) as journal:
                        result = (
                            recover_product(
                                journal, UninstallHost.for_recovery, DiagnosticWorkspaces(owner=0)
                            )
                            if journal.pending()
                            else {"status": "nothingToRecover", "readOnly": True}
                        )
            print(json.dumps(result, indent=2))
            return (
                0
                if result["status"]
                in (
                    "nothingToRecover",
                    "originalInstallationRecovered",
                    "committedRemovalRecovered",
                )
                else 1
            )
        if pending:
            print(json.dumps(pending, indent=2))
            return 1
        host = UninstallHost()
        with RemovalFiles(TARGETS, owner=0, user_components=REMOVAL_APPLICATIONS) as removal:
            if options.remove:
                with exclusive_lock(SUPPORT / ".install.lock"):
                    if pending_update():
                        raise RuntimeError(
                            "Coordinated update recovery appeared before lock acquisition"
                        )
                    with RemovalJournal(
                        SUPPORT, TARGETS, owner=0, user_components=REMOVAL_APPLICATIONS
                    ) as journal:
                        if journal.pending():
                            raise RuntimeError(
                                "Unfinished uninstall recovery appeared before lock acquisition"
                            )
                        work = Path(tempfile.mkdtemp(prefix="loopz-uninstall-", dir="/private/tmp"))
                        workspaces = DiagnosticWorkspaces(owner=0)
                        workspace = workspaces.describe(work)
                        try:
                            result = remove_product(
                                removal,
                                host,
                                work,
                                options.expect_plan_sha,
                                journal=journal,
                                finalize_workspace=workspaces.clean,
                            )
                        finally:
                            try:
                                clean = not journal.created or not journal.pending()
                            except (OSError, ValueError):
                                clean = False
                            if clean:
                                workspaces.clean(workspace)
            else:
                plan = preflight(removal, host)
                result = {
                    "status": "readyForAdministratorPreflight",
                    "readOnly": True,
                    "planSHA256": digest(plan),
                    "plan": plan,
                    "remainingChecks": [
                        "Revalidate this exact plan as administrator",
                        "Verify isolated Apple driver-host identity before mutation",
                        "Require an uninterrupted output continuity guard",
                    ],
                    "limits": [
                        "No removal, system signalling, audio IO or login changes performed",
                        "LoopZ jobs and login startup must be off in every active GUI session",
                        "Logged-out users' login registrations are outside this active-session review",
                        "Actual multi-user uninstall/recovery lifecycle has not been certified",
                    ],
                }
        print(json.dumps(result, indent=2))
        return 0 if result["status"] in ("passed", "readyForAdministratorPreflight") else 1
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "failed", "mode": "loopz-uninstall", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
