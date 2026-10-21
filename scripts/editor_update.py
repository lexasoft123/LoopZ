"""Interface-only updates retain the installed Engine, session and device graph."""

import json
import re
from pathlib import Path

from desktop_install import IDENTIFIERS, TARGETS, deploy, requirement, validate_runtime_capabilities
from install_desktop import DesktopHost, verify_code
from install_routing import command, executable
from install_transaction import files

EDITOR = "LoopZ.app"
ENGINE = "LoopZ Engine.app"


def require_same_engine(package, targets=TARGETS):
    expected = files(Path(package) / ENGINE)
    if not expected or files(targets[ENGINE]) != expected:
        raise RuntimeError("An interface-only update must contain the unchanged installed Engine")
    return expected


LOGIN_FILES = (
    "Contents/MacOS/LoopZLogin",
    "Contents/Library/LaunchAgents/com.lexasoft.loopz.login.plist",
)
LOGIN_FLAGS = ("available", "enabled", "registered", "requiresApproval")


def login_state(status):
    value = status.get("login") if isinstance(status, dict) else None
    if (
        not isinstance(status, dict)
        or type(status.get("version")) is not int
        or status["version"] != 1
        or not isinstance(value, dict)
        or any(type(value.get(key)) is not bool for key in LOGIN_FLAGS)
    ):
        raise RuntimeError("Login startup state could not be established")
    if value["enabled"] and value["requiresApproval"]:
        raise RuntimeError("Login startup cannot be enabled and awaiting approval together")
    if (value["enabled"] or value["requiresApproval"]) and not value["registered"]:
        raise RuntimeError("Login startup is active without a registration")
    if not value["available"] and any(value[key] for key in LOGIN_FLAGS[1:]):
        raise RuntimeError("Login startup is registered at an unavailable application")
    return {key: value[key] for key in LOGIN_FLAGS}


def require_unchanged_login(package, targets=TARGETS):
    for relative in LOGIN_FILES:
        expected = files(Path(package) / EDITOR / relative)
        if not expected or files(targets[EDITOR] / relative) != expected:
            raise RuntimeError(
                "An enabled login item requires its helper and definition to remain unchanged"
            )


def session_generations(host, engine):
    result = {}
    for name in ("com.lexasoft.loopz.session", "com.lexasoft.loopz.engine"):
        label = f"{host.domain}/{name}"
        job = host.user_command("/bin/launchctl", "print", label, check=False)
        if job.returncode:
            if (
                job.returncode not in (3, 113)
                or "Could not find service" not in job.stdout + job.stderr
            ):
                raise RuntimeError("The existing Engine session could not be inspected")
            result[name] = None
            continue
        pid = re.search(r"^\s*pid = ([0-9]+)\s*$", job.stdout, re.M)
        if not pid or executable(int(pid[1])) != str(engine / "Contents/MacOS/LoopZ Engine"):
            raise RuntimeError("The existing Engine session has an unrecognized process")
        process = host.user_command(
            "/bin/ps", "-p", pid[1], "-o", "uid=,pid=,lstart=,command="
        ).stdout.strip()
        if not re.match(rf"{host.uid}\s", process):
            raise RuntimeError("The Engine session belongs to a different account")
        result[name] = process
    return result


class PassiveSessionGuard:
    """Observe the existing session; never open audio or publish a test device."""

    def __init__(self, observe):
        self.observe = observe
        self.baseline = observe()

    def ready(self):
        self.check()

    def check(self):
        if self.observe() != self.baseline:
            raise RuntimeError("The Engine or its session changed during the interface update")

    def stop(self):
        self.check()
        return {
            "passed": True,
            "mode": "existing-session-observation",
            "audioOpened": False,
            "audioFramesVerified": False,
        }

    def close(self):
        pass


