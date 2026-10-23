"""Preserve canonical routing intent across a user-owned Engine replacement."""

import copy
import json
import os
from pathlib import Path
import plistlib
import re
import signal
import subprocess
import time

from desktop_install import IDENTIFIERS, TARGETS, requirement
from app_update_admission import app_update_pending
from install_routing import command, verify_code
from install_transaction import check_ancestors
from process_code import process_generation, require_process_code, same_generation
from suite_update import unique_object

SESSION = "com.lexasoft.loopz.session"
ENGINE = "LoopZ Engine.app"
EDITOR = "LoopZ.app"


def session_definition(routes):
    return {
        "Label": SESSION,
        "ProgramArguments": [
            str(TARGETS[ENGINE] / "Contents/MacOS/LoopZ Engine"),
            "--routing-document", str(routes), "--editor-service", "true", "--block-frames", "64",
        ],
        "RunAtLoad": True,
        "KeepAlive": True,
        "MachServices": {"com.lexasoft.loopz.editor.control": True},
        "ProcessType": "Interactive",
        "ThrottleInterval": 10,
        "ExitTimeOut": 5,
        "StandardOutPath": "/dev/null",
        "StandardErrorPath": "/dev/null",
    }


def committed(state):
    restoration = state.get("restoration", {})
    return (
        restoration.get("phase") == "restored"
        and restoration.get("activeGeneration") == restoration.get("desiredGeneration")
        and restoration.get("activeGeneration") not in (None, "0", 0)
    )


def contract(state, document):
    runtime = state.get("runtime", {})
    if (
        state.get("version") != 1
        or state.get("document") != document
        or document.get("version") != 2
        or not isinstance(document.get("devices"), list)
        or len(document["devices"]) > 32
        or runtime.get("started") is not True
        or runtime.get("documentError")
        or runtime.get("controlError")
        or runtime.get("renderError")
        or not isinstance(state.get("revision", {}).get("epoch"), str)
    ):
        raise RuntimeError("The Engine has not acknowledged the saved routing intent")
    hardware = state.get("hardware", {})
    preferences = hardware.get("bufferPreferences", [])
    if hardware.get("bufferSettingsError"):
        raise RuntimeError("Recover saved audio settings before replacing the Engine")
    return {
        "document": copy.deepcopy(document),
        "epoch": state["revision"]["epoch"],
        "committed": committed(state),
        "rendering": runtime.get("rendering") is True,
        "fullyRestored": runtime.get("restored") is True,
        "sources": [
            (s["kind"], s["selector"])
            for s in runtime.get("sources", [])
            if s.get("demanded") and s.get("connected") and s.get("complete")
        ],
        "monitors": [
            (m["device"], m["monitor"])
            for m in runtime.get("monitors", [])
            if m.get("connected") and m.get("complete")
        ],
        "buffers": {p["uid"]: p["frames"] for p in preferences},
        "appliedBuffers": [p["uid"] for p in preferences if p.get("applied")],
        "microphoneAuthorized": runtime.get("permissions", {}).get("microphone") == "authorized",
    }


def same_routing_intent(actual, expected):
    """Device recreation may refresh its internal driver-format receipt."""
    def intent(document):
        result = copy.deepcopy(document)
        if isinstance(result, dict) and isinstance(result.get("devices"), list):
            for device in result["devices"]:
                if isinstance(device, dict):
                    device.pop("formatRequestReceipt", None)
        return result

    return intent(actual) == intent(expected)


def restored(state, expected):
    if not same_routing_intent(state.get("document"), expected["document"]):
        return False
    runtime = state.get("runtime", {})
    if (
        state.get("revision", {}).get("epoch") in (None, expected["epoch"])
        or runtime.get("started") is not True
        or runtime.get("documentError")
        or runtime.get("controlError")
        or runtime.get("renderError")
    ):
        return False
    if expected["committed"] and not committed(state):
        return False
    render_needed = any(
        device.get("routing", {}).get("enabled", True)
        for device in expected["document"]["devices"]
    )
    if render_needed and expected["rendering"] and runtime.get("rendering") is not True:
        return False
    if expected["fullyRestored"] and runtime.get("restored") is not True:
        return False
    sources = {
        (s["kind"], s["selector"])
        for s in runtime.get("sources", [])
        if s.get("connected") and s.get("complete")
    }
    monitors = {
        (m["device"], m["monitor"])
        for m in runtime.get("monitors", [])
        if m.get("connected") and m.get("complete")
    }
    if not set(expected["sources"]) <= sources or not set(expected["monitors"]) <= monitors:
        return False
    hardware = state.get("hardware", {})
    preferences = hardware.get("bufferPreferences", [])
    if (
        hardware.get("bufferSettingsError")
        or {p["uid"]: p["frames"] for p in preferences} != expected["buffers"]
    ):
        return False
    if not set(expected["appliedBuffers"]) <= {p["uid"] for p in preferences if p.get("applied")}:
        return False
    if (
        expected["microphoneAuthorized"]
        and runtime.get("permissions", {}).get("microphone") != "authorized"
    ):
        return False
    if expected["committed"]:
        ids = {d["id"] for d in expected["document"]["devices"]}
        if {d["id"] for d in runtime.get("devices", [])} != ids:
            return False
    return True


