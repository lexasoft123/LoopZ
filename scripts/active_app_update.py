"""Prepared app-pair replacement and restoration of the existing routing graph."""

from functools import partial
import time

from active_session import RoutingSession
from desktop_files import DesktopFileTransaction
from desktop_install import IDENTIFIERS, NAMES, TARGETS, deploy, requirement
from editor_update import EditorUpdateHost, login_state, require_unchanged_login
from install_desktop import DesktopHost, verify_code
from install_routing import command


class RoutingFileTransaction(DesktopFileTransaction):
    def __init__(self, *args, session, **kwargs):
        super().__init__(*args, **kwargs)
        self.session = session

    def apply(self):
        # Preparation/fsync finishes while the old Engine still processes audio.
        self.session.pause_engine()
        super().apply()

    def rollback(self):
        self.session.stop_for_rollback()
        super().rollback()
        self.session.restore()


class ActiveAppHost(DesktopHost):
    allow_pending_app_update = True

    def __init__(self, guard_uid=None):
        super().__init__(guard_uid)
        self.expected_login = None
        self.session = RoutingSession(self)
        self.recovery = None

    def start_guard(self, work, before):
        guard = super().start_guard(work, before)
        try:
            if self.recovery is not None:
                self.recovery.observe_guard(guard)
            return guard
        except BaseException:
            guard.close()
            raise

    def existing_apps(self, package, replace_engine, replace_editor):
        if not replace_engine or not replace_editor:
            raise ValueError("A paired application update must review both bundles")
        for name in NAMES:
            verify_code(
                TARGETS[name],
                IDENTIFIERS[name],
                requirement(IDENTIFIERS[name]),
                name == "LoopZ Engine.app",
            )
        current = login_state(self.app_status())
        if not current["available"]:
            raise RuntimeError("The installed desktop is unavailable")
        if self.expected_login is not None and self.expected_login != current:
            raise RuntimeError("Login startup changed during update review")
        self.expected_login = current
        if current["registered"]:
            require_unchanged_login(package)

    def require_idle(self, snapshot):
        # Existing devices and public clients remain live. Only the GUI closes.
        running = command("/usr/bin/pgrep", "-x", "LoopZ", check=False)
        if running.returncode not in (0, 1) or running.stdout.strip():
            raise RuntimeError("Close the desktop before promoting the app pair")
        self.session.inspect()

    def protected_state(self):
        return {**super().protected_state(), "loginStartup": login_state(self.app_status())}

    def validate_apps(self):
        # Includes the editor, unchanged registered helper and current login state.
        return EditorUpdateHost.validate_apps(self)

    def validate_activation(self, work):
        state = self.session.restore()
        return {
            "mode": "saved-routing-restoration",
            "validationDevicePublished": False,
            "savedDocumentPreserved": True,
            "deviceCount": len(state["document"]["devices"]),
            "restorationPending": state["runtime"]["restored"] is not True,
            "restorationObservedMs": (time.monotonic() - self.session.pause_started) * 1000,
            "acknowledgement": state,
        }

    def cleanup_validation(self):
        # No installer graph or second Engine was started. Keep restored routing.
        pass

    def activation_diagnostics(self):
        result = {"audioCatalog": self.snapshot()}
        try:
            result["engine"] = self.session.snapshot()
        except Exception as error:
            result["engineError"] = str(error)
        return result


def deploy_active(package, targets, host, work, transaction_factory=RoutingFileTransaction):
    factory = partial(transaction_factory, session=host.session)
    result = deploy(package, targets, host, work, transaction_factory=factory)
    result.update(mode="user-active-app-update", originalRoutingDocumentRetained=True)
    if result.get("capabilities"):
        result["loginRegistered"] = login_state(result["capabilities"]["desktop"])["registered"]
    if host.session.last_acknowledgement is not None:
        result["currentEngineAcknowledgement"] = host.session.last_acknowledgement
    return result
