"""macOS-only adapter for the guarded removal transaction."""

import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import time

from desktop_install import requirement
from install_desktop import DesktopHost
from install_local import verify_code as verify_legacy_code
from install_routing import MacHost, command, executable, verify_code
from prepare_routing_install import stable_requirement
from uninstall_driver import DriverRemoval
from uninstall_files import RemovalFiles, open_directory
from uninstall_policy import SUPPORT, TARGETS, require_idle
from uninstall_sessions import SessionInspector, require_sessions_off, validate_sessions

JOBS = {
    "system/com.lexasoft.loopz.routing": (
        TARGETS["routingDaemon"],
        TARGETS["loopz-routing-broker"],
    ),
    "system/com.lexasoft.loopz.bridge": (TARGETS["legacyDaemon"], TARGETS["loopz-broker"]),
}
IDENTITIES = {
    "desktop": "com.lexasoft.loopz.editor",
    "engine": "com.lexasoft.loopz.engine",
    "driver": "com.lexasoft.loopz.driver",
    "routingDriver": "com.lexasoft.loopz.driver.routing",
    "loopz-routing-broker": "com.lexasoft.loopz.routing.broker",
    "loopz-config": "com.lexasoft.loopz.config",
    "loopz-routing-install-check": "com.lexasoft.loopz.routing.install-check",
    "loopz-routing-pin-check": "com.lexasoft.loopz.routing.pin-check",
    "routing/loopz-probe": "com.lexasoft.loopz.probe",
}
LEGACY_IDENTITIES = {
    name: f"com.lexasoft.loopz.{suffix}"
    for name, suffix in (
        ("loopz-broker", "broker"),
        ("loopz-probe", "probe"),
        ("loopz-install-check", "install-check"),
    )
}


