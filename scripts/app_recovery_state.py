"""Durable, fixed-namespace lifetime of a temporary account update recovery job."""

import copy
import os
from pathlib import Path
import re
import stat
import uuid

from app_login_session import validate_session
from app_recovery_context import validate_context
from app_recovery_tools import require_private_directory
from desktop_files import identity
from process_record import validate_process
from protected_record import ProtectedRecord
from uninstall_files import open_directory

NAME = ".app-recovery-state.json"
READY = ".app-recovery-ready.json"
CHECKER = Path("/Library/Application Support/LoopZ/loopz-routing-install-check")


def validate_hex(value, length):
    if not isinstance(value, str) or not re.fullmatch(rf"[a-f0-9]{{{length}}}", value):
        raise ValueError("Invalid recovery transaction or tool identity")


class AppRecoveryState(ProtectedRecord):
    def __init__(self, folder):
        super().__init__(folder, NAME)
        if os.getuid() < 501 or os.geteuid() != os.getuid():
            self.close()
            raise ValueError("Account update recovery cannot use administrator authority")

    def validate(self, value):
        if not isinstance(value, dict) or set(value) != {
            "version",
            "owner",
            "token",
            "tools",
            "session",
            "workspace",
            "guard",
            "phase",
            "intent",
        }:
            raise ValueError("Invalid app recovery lifetime record")
        if (
            type(value["version"]) is not int
            or value["version"] != 1
            or type(value["owner"]) is not int
            or value["owner"] != self.owner
        ):
            raise ValueError("Unsupported recovery lifetime version or owner")
        validate_hex(value["token"], 32)
        validate_hex(value["tools"], 64)
        validate_session(value["session"])
        if value["phase"] not in ("creating", "working", "finishing"):
            raise ValueError("Unknown recovery lifetime phase")
        node = value["workspace"]
        if node is None:
            if value["phase"] not in ("creating", "finishing"):
                raise ValueError("The prepared recovery workspace is missing its identity")
        elif (
            not isinstance(node, list)
            or len(node) != 2
            or any(type(part) is not int or part <= 0 for part in node)
        ):
            raise ValueError("Invalid recovery workspace identity")
        if value["guard"] is not None:
            validate_process(value["guard"], owner=self.owner, executable=CHECKER)
        if value["intent"] is not None:
            mode = value["intent"].get("mode") if isinstance(value["intent"], dict) else None
            names = ["LoopZ.app"] if mode == "interface" else ["LoopZ.app", "LoopZ Engine.app"]
            validate_context(value["intent"], names)

    @property
    def work(self):
        return self.folder / "Work" / self.value["token"]

    def begin(self, tools, session):
        if self.pending():
            raise RuntimeError("Finish the interrupted app update recovery first")
        validate_hex(tools, 64)
        require_private_directory(self.folder / "Work", self.owner)
        self.save(
            {
                "version": 1,
                "owner": self.owner,
                "token": uuid.uuid4().hex,
                "tools": tools,
                "session": session,
                "workspace": None,
                "guard": None,
                "phase": "creating",
                "intent": None,
            },
            initial=True,
        )
        self.work.mkdir(mode=0o700)
        descriptor = open_directory(self.work)
        try:
            value = copy.deepcopy(self.value)
            value.update(workspace=list(identity(os.fstat(descriptor))), phase="working")
            self.save(value)
        finally:
            os.close(descriptor)

    def open_work(self):
        try:
            descriptor = open_directory(self.work)
        except FileNotFoundError:
            return None
        info = os.fstat(descriptor)
        if (
            info.st_uid != self.owner
            or stat.S_IMODE(info.st_mode) != 0o700
            or self.value["workspace"] is not None
            and list(identity(info)) != self.value["workspace"]
        ):
            os.close(descriptor)
            raise RuntimeError("The update workspace identity or privacy changed")
        if self.value["workspace"] is None and os.listdir(descriptor):
            os.close(descriptor)
            raise RuntimeError("Unrecorded workspace content is retained for inspection")
        return descriptor

    def guard_started(self, process):
        value = copy.deepcopy(self.value)
        if value["guard"] is not None:
            raise RuntimeError("A recovery lifetime cannot replace its recorded guard")
        value["guard"] = process
        self.save(value)

    def record_intent(self, context):
        value = copy.deepcopy(self.value)
        if value["intent"] is not None and value["intent"] != context:
            raise RuntimeError("An update cannot replace its recorded startup intent")
        value["intent"] = copy.deepcopy(context)
        self.save(value)

    def resolve_creation(self):
        if self.value["workspace"] is not None:
            return
        descriptor = self.open_work()
        if descriptor is not None:
            try:
                value = copy.deepcopy(self.value)
                value.update(workspace=list(identity(os.fstat(descriptor))), phase="working")
                self.save(value)
            finally:
                os.close(descriptor)

    def finishing(self):
        value = copy.deepcopy(self.value)
        value["phase"] = "finishing"
        self.save(value)


class RecoveryReady(ProtectedRecord):
    def __init__(self, folder):
        super().__init__(folder, READY)

    def validate(self, value):
        required = {"version", "token", "tools", "process", "session"}
        if not isinstance(value, dict) or set(value) not in (required, required | {"guard"}):
            raise ValueError("Invalid update recovery readiness record")
        if type(value["version"]) is not int or value["version"] != 1:
            raise ValueError("Unsupported update recovery readiness version")
        validate_hex(value["token"], 32)
        validate_hex(value["tools"], 64)
        validate_process(value["process"], owner=self.owner)
        validate_session(value["session"])
        if value.get("guard") is not None:
            validate_process(value["guard"], owner=self.owner, executable=CHECKER)
