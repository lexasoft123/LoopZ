"""Independent account recovery owner, started only for a pending update."""

import fcntl
import json
import os
from pathlib import Path
import stat
import subprocess
import time

from active_session import RoutingSession
from app_login_session import login_session, restore_audio
from app_recovery_agent import cleanup_agent, read_owned
from app_recovery_context import validate_context
from app_recovery_state import AppRecoveryState, RecoveryReady
from app_recovery_workspace import AppRecoveryWorkspace, CHECKER_REQUIREMENT
from app_recovery_verification import VerifiedAppCache, component_seal
from app_update_admission import updates_directory
from app_update_journal import AppUpdateJournal
from desktop_install import TARGETS
from install_routing import command
from install_desktop import CHECKER, CONFIG
from process_code import process_generation, require_process_code, same_generation
from recover_app_update import RecoveryHost, recover
from routing_install import SUPPORT
from update_desktop import require_no_pending_maintenance, save_result, shared_backend_lock


def wait_for_update_lock(folder, on_wait=lambda: None):
    descriptor = os.open(folder / ".update.lock", os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_mode & 0o022
            or info.st_nlink != 1
        ):
            raise ValueError("The app update lock changed ownership or permissions")
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return descriptor
            except BlockingIOError:
                on_wait()
                time.sleep(0.05)
    except BaseException:
        os.close(descriptor)
        raise


def recover_owned(
    state,
    host,
    *,
    current_session,
    verify=None,
    session_factory=RoutingSession,
    workspace_factory=AppRecoveryWorkspace,
    force_runtime=False,
    trace=lambda phase: None,
):
    state.resolve_creation()
    workspace = workspace_factory(state)
    intent = state.value["intent"]
    allowed_without_journal = intent is not None and (
        force_runtime
        or restore_audio(
            state.value["session"],
            current_session,
            intent["login"]["enabled"],
            False,
        )
    )
    result = {
        "status": "passed",
        "mode": "automatic-app-recovery",
        "appFilesChanged": False,
        "interfaceReopenRequested": bool(
            allowed_without_journal and intent["interfaceWasOpen"]
        ),
    }
    with AppUpdateJournal(state.folder, TARGETS) as journal:
        if journal.pending():
            value = journal.load()
            if value["token"] != state.value["token"] or value["tools"] != state.value["tools"]:
                raise RuntimeError(
                    "The app files and recovery owner belong to different updates"
                )
            context = validate_context(value["context"], value["names"])
            session_loaded = False
            if state.value["session"] != current_session and not context["login"]["enabled"]:
                session_loaded = session_factory(host).job_state() is not None
            allowed = force_runtime or restore_audio(
                state.value["session"],
                current_session,
                context["login"]["enabled"],
                session_loaded,
            )
            options = {"verify": verify} if verify is not None else {}
            result = recover(
                journal,
                host,
                session_factory=session_factory,
                restore_runtime=allowed,
                reopen_interface=False,
                **options,
                trace=trace,
            )
            result.update(mode="automatic-app-recovery", appFilesChanged=True)
    state.finishing()
    # Keep physical continuity observation alive through graph restoration.
    # Its cooperative shutdown is needed for workspace cleanup, not app repair.
    trace("stop-guard")
    workspace.quiesce()
    trace("guard-stopped")
    trace("clean-workspace")
    workspace.clean()
    trace("workspace-cleaned")
    value = state.value
    state.remove_record()
    result.update(
        automatic=True,
        sameLoginSession=value["session"] == current_session,
        temporaryWorkspaceRemoved=True,
        transaction=value["token"],
        recoveryTools=value["tools"],
    )
    return result


def previous_failures(folder, token):
    try:
        encoded, _ = read_owned(folder / "last-result.json", os.getuid(), limit=2 * 1024 * 1024)
        value = json.loads(encoded)
        if not isinstance(value, dict):
            return []
        failures = (
            value.get("recoveryFailures", []) if value.get("transaction") == token else []
        )
        return failures[-7:] if isinstance(failures, list) else []
    except (OSError, ValueError, RuntimeError):
        return []


def update_lock_busy(folder):
    descriptor = os.open(folder / ".update.lock", os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_mode & 0o022
            or info.st_nlink != 1
        ):
            raise ValueError("The app update lock changed ownership or permissions")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return False
        except BlockingIOError:
            return True
    finally:
        os.close(descriptor)


def support_seals():
    return {str(path): component_seal(path) for path in (CHECKER, CONFIG)}