class UninstallHost(DesktopHost):
    def __init__(self):
        super().__init__()
        self.original_checker = self.checker
        self.transition = None
        self.session_admission = None
        self.recovering_sessions = False

    @classmethod
    def for_recovery(cls, removal, before):
        if (
            not isinstance(before, dict)
            or not isinstance(before.get("jobs"), dict)
            or set(before["jobs"]) != set(JOBS)
            or any(v is not None and not isinstance(v, str) for v in before["jobs"].values())
            or type(before.get("consoleUID")) is not int
            or before["consoleUID"] <= 0
            or not isinstance(before.get("login"), dict)
            or before["login"].get("registered") is not False
            or before["login"].get("requiresApproval") is not False
            or not isinstance(before.get("catalog"), dict)
            or not isinstance(before.get("documents"), dict)
            or not isinstance(before.get("coreAudio"), str)
        ):
            raise ValueError("Recovery lacks a validated original service/login context")
        require_idle(before.get("catalog", {}))
        if "sessions" in before:
            require_sessions_off(before["sessions"])
        result = cls.__new__(cls)
        MacHost.__init__(result, SUPPORT)
        if result.uid != before["consoleUID"]:
            raise RuntimeError(
                "Sign in to the original console account before restoring LoopZ services"
            )
        name = "loopz-routing-install-check"
        if not removal.originals.get(name):
            raise ValueError("The original recovery checker is not recorded")
        source = removal.quarantined(name) if name in removal.moved else removal.targets[name]
        identifier = IDENTITIES[name]
        verify_code(source, identifier, requirement(identifier), False)
        result.original_checker = result.checker = source
        result.transition = None
        result.session_admission = before.get("sessions")
        result.recovering_sessions = True
        return result

    def require_recovery_stopped(self, state, before):
        # A quarantined desktop cannot report SMAppService state. Its recorded
        # grant was off, and all app/login processes and jobs must still be absent.
        if state["login"] is None and not TARGETS["desktop"].exists():
            state = {**state, "login": before["login"]}
        if self.session_admission is None and not state["sessions"]["desktopAvailable"]:
            # Old journals recorded only the console account. Never extend that
            # limited grant to another account while the desktop is unavailable.
            accounts = validate_sessions(state["sessions"])
            if len(accounts) != 1 or accounts[0]["uid"] != before["consoleUID"]:
                raise RuntimeError("Legacy recovery lacks login choices for other signed-in users")
            self.session_admission = {
                **state["sessions"],
                "desktopAvailable": True,
                "accounts": [{**accounts[0], "login": before["login"]}],
            }
        self.require_stopped(state)

    def login_for_uid(self, uid):
        # A fixed status-only entry point runs in the account being inspected.
        # It does not register, unregister, or launch the routing Engine.
        if uid != self.uid and os.geteuid() != 0:
            raise RuntimeError(
                "Administrator preflight is required to inspect another signed-in user's login choice"
            )
        verify_code(
            TARGETS["desktop"], IDENTITIES["desktop"], requirement(IDENTITIES["desktop"]), False
        )
        if uid == self.uid:
            return self.app_status()["login"]
        response = command(
            "/bin/launchctl",
            "asuser",
            uid,
            "/usr/bin/sudo",
            "-n",
            "-H",
            "-u",
            f"#{uid}",
            TARGETS["desktop"] / "Contents/MacOS/LoopZ",
            "--background-status",
        )
        value = json.loads(response.stdout)
        if value.get("version") != 1 or not isinstance(value.get("login"), dict):
            raise RuntimeError("Per-user desktop login status is unavailable")
        return value["login"]

    def restore_driver(self, removal):
        observer = DriverRemoval.for_restoration(removal)
        observer.restore()

    def verify_components(self, inventory):
        if sum(inventory[name] is not None for name in ("driver", "routingDriver")) != 1:
            raise RuntimeError("Removal requires exactly one installed driver slot")
        for name, identifier in IDENTITIES.items():
            if inventory[name] is not None:
                verify_code(TARGETS[name], identifier, requirement(identifier), name == "engine")
        for name, identifier in LEGACY_IDENTITIES.items():
            if inventory[name] is not None:
                verify_legacy_code(TARGETS[name], identifier)
        for name in ("desktop", "engine", "loopz-routing-install-check", "loopz-config"):
            if inventory[name] is None:
                raise RuntimeError(
                    "A matching installed desktop/Engine and routing tools are needed for guarded removal"
                )
        verify_code(
            TARGETS["desktop"] / "Contents/MacOS/LoopZLogin",
            "com.lexasoft.loopz.login",
            requirement("com.lexasoft.loopz.login"),
            False,
        )
        for label, (plist, program) in JOBS.items():
            if not plist.exists():
                continue
            value = plistlib.loads(plist.read_bytes())
            if (
                value.get("Label") != label.removeprefix("system/")
                or not isinstance(value.get("ProgramArguments"), list)
                or value["ProgramArguments"][:1] != [str(program)]
                or value.get("EnvironmentVariables")
                or value.get("Program")
            ):
                raise RuntimeError("A foreign daemon definition occupies a LoopZ path")
        pin = {
            "protocol": 2,
            "brokerRequirement": stable_requirement(
                "com.lexasoft.loopz.routing.broker", "USJ7H3X44X"
            ),
        }
        if json.loads(TARGETS["routing-service.json"].read_text()) != pin:
            raise RuntimeError(
                "Routing trust configuration differs from the installed publisher policy"
            )

    def jobs(self):
        result = {}
        for label, (plist, program) in JOBS.items():
            response = command("/bin/launchctl", "print", label, check=False)
            if response.returncode:
                if (
                    response.returncode not in (3, 113)
                    or "Could not find service" not in response.stdout + response.stderr
                ):
                    raise RuntimeError("LoopZ daemon state could not be established")
                result[label] = None
                continue
            lines = {line.strip() for line in response.stdout.splitlines()}
            pid = re.search(r"^\s*pid = (\d+)\s*$", response.stdout, re.M)
            if not pid or f"path = {plist}" not in lines or f"program = {program}" not in lines:
                raise RuntimeError("A foreign or unidentified job occupies a LoopZ service label")
            if executable(int(pid[1])) != str(program):
                raise RuntimeError("The LoopZ job executable differs from its protected definition")
            result[label] = command(
                "/bin/ps", "-p", pid[1], "-o", "pid=,lstart=,command="
            ).stdout.strip()
        return result

    def documents(self):
        support = Path(self.user.pw_dir) / "Library/Application Support/LoopZ"
        try:
            directory = open_directory(support)
        except FileNotFoundError:
            exists = False
        else:
            try:
                info = os.fstat(directory)
                if info.st_uid != self.uid or info.st_mode & 0o022:
                    raise RuntimeError("User routing directory has unsafe ownership or permissions")
                exists = True
            finally:
                os.close(directory)
        targets = {
            name: support / name
            for name in (
                "routes.json",
                "routes.json.backup",
                "Session/engine.plist",
                "Session/session.lock",
            )
        }
        with RemovalFiles(targets, owner=self.uid) as reader:
            return {"directoryExists": exists, "files": reader.inventory()}

    def state(self):
        if os.stat("/dev/console").st_uid != self.uid:
            raise RuntimeError("Console account changed during removal review")
        available = TARGETS["desktop"].exists()
        sessions = SessionInspector(command, self.login_for_uid).snapshot(self.uid, available)
        if os.stat("/dev/console").st_uid != self.uid:
            raise RuntimeError("Console account changed during removal review")
        login = next(a["login"] for a in sessions["accounts"] if a["uid"] == self.uid)
        return {
            "catalog": self.snapshot(),
            "coreAudio": self.service_identity(),
            "documents": self.documents(),
            "login": login,
            "jobs": self.jobs(),
            "consoleUID": self.uid,
            "sessions": sessions,
        }

    def require_stopped(self, state):
        self.require_app_recovery_clear()
        require_idle(state["catalog"])
        self.require_idle(state["catalog"])
        require_sessions_off(
            state["sessions"],
            self.session_admission,
            allow_new_generation=self.recovering_sessions,
        )
        if state["sessions"]["desktopAvailable"]:
            self.session_admission = require_sessions_off(state["sessions"])
        login = state["login"]
        if login is None and not state["sessions"]["desktopAvailable"]:
            login = next(
                a["login"] for a in self.session_admission["accounts"] if a["uid"] == self.uid
            )
        if (
            not isinstance(login, dict)
            or login.get("registered") is not False
            or login.get("requiresApproval") is not False
        ):
            raise RuntimeError("Turn off Start at login in LoopZ Settings before removal")
        processes = command("/usr/bin/pgrep", "-x", "LoopZLogin", check=False)
        if processes.returncode not in (0, 1):
            raise RuntimeError("Login-helper state could not be established")
        for value in processes.stdout.split():
            if executable(int(value)).endswith("/LoopZ.app/Contents/MacOS/LoopZLogin"):
                raise RuntimeError("The LoopZ login helper is still running")

    def prepare_tools(self, work):
        info = work.lstat()
        if info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise RuntimeError("Removal requires a private owned work directory")
        destination = work / "loopz-removal-check"
        before = hashlib.sha256(self.original_checker.read_bytes()).digest()
        with self.original_checker.open("rb") as source, destination.open("xb") as output:
            shutil.copyfileobj(source, output)
        destination.chmod(0o755)
        if hashlib.sha256(destination.read_bytes()).digest() != before:
            raise RuntimeError("Removal checker changed while copying")
        verify_code(
            destination,
            IDENTITIES["loopz-routing-install-check"],
            requirement(IDENTITIES["loopz-routing-install-check"]),
            False,
        )
        self.checker = destination

    def start_guard(self, work, before):
        # The console-owned guard must traverse this root-owned private folder.
        # Grant only that account search access; keep the checker and stop file
        # root-owned and keep every mode/journal privacy check in force.
        command("/bin/chmod", "+a", f"user:{self.uid} allow search", work)
        return super().start_guard(work, before)

    def driver_transition(self, removal):
        self.transition = DriverRemoval(removal)
        return self.transition

    def stop_services(self, before):
        if self.jobs() != before["jobs"]:
            raise RuntimeError("LoopZ services changed after review")
        for label, state in before["jobs"].items():
            if state is not None:
                command("/bin/launchctl", "bootout", label)
        if any(value is not None for value in self.jobs().values()):
            raise RuntimeError("LoopZ services did not stop")

    def restore_services(self, before):
        for label, state in before["jobs"].items():
            if state is not None and self.jobs()[label] is None:
                command("/bin/launchctl", "bootstrap", "system", JOBS[label][0])
        if {key: value is not None for key, value in self.jobs().items()} != {
            key: value is not None for key, value in before["jobs"].items()
        }:
            raise RuntimeError("LoopZ service registration was not restored")

    def wait_removed(self):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            snapshot = self.snapshot()
            require_idle(snapshot)
            if (
                snapshot.get("routingPlugIn", {}).get("registered") is False
                and not self.transition.pids()
            ):
                return
            time.sleep(0.1)
        raise RuntimeError("LoopZ driver did not withdraw after isolated removal")
