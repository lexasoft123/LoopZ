#!/usr/bin/env python3
"""Update apps and restore the existing desktop session afterwards.

--migrate authorizes the one-time root-owned-app migration. Later updates use
the active-routing updater as the console account. No driver or broker is replaced.
"""

import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import signal
import subprocess
import sys
import time

from desktop_install import IDENTIFIERS, TARGETS, requirement, verify_package
from app_update_admission import app_update_pending
from install_desktop import DesktopHost
from install_routing import command, executable, verify_code
from install_transaction import check_ancestors
from update_desktop import MigrationRequired, require_user_apps

ROOT = Path(__file__).resolve().parent
SESSION = "com.lexasoft.loopz.session"


class EmptySession:
    def __init__(self, host):
        self.host = host
        self.folder = Path(host.user.pw_dir) / "Library/Application Support/LoopZ"
        self.job = self.folder / "Session/engine.plist"
        self.label = f"gui/{host.uid}/{SESSION}"
        self.definition = None
        self.ui = []
        self.paused = False
        self.ui_closed = False

    def process(self, pid):
        path = TARGETS["LoopZ.app"] / "Contents/MacOS/LoopZ"
        if executable(pid) != str(path):
            raise RuntimeError("A different executable occupies the LoopZ process name")
        result = command("/bin/ps", "-p", pid, "-o", "uid=,lstart=,command=", check=False)
        if result.returncode:
            return None
        if not re.match(rf"\s*{self.host.uid}\s", result.stdout):
            raise RuntimeError("Another account is running the desktop; leave it untouched")
        return result.stdout.strip()

    def inspect(self):
        self.host.protected_state()  # Includes ownership/link checks on user files.
        routes = self.folder / "routes.json"
        check_ancestors([routes, self.job])
        document = json.loads(routes.read_text())
        if document.get("version") != 2 or document.get("devices") != []:
            raise RuntimeError(
                "This update requires an empty saved mix; active routing is preserved"
            )
        # A native snapshot uses the authenticated Engine connection, not UI input.
        state = json.loads(
            command(TARGETS["LoopZ.app"] / "Contents/MacOS/LoopZ", "--routing-status").stdout
        )
        if (
            state.get("document", {}).get("devices") != []
            or state.get("runtime", {}).get("devices") != []
            or state.get("runtime", {}).get("restored") is not True
        ):
            raise RuntimeError("The current Engine is not idle and fully restored")
        before = self.host.snapshot()
        if any(
            before.get(key) != []
            for key in ("routingDevices", "routingBlockingRoutes", "loopzBlockingRoutes")
        ):
            raise RuntimeError("Audio clients still depend on LoopZ")
        job = command("/bin/launchctl", "print", self.label)
        if f"path = {self.job}" not in {
            line.strip() for line in job.stdout.splitlines()
        } or f"program = {TARGETS['LoopZ Engine.app']}/Contents/MacOS/LoopZ Engine" not in {
            line.strip() for line in job.stdout.splitlines()
        }:
            raise RuntimeError("The session job is not the canonical LoopZ Engine")
        self.definition = self.job.read_bytes()
        definition = plistlib.loads(self.definition)
        expected = [
            str(TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine"),
            "--routing-document",
            str(routes),
            "--editor-service",
            "true",
            "--block-frames",
            "64",
        ]
        if definition.get("Label") != SESSION or definition.get("ProgramArguments") != expected:
            raise RuntimeError("The saved session job has an unsupported configuration")
        processes = command("/usr/bin/pgrep", "-x", "LoopZ", check=False)
        if processes.returncode not in (0, 1):
            raise RuntimeError("Cannot inspect the desktop processes")
        self.ui = [(int(pid), self.process(int(pid))) for pid in processes.stdout.split()]
        return state

    def pause(self):
        self.inspect()
        for pid, generation in self.ui:
            if generation is not None and self.process(pid) == generation:
                self.ui_closed = True
                os.kill(pid, signal.SIGTERM)
                deadline = time.monotonic() + 8
                while self.process_alive(pid, generation):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("The desktop did not quit; no forced termination used")
                    time.sleep(0.05)
        self.paused = True  # Restore even if bootout returns an uncertain result.
        command("/bin/launchctl", "bootout", self.label)
        deadline = time.monotonic() + 8
        while not command("/bin/launchctl", "print", self.label, check=False).returncode:
            if time.monotonic() >= deadline:
                raise RuntimeError("The empty Engine session did not stop")
            time.sleep(0.05)

    def process_alive(self, pid, generation):
        result = command("/bin/ps", "-p", pid, "-o", "uid=,lstart=,command=", check=False)
        return not result.returncode and result.stdout.strip() == generation

    def restore(self):
        if self.paused:
            check_ancestors([self.job])
            if self.job.read_bytes() != self.definition:
                raise RuntimeError(
                    "The session definition changed; refusing to load a different job"
                )
            running = command("/bin/launchctl", "print", self.label, check=False)
            if running.returncode:
                verify_code(
                    TARGETS["LoopZ Engine.app"],
                    IDENTIFIERS["LoopZ Engine.app"],
                    requirement(IDENTIFIERS["LoopZ Engine.app"]),
                    True,
                )
                command("/bin/launchctl", "bootstrap", f"gui/{self.host.uid}", self.job)
            deadline = time.monotonic() + 20
            while True:
                try:
                    response = command(
                        TARGETS["LoopZ.app"] / "Contents/MacOS/LoopZ",
                        "--routing-status",
                        check=False,
                        timeout=3,
                    )
                    state = json.loads(response.stdout) if response.returncode == 0 else {}
                except (subprocess.SubprocessError, ValueError):
                    state = {}
                if (
                    state.get("document", {}).get("devices") == []
                    and state.get("runtime", {}).get("devices") == []
                    and state.get("runtime", {}).get("restored") is True
                ):
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        "The apps were updated, but the Engine did not restore its empty graph"
                    )
                time.sleep(0.2)
            self.paused = False
        if self.ui_closed and not app_update_pending(self.host.uid):
            command("/usr/bin/open", TARGETS["LoopZ.app"])
            self.ui_closed = False


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--manifest-sha")
    parser.add_argument("--migrate", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--recover", action="store_true")
    options = parser.parse_args(arguments)
    if not os.getuid() or os.getuid() != os.geteuid():
        parser.error("Run this step as your account, without a leading sudo")
    if options.recover:
        if options.package or options.manifest_sha or options.migrate or options.check:
            parser.error("Run --recover separately from package, migration and check options")
        from recover_app_update import main as recover_apps

        return recover_apps([])
    if not options.package or not options.manifest_sha:
        parser.error("An update requires --package and --manifest-sha")
    if not options.migrate:
        invocation = [
            "/usr/bin/python3",
            "-E",
            "-s",
            "-S",
            str(ROOT / "update_active_apps.py"),
            "--package",
            str(options.package.absolute()),
            "--manifest-sha",
            options.manifest_sha,
        ]
        if options.check:
            invocation.append("--check")
        return subprocess.run(invocation).returncode
    host = DesktopHost()
    if host.uid != os.getuid():
        raise RuntimeError("An active console account is required")
    package = options.package.absolute()
    verify_package(package, verify_code, options.manifest_sha)
    host.existing_apps(package, True, True)
    migration = False
    try:
        require_user_apps(TARGETS, host.uid)
    except MigrationRequired:
        migration = True
    session = EmptySession(host)
    session.inspect()
    if options.check:
        print(
            json.dumps(
                {
                    "status": "ready",
                    "readOnly": True,
                    "requiresAdministrator": migration,
                    "sessionRestoration": True,
                    "driverPreserved": True,
                    "manifestSHA256": options.manifest_sha,
                }
            )
        )
        return 0
    if migration and not options.migrate:
        raise MigrationRequired("Use --migrate once for the existing root-owned apps")
    if migration:
        if not sys.stdin.isatty():
            raise RuntimeError(
                "Run the migration in Terminal to authorize it before apps are stopped"
            )
        # Authorization happens before any session interruption; no password is read by Python.
        subprocess.run(["/usr/bin/sudo", "-v"], check=True)
    tool = "install_desktop.py" if migration else "update_desktop.py"
    invocation = (["/usr/bin/sudo", "-n"] if migration else []) + [
        "/usr/bin/python3",
        "-E",
        "-s",
        "-S",
        str(ROOT / tool),
        "--package",
        str(package),
        "--manifest-sha",
        options.manifest_sha,
    ]
    if migration:
        invocation += ["--replace-engine", "--replace-editor"]
    try:
        session.pause()
        result = subprocess.run(invocation)
        return result.returncode
    finally:
        session.restore()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "failed", "mode": "managed-app-update", "error": str(error)}))
        raise SystemExit(1) from None