def run_ready_owner(state, tools, token, current):
    owner, folder = os.getuid(), state.folder
    started = time.monotonic()
    events = []
    failures = previous_failures(folder, token)

    def trace(phase):
        events.append({"phase": phase, "elapsedMs": (time.monotonic() - started) * 1000})

    trace("owner-started")
    lock = None
    host = None
    sealed_support = None
    verified_apps = VerifiedAppCache()
    verified_guard = None

    def preverify_guard():
        nonlocal verified_guard
        if verified_guard is not None or not state.pending():
            return
        try:
            value = state.load()
        except RuntimeError as error:
            if "changed while reading" in str(error) or "replaced while reading" in str(error):
                return
            raise
        if value["token"] != token or value["tools"] != tools or value["guard"] is None:
            return
        recorded = value["guard"]
        try:
            process = process_generation(recorded["pid"], owner)
        except ProcessLookupError:
            return
        if process != recorded:
            raise RuntimeError("The guard changed before preverification")
        trace("preverify-guard")
        require_process_code(process["pid"], CHECKER_REQUIREMENT)
        if not same_generation(process_generation(process["pid"], owner), process):
            raise RuntimeError("The guard restarted during preverification")
        with RecoveryReady(folder) as ready:
            value = ready.load()
            value["guard"] = process
            ready.save(value)
        verified_guard = process
        trace("guard-preverified")

    try:
        # The parent cannot stop audio or move an app until we acknowledge
        # readiness. Validate immutable support tools and original apps here,
        # while routing is still running, rather than in the interruption.
        if update_lock_busy(folder):
            trace("preverify-components")
            try:
                host = RecoveryHost()
                sealed_support = support_seals()
                for name, path in TARGETS.items():
                    if path.exists():
                        verified_apps(name, path)
                trace("components-preverified")
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
                host = None
                trace("preverification-unavailable")
        with RecoveryReady(folder) as ready:
            if ready.pending():
                old = ready.load()
                if old["token"] != token or old["tools"] != tools:
                    raise RuntimeError("Another update owns the readiness acknowledgement")
            ready.save(
                {
                    "version": 1,
                    "token": token,
                    "tools": tools,
                    "process": process_generation(os.getpid(), owner),
                    "session": current,
                    "guard": None,
                },
                initial=ready.value is None,
            )
        trace("ready")
        lock = wait_for_update_lock(folder, on_wait=preverify_guard)
        trace("update-lock-acquired")
        if not state.pending():
            cleanup_agent(folder, token, tools)
            return
        value = state.load()
        if value["token"] != token or value["tools"] != tools:
            cleanup_agent(folder, token, tools)
            return
        with shared_backend_lock(SUPPORT / ".install.lock"):
            trace("backend-lock-acquired")
            require_no_pending_maintenance(SUPPORT)
            if host is None or support_seals() != sealed_support:
                host = RecoveryHost()
            trace("host-verified")
            if host.uid != owner or os.stat("/dev/console").st_uid != owner:
                raise RuntimeError("Wait for the owning account to become the active console")
            result = recover_owned(
                state,
                host,
                current_session=current,
                trace=trace,
                verify=verified_apps,
            )
            trace("recovery-complete")
            cleanup_agent(folder, token, tools, bootout=False)
            trace("agent-files-removed")
            if result.get("interfaceReopenRequested"):
                host.user_command("/usr/bin/open", TARGETS["LoopZ.app"])
            trace("interface-restored")
            result.update(
                recoveryTrace=events, recoveryFailures=failures, recoveryOwnerPID=os.getpid()
            )
            save_result(folder, result)
            print(json.dumps(result), flush=True)
    except BaseException as error:
        trace("attempt-failed")
        failures.append({"pid": os.getpid(), "error": str(error)[:4096], "trace": events})
        save_result(
            folder,
            {
                "status": "retrying",
                "mode": "automatic-app-recovery",
                "automatic": True,
                "transaction": token,
                "recoveryTools": tools,
                "recoveryFailures": failures[-8:],
            },
        )
        raise
    finally:
        if lock is not None:
            os.close(lock)


def main(tools, token):
    owner = os.getuid()
    if owner < 501 or os.geteuid() != owner:
        raise ValueError("Automatic app recovery cannot run with administrator authority")
    folder = updates_directory(owner)
    current = login_session()
    with AppRecoveryState(folder) as state:
        if not state.pending():
            cleanup_agent(folder, token, tools)
            return 0
        value = state.load()
        if value["token"] != token or value["tools"] != tools:
            cleanup_agent(folder, token, tools)
            return 0
        run_ready_owner(state, tools, token, current)
    # All receipts and cleanup precede removing our own loaded job. No other
    # account, Engine label, driver host, broker or Core Audio job is signalled.
    command(
        "/bin/launchctl",
        "bootout",
        f"gui/{owner}/com.lexasoft.loopz.update-recovery.{token}",
        check=False,
    )
    return 0