class RoutingSession:
    def __init__(self, host):
        self.host = host
        self.folder = Path(host.user.pw_dir) / "Library/Application Support/LoopZ"
        self.routes = self.folder / "routes.json"
        self.job = self.folder / "Session/engine.plist"
        self.label = f"gui/{host.uid}/{SESSION}"
        self.definition = None
        self.document_bytes = None
        self.expected = None
        self.original_engine = None
        self.paused = False
        self.ui_closed = False
        self.pause_started = None
        self.last_acknowledgement = None

    def snapshot(self):
        response = self.host.user_command(
            TARGETS[EDITOR] / "Contents/MacOS/LoopZ", "--routing-status", timeout=3
        )
        return json.loads(response.stdout)

    def job_state(self):
        result = self.host.user_command("/bin/launchctl", "print", self.label, check=False)
        if result.returncode:
            if (
                result.returncode not in (3, 113)
                or "Could not find service" not in result.stdout + result.stderr
            ):
                raise RuntimeError("The canonical Engine session could not be inspected")
            return None
        lines = {line.strip() for line in result.stdout.splitlines()}
        if (
            f"path = {self.job}" not in lines
            or f"program = {TARGETS[ENGINE]}/Contents/MacOS/LoopZ Engine" not in lines
        ):
            raise RuntimeError("A different job occupies the canonical Engine label")
        found = re.search(r"^\s*pid = (\d+)\s*$", result.stdout, re.M)
        if not found:
            return {"loaded": True, "process": None}
        try:
            process = process_generation(int(found[1]), self.host.uid)
        except ProcessLookupError:
            return {"loaded": True, "process": None}
        if process["executable"] != str(TARGETS[ENGINE] / "Contents/MacOS/LoopZ Engine"):
            raise RuntimeError("The Engine job is running a different executable")
        require_process_code(process["pid"], requirement(IDENTIFIERS[ENGINE]))
        if not same_generation(process_generation(process["pid"], self.host.uid), process):
            raise RuntimeError("The Engine generation changed during inspection")
        return {"loaded": True, "process": process}

    def read_intent(self):
        self.host.protected_state()
        check_ancestors([self.routes, self.job])
        definition = self.job.read_bytes()
        saved = plistlib.loads(definition)
        expected = session_definition(self.routes)
        if (
            saved != expected
            or any(type(saved[key]) is not type(value) for key, value in expected.items())
            or saved["MachServices"]["com.lexasoft.loopz.editor.control"] is not True
        ):
            raise RuntimeError("The canonical Engine definition has unsupported arguments")
        if self.definition is not None and self.definition != definition:
            raise RuntimeError("The canonical Engine definition changed during review")
        self.definition = definition
        document_bytes = self.routes.read_bytes()
        if len(document_bytes) > 16 * 1024 * 1024:
            raise RuntimeError("The saved routing document exceeds its size limit")
        if self.document_bytes is not None and document_bytes != self.document_bytes:
            raise RuntimeError("Routing changed during review; refresh the update before retrying")
        self.document_bytes = document_bytes
        document = json.loads(document_bytes, object_pairs_hook=unique_object)
        if (
            not isinstance(document, dict)
            or type(document.get("version")) is not int
            or document["version"] != 2
            or not isinstance(document.get("devices"), list)
            or len(document["devices"]) > 32
        ):
            raise RuntimeError("Recover the saved routing document before restoring apps")
        return document

    def inspect(self):
        document = self.read_intent()
        job = self.job_state()
        if not job or not job["process"]:
            raise RuntimeError("Start the canonical Engine session before updating apps")
        if self.original_engine and not same_generation(self.original_engine, job["process"]):
            raise RuntimeError("The Engine restarted during update review")
        self.original_engine = job["process"]
        state = self.snapshot()
        candidate = contract(state, document)
        if self.expected is not None and candidate["epoch"] != self.expected["epoch"]:
            raise RuntimeError("The authenticated Engine changed during update review")
        self.expected = candidate
        return state

    def close_interface(self, *, recheck=True):
        found = command("/usr/bin/pgrep", "-x", "LoopZ", check=False)
        if found.returncode not in (0, 1):
            raise RuntimeError("Desktop processes could not be inspected")
        verified = []
        for value in found.stdout.split():
            process = process_generation(int(value), self.host.uid)
            if process["executable"] != str(TARGETS[EDITOR] / "Contents/MacOS/LoopZ"):
                raise RuntimeError("A different executable uses the desktop process name")
            require_process_code(process["pid"], requirement(IDENTIFIERS[EDITOR]))
            verified.append(process)
        for process in verified:
            if not same_generation(process, process_generation(process["pid"], self.host.uid)):
                raise RuntimeError("The desktop generation changed before closing")
            self.ui_closed = True
            os.kill(process["pid"], signal.SIGTERM)
            until = time.monotonic() + 8
            while True:
                try:
                    current = process_generation(process["pid"], self.host.uid)
                    if not same_generation(process, current):
                        break
                except ProcessLookupError:
                    break
                if time.monotonic() >= until:
                    raise RuntimeError(
                        "The desktop did not finish closing; routing was left running"
                    )
                time.sleep(0.05)
        # Closing preview releases unused captures; do not require those to open
        # during restoration when no actual recording client or monitor needs them.
        time.sleep(0.1)
        if recheck:
            self.inspect()

    def unchanged_files(self):
        check_ancestors([self.routes, self.job])
        if (
            self.job.read_bytes() != self.definition
            or self.routes.read_bytes() != self.document_bytes
        ):
            raise RuntimeError("Saved routing/session intent changed; no file will be overwritten")

    def pause_engine(self):
        self.inspect()
        self.pause_started = time.monotonic()
        self.paused = True
        self.host.user_command("/bin/launchctl", "bootout", self.label)
        self.wait_stopped()

    def wait_stopped(self):
        until = time.monotonic() + 8
        while self.job_state() is not None:
            if time.monotonic() >= until:
                raise RuntimeError("The canonical Engine did not finish stopping")
            time.sleep(0.05)

    def stop_for_rollback(self):
        self.unchanged_files()
        if self.job_state() is not None:
            self.paused = True
            self.host.user_command("/bin/launchctl", "bootout", self.label)
            self.wait_stopped()

    def restore(self):
        self.unchanged_files()
        if self.job_state() is None:
            verify_code(
                TARGETS[ENGINE], IDENTIFIERS[ENGINE], requirement(IDENTIFIERS[ENGINE]), True
            )
            self.host.user_command("/bin/launchctl", "bootstrap", self.host.domain, self.job)
        until = time.monotonic() + 20
        while time.monotonic() < until:
            try:
                state = self.snapshot()
                if restored(state, self.expected):
                    self.last_acknowledgement = state
                    self.paused = False
                    return state
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
                pass
            time.sleep(0.1)
        raise RuntimeError("The replacement Engine did not restore the saved routing contract")

    def restore_current(self):
        """Explicit recovery loads current saved intent; it never writes an older mix."""
        self.unchanged_files()
        document = json.loads(self.document_bytes)
        if self.job_state() is None:
            verify_code(
                TARGETS[ENGINE], IDENTIFIERS[ENGINE], requirement(IDENTIFIERS[ENGINE]), True
            )
            self.host.user_command("/bin/launchctl", "bootstrap", self.host.domain, self.job)
        until = time.monotonic() + 20
        while time.monotonic() < until:
            try:
                state = self.snapshot()
                contract(state, document)
                runtime = state["runtime"]
                if (
                    committed(state)
                    and (
                        runtime.get("rendering") is True
                        or (
                            not any(
                                device.get("routing", {}).get("enabled", True)
                                for device in document["devices"]
                            )
                            and runtime.get("restored") is True
                        )
                    )
                    and {item["id"] for item in runtime.get("devices", [])}
                    == {item["id"] for item in document["devices"]}
                ):
                    self.last_acknowledgement = state
                    self.paused = False
                    return state
            except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError):
                pass
            time.sleep(0.1)
        raise RuntimeError("The recovered Engine did not acknowledge the current saved graph")

    def reopen_interface(self):
        if self.ui_closed and not app_update_pending(self.host.uid):
            self.host.user_command("/usr/bin/open", TARGETS[EDITOR])
            self.ui_closed = False
