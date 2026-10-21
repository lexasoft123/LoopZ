#!/usr/bin/env python3
"""Close and update the interface while preserving the running audio session."""

import argparse
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import time

from desktop_install import TARGETS, verify_package
from app_update_admission import require_no_pending_app_update
from editor_update import EDITOR, EditorUpdateHost
from install_routing import command, verify_code
from manage_app_update import EmptySession
from update_desktop import MigrationRequired, require_user_apps


class InterfaceSession(EmptySession):
    def inspect(self):
        self.host.protected_state()
        processes = command("/usr/bin/pgrep", "-x", "LoopZ", check=False)
        if processes.returncode not in (0, 1):
            raise RuntimeError("Cannot inspect the desktop processes")
        self.ui = [(int(pid), self.process(int(pid))) for pid in processes.stdout.split()]

    def pause(self):
        self.inspect()
        for pid, generation in self.ui:
            if generation is not None and self.process(pid) == generation:
                self.ui_closed = True
                os.kill(pid, signal.SIGTERM)
                deadline = time.monotonic() + 8
                while self.process_alive(pid, generation):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("The desktop did not quit; the Engine was left running")
                    time.sleep(0.05)
        # Keep EmptySession.paused false: restoration only reopens the desktop.


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest-sha", required=True)
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args(arguments)
    if not os.getuid() or os.getuid() != os.geteuid():
        parser.error("Run interface updates as your account, without sudo")
    require_no_pending_app_update(os.getuid())
    if not re.fullmatch(r"[a-f0-9]{64}", options.manifest_sha):
        parser.error("Use the reviewed SHA-256 package digest")
    package = options.package.absolute()
    verify_package(package, verify_code, options.manifest_sha)
    host = EditorUpdateHost(package)
    if host.uid != os.getuid():
        raise RuntimeError("An active console account is required")
    require_user_apps({EDITOR: TARGETS[EDITOR]}, host.uid)
    host.existing_apps(package, False, True)
    session = InterfaceSession(host)
    session.inspect()
    if options.check:
        print(
            json.dumps(
                {
                    "status": "ready",
                    "readOnly": True,
                    "requiresAdministrator": False,
                    "engineSessionStopped": False,
                    "manifestSHA256": options.manifest_sha,
                }
            )
        )
        return 0
    invocation = [
        "/usr/bin/python3",
        "-E",
        "-s",
        "-S",
        str(Path(__file__).resolve().parent / "update_editor.py"),
        "--package",
        str(package),
        "--manifest-sha",
        options.manifest_sha,
    ]
    try:
        session.pause()
        if session.ui_closed:
            invocation.append("--reopen-interface")
        return subprocess.run(invocation).returncode
    finally:
        session.restore()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MigrationRequired as error:
        print(json.dumps({"status": "administratorMigrationRequired", "error": str(error)}))
        raise SystemExit(2) from None
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(
            json.dumps(
                {"status": "failed", "mode": "managed-interface-update", "error": str(error)}
            )
        )
        raise SystemExit(1) from None
