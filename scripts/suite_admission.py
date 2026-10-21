"""Read-only macOS admission and preservation for the coordinated update.

The four mutable components are deliberately excluded from preservation. Paths
and commands come from product policy, never from a recovery journal. Recorded
UIDs can extend document observation to previously signed-in accounts, but only
the live launchd catalog can select accounts in which status code is executed.
"""

import os
from pathlib import Path
import pwd

from driver_update import DriverUpdateHost
from install_desktop import CHECKER, CONFIG
from install_routing import command
from routing_install import SUPPORT
from suite_update import TARGETS
from uninstall_files import RemovalFiles, open_directory
from uninstall_host import UninstallHost
from uninstall_sessions import (
    SessionInspector,
    require_sessions_off,
    validate_session_preservation,
)

DEFAULTS = ("defaultInput", "defaultOutput", "defaultSystemOutput")
PRESERVED = {
    "checker": CHECKER,
    "config": CONFIG,
    "pin": SUPPORT / "routing-service.json",
    "pinChecker": SUPPORT / "loopz-routing-pin-check",
    "daemon": Path("/Library/LaunchDaemons/com.lexasoft.loopz.routing.plist"),
    "separateDriver": Path("/Library/Audio/Plug-Ins/HAL/LoopZRoutingAudio.driver"),
    "legacyBroker": SUPPORT / "loopz-broker",
    "legacyProbe": SUPPORT / "loopz-probe",
    "legacyChecker": SUPPORT / "loopz-install-check",
    "legacyDaemon": Path("/Library/LaunchDaemons/com.lexasoft.loopz.bridge.plist"),
    "routingProbe": SUPPORT / "routing/loopz-probe",
    "routingAgent": SUPPORT / "routing/com.lexasoft.loopz.engine.plist",
    "desktopReceipt": SUPPORT / "desktop-install-result.json",
    "routingReceipt": SUPPORT / "routing-install-result.json",
}
DOCUMENTS = ("routes.json", "routes.json.backup", "Session/engine.plist", "Session/session.lock")


def document_uids(values):
    values = set(values)
    if len(values) > 512 or any(
        type(uid) is not int or not 0 < uid <= 0xFFFFFFFF for uid in values
    ):
        raise ValueError("Invalid bounded document-account inventory")
    return sorted(values)


def user_documents(uid):
    user = pwd.getpwuid(uid)
    support = Path(user.pw_dir) / "Library/Application Support/LoopZ"
    try:
        descriptor = open_directory(support)
    except FileNotFoundError:
        exists = False
    else:
        try:
            info = os.fstat(descriptor)
            if info.st_uid != uid or info.st_mode & 0o022:
                raise RuntimeError("User routing directory has unsafe ownership or permissions")
            exists = True
        finally:
            os.close(descriptor)
    with RemovalFiles({name: support / name for name in DOCUMENTS}, owner=uid) as reader:
        return {"directoryExists": exists, "files": reader.inventory()}


class SuiteAdmission(DriverUpdateHost):
    def __init__(self):
        super().__init__()
        self.session_admission = None
        self.recovering_sessions = False
        self.document_users = []

    def login_for_uid(self, uid):
        return UninstallHost.login_for_uid(self, uid)

    def sessions(self):
        available = TARGETS["LoopZ.app"].exists()
        state = SessionInspector(command, self.login_for_uid).snapshot(self.uid, available)
        require_sessions_off(
            state, self.session_admission, allow_new_generation=self.recovering_sessions
        )
        return state

    def preserved(self, accounts):
        with RemovalFiles(PRESERVED, owner=0) as reader:
            protected = reader.inventory()
        return {
            "components": protected,
            "documents": {str(uid): user_documents(uid) for uid in document_uids(accounts)},
        }

    def capture(self):
        if os.stat("/dev/console").st_uid != self.uid:
            raise RuntimeError("The console account changed during suite update admission")
        catalog = self.snapshot()
        self.require_idle(catalog)
        sessions = self.sessions()
        users = document_uids([*self.document_users, *[a["uid"] for a in sessions["accounts"]]])
        before = {
            "consoleUID": self.uid,
            "coreAudio": self.service_identity(),
            "defaults": {key: catalog[key] for key in DEFAULTS},
            "sessions": sessions,
            "preserved": self.preserved(users),
        }
        if os.stat("/dev/console").st_uid != self.uid:
            raise RuntimeError("The console account changed during suite update admission")
        return before

    def admit(self, baseline):
        if not isinstance(baseline, dict) or baseline.get("consoleUID") != self.uid:
            raise RuntimeError("No matching console baseline for coordinated update admission")
        current = self.capture()
        if current["defaults"] != baseline["defaults"]:
            raise RuntimeError("Audio defaults changed during the coordinated update")
        if current["coreAudio"] != baseline["coreAudio"]:
            raise RuntimeError("Core Audio changed during the coordinated update")
        if current["preserved"] != baseline["preserved"]:
            raise RuntimeError("Protected components or user documents changed during the update")
        validate_session_preservation(baseline["sessions"], current["sessions"])

    def recovery_baseline(self, original):
        if original["consoleUID"] != self.uid:
            raise RuntimeError("Sign in to the original console account before suite recovery")
        require_sessions_off(original["sessions"])
        documents = original["preserved"].get("documents")
        if not isinstance(documents, dict) or any(
            not isinstance(key, str) or not key.isdecimal() or str(int(key)) != key
            for key in documents
        ):
            raise ValueError("Original document account inventory is incomplete")
        self.document_users = document_uids(int(key) for key in documents)
        self.session_admission = original["sessions"]
        self.recovering_sessions = True
        # Keep actual desktop availability in the fresh baseline. Admission
        # compares its GUI identities on every transition, while a missing
        # desktop can borrow only the original account's recorded off choice.
        return self.capture()