class EditorUpdateHost(DesktopHost):
    allow_pending_app_update = True

    def __init__(self, package):
        super().__init__()
        self.expected_engine = require_same_engine(package, TARGETS)
        self.expected_login = None

    def engine_state(self):
        path = TARGETS[ENGINE]
        info = path.lstat()
        current = files(path)
        if current != self.expected_engine:
            raise RuntimeError("The installed Engine changed after interface-update review")
        return {
            "files": current,
            "device": info.st_dev,
            "inode": info.st_ino,
            "owner": info.st_uid,
            "mode": info.st_mode,
            "sessions": session_generations(self, path),
        }

    def require_idle(self, snapshot):
        # Active routing is allowed. Only the interface must finish quitting.
        self.engine_state()
        processes = command("/usr/bin/pgrep", "-x", "LoopZ", check=False)
        if processes.returncode not in (0, 1):
            raise RuntimeError("The desktop process could not be inspected")
        for pid in processes.stdout.split():
            if executable(int(pid)) == str(TARGETS[EDITOR] / "Contents/MacOS/LoopZ"):
                raise RuntimeError("Close the desktop before replacing its interface")

    def protected_state(self):
        return {
            **super().protected_state(),
            "unchangedEngine": self.engine_state(),
            "loginStartup": login_state(self.app_status()),
        }

    def existing_apps(self, package, replace_engine, replace_editor):
        if replace_engine:
            raise ValueError("The interface updater cannot replace the Engine")
        require_same_engine(package, TARGETS)
        for name in (EDITOR, ENGINE):
            verify_code(
                TARGETS[name], IDENTIFIERS[name], requirement(IDENTIFIERS[name]), name == ENGINE
            )
        if not replace_editor and files(TARGETS[EDITOR]) != files(Path(package) / EDITOR):
            raise ValueError("Replacing the interface requires an explicit update")
        current = login_state(self.app_status())
        if self.expected_login is None:
            self.expected_login = current
        elif current != self.expected_login:
            raise RuntimeError("Login startup changed after interface-update review")
        if current["registered"]:
            require_unchanged_login(package, TARGETS)

    def validate_apps(self):
        for relative in ("", "Contents/MacOS/LoopZLogin"):
            key = EDITOR if not relative else EDITOR + "/" + relative
            verify_code(
                TARGETS[EDITOR] / relative, IDENTIFIERS[key], requirement(IDENTIFIERS[key]), False
            )
        self.user_command(TARGETS[EDITOR] / "Contents/MacOS/LoopZ", "--verify-interface")
        capabilities = json.loads(
            self.user_command(
                TARGETS[ENGINE] / "Contents/MacOS/LoopZ Engine", "--runtime-capabilities"
            ).stdout
        )
        validate_runtime_capabilities(capabilities)
        status = self.app_status()
        current = login_state(status)
        if current != self.expected_login or not current["available"]:
            raise RuntimeError("The interface update did not preserve login startup")
        return {"engine": capabilities, "desktop": status, "interfaceSealVerified": True}

    def start_guard(self, work, before):
        return PassiveSessionGuard(self.engine_state)

    def validate_activation(self, work):
        return {
            "mode": "existing-engine",
            "engineReplaced": False,
            "engineSessionStopped": False,
            "validationDevicePublished": False,
            "updateOpenedCapture": False,
            "updateOpenedPhysicalOutput": False,
        }

    def cleanup_validation(self):
        pass

    def activation_diagnostics(self):
        return {"audioCatalog": self.snapshot(), "engine": self.engine_state()}


def deploy_editor(package, targets, host, work, **options):
    require_same_engine(package, targets)
    result = deploy(package, targets, host, work, names=(EDITOR,), **options)
    result.update(mode="user-interface-update", engineReplaced=False, engineSessionStopped=False)
    if result["status"] == "passed":
        desktop = result.get("capabilities", {}).get("desktop")
        if desktop is not None:
            result["loginRegistered"] = login_state(desktop)["registered"]
    # The shared file transaction calls this guard slot "hardwareContinuity".
    # This path uses passive observations; never claim a waveform/IO check.
    if "hardwareContinuity" in result:
        result["runtimePreservation"] = result.pop("hardwareContinuity")
    if "rollbackHardwareContinuity" in result:
        result["rollbackRuntimePreservation"] = result.pop("rollbackHardwareContinuity")
    return result
