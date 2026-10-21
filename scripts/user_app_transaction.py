"""Durable user-app promotion; runtime acknowledgement precedes the commit record."""

import hashlib
import os
from pathlib import Path
import shutil

from app_recovery_context import validate_context
from desktop_files import DesktopFileTransaction, identity
from desktop_install import IDENTIFIERS, requirement
from install_routing import verify_code
from uninstall_files import rename_exclusive


def verify_component(name, path):
    verify_code(
        path, IDENTIFIERS[name], requirement(IDENTIFIERS[name]), name == "LoopZ Engine.app"
    )
    if name == "LoopZ.app":
        key = name + "/Contents/MacOS/LoopZLogin"
        verify_code(
            path / "Contents/MacOS/LoopZLogin",
            IDENTIFIERS[key],
            requirement(IDENTIFIERS[key]),
            False,
        )


class UserAppTransaction(DesktopFileTransaction):
    def __init__(
        self,
        *arguments,
        journal,
        context,
        session=None,
        verify=verify_component,
        recovery=None,
    ):
        super().__init__(*arguments)
        validate_context(context, self.names)
        self.journal = journal
        self.context = context
        self.session = session
        self.verify = verify
        self.activation_attempted = False
        self.recovery = recovery
        if any(self.targets[name] != journal.targets[name] for name in self.names):
            raise ValueError("The transaction must use the journal's fixed app locations")
        if (context["mode"] == "routing") != (session is not None):
            raise ValueError("Only paired app transactions may manage the routing session")
        if recovery is not None:
            if self.stage.parent != recovery.work:
                raise ValueError(
                    "The prepared package must belong to the managed recovery workspace"
                )
            self.token = recovery.state.value["token"]
            recovery.state.record_intent(context)

    def prepare(self):
        tools = (
            self.recovery.tools
            if self.recovery is not None
            else hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        )
        self.journal.begin(self, self.context, tools)
        super().prepare()
        self.journal.phase("prepared")

    def will_prepare(self, name, prepared):
        self.journal.prepared_node(name, prepared)

    def apply(self):
        if self.recovery is not None:
            self.recovery.require_ready()
        self.journal.phase("activating")
        self.activation_attempted = True
        if self.session is not None:
            self.session.pause_engine()
        super().apply()
        self.journal.phase("validating")

    def _promote(self, name):
        target = self.targets[name]
        parent = self.parent(target)
        if list(identity(os.fstat(parent))) != self.journal.value["parents"][name]:
            raise RuntimeError("An application parent changed before promotion")
        candidate = self.candidates[name]
        self.journal.require_match(target, self.journal.value["old"][name])
        self.journal.require_match(candidate, self.journal.value["new"][name])
        backup = self.journal.slot(name, "old")
        rename_exclusive(parent, parent, target.name, backup.name)
        self.backups[name] = backup
        self.expected[backup] = tuple(
            self.journal.value["old"][name][key] for key in ("device", "inode")
        )
        self.changed.append(name)
        os.fsync(parent)
        rename_exclusive(parent, parent, candidate.name, target.name)
        self.expected[target] = self.expected[candidate]
        os.fsync(parent)

    def rollback(self):
        if self.session is not None and self.activation_attempted:
            self.session.stop_for_rollback()
        self.journal.restore_files(self.verify)
        if self.session is not None and self.activation_attempted:
            self.session.restore()
        self.changed.clear()

    def commit(self):
        # Called only after signed apps, routing, protected state and IO guards pass.
        self.journal.phase("committed")

    def finish(self):
        try:
            if self.journal.value is not None and self.journal.pending():
                phase = self.journal.value["phase"]
                if phase in ("preparing", "prepared"):
                    # No session stop or promotion has occurred at these phases.
                    self.journal.restore_files(self.verify)
                self.journal.finish(
                    self.verify,
                    committed=self.journal.value["phase"] in ("committed", "committed_cleanup"),
                )
            if self.prepared and self.prepared.exists():
                shutil.rmtree(self.prepared)
        finally:
            self.close()
