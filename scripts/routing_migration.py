"""Guard an explicit migration of the existing, isolated LoopZ HAL slot."""

import json
import os
from pathlib import Path
import subprocess
import stat
import time

import driver_host
from install_routing import MacHost, command
from install_transaction import blocked_signals, validate_after
from routing_install import SUPPORT, TARGETS

DRIVER_KEY = "LoopZRoutingAudio.driver"
DRIVER = Path("/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver")
BINARY = DRIVER / "Contents/MacOS/LoopZAudio"
MIGRATION_TARGETS = {**TARGETS, DRIVER_KEY: DRIVER}


def validate_legacy_checker(checker):
    # Path.stat(follow_symlinks=...) is unavailable in macOS's Python 3.9.
    # lstat also rejects a symlink without following its target.
    info = checker.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise RuntimeError("The legacy bridge checker must be root-owned and protected")


def read_legacy_bridge_status(checker):
    validate_legacy_checker(checker)
    return json.loads(command(checker, "--bridge-status").stdout)


class LegacySlotHost(MacHost):
    activation_mode = "existing-loopz-slot-migration"

    def __init__(self, package, guard_uid=None):
        super().__init__(package, guard_uid)
        self.original_host = None
        self.original_snapshot = None
        self.original_core_audio = None
        self.signalled = False
        self.rollback_host = None

    def legacy_peer(self):
        return read_legacy_bridge_status(SUPPORT / "loopz-install-check")

    def require_idle(self, snapshot):
        super().require_idle(snapshot)
        driver_host.require_idle(snapshot)
        if TARGETS[DRIVER_KEY].exists() or snapshot.get("routingPlugIn", {}).get("registered"):
            raise RuntimeError("A separate routing-driver slot is already present")
        validate_legacy_checker(SUPPORT / "loopz-install-check")
        if not os.geteuid() and self.original_host is None:
            peer = self.legacy_peer()
            if not peer.get("driverAttached") or peer.get("helperAttached"):
                raise RuntimeError("The isolated legacy driver must be attached without capture")
            self.original_host = driver_host.inspect(peer.get("driverPID", 0), BINARY)
            self.original_snapshot = snapshot
            self.original_core_audio = self.service_identity()

    def protected_state(self):
        result = super().protected_state()
        # This explicit mode replaces only this driver slot. The legacy broker,
        # checker, probe and daemon remain available to authenticate rollback.
        result.pop(str(DRIVER))
        return result

    def transition_driver(self, transaction, guard):
        if not self.original_host or DRIVER_KEY not in transaction.backups:
            raise RuntimeError("A verified original driver and rollback bundle are required")
        peer = self.legacy_peer()
        if (
            not peer.get("driverAttached")
            or peer.get("helperAttached")
            or peer.get("driverPID") != self.original_host["pid"]
        ):
            raise RuntimeError("The resident legacy driver did not authenticate")
        backup = transaction.backups[DRIVER_KEY]
        audited = driver_host.inspect(
            self.original_host["pid"],
            backup / "Contents/MacOS/LoopZAudio",
            {DRIVER.name, backup.name},
        )
        for field in (
            "pid",
            "uid",
            "startSeconds",
            "startMicroseconds",
            "executable",
            "device",
            "inode",
        ):
            if audited[field] != self.original_host[field]:
                raise RuntimeError("The original driver host generation changed")
        self.require_idle(self.snapshot())
        if self.service_identity() != self.original_core_audio:
            raise RuntimeError("Core Audio changed before the isolated transition")
        guard.check()
        with blocked_signals():
            driver_host.signal_checked(self.original_host, force=True)
            self.signalled = True

    def validate_activation(self, work):
        result = super().validate_activation(work)
        replacement = driver_host.discover(BINARY)
        if replacement["pid"] == self.original_host["pid"]:
            raise RuntimeError("The driver host was not replaced")
        result["isolatedDriverHost"] = replacement
        result["originalDriverHost"] = self.original_host
        return result

    def prepare_driver_rollback(self, transaction):
        del transaction
        if not self.signalled:
            return
        try:
            self.rollback_host = driver_host.discover(BINARY)
        except (RuntimeError, OSError, subprocess.SubprocessError):
            # No unverified process may be signalled. Restoration below can
            # still succeed if macOS retries after the old file is restored.
            self.rollback_host = None

    def restore_driver(self, transaction):
        del transaction
        if not self.original_snapshot:
            return
        if self.rollback_host:
            with blocked_signals():
                driver_host.signal_checked(self.rollback_host, force=True)
        deadline = time.monotonic() + 25
        last = "Legacy driver has not returned"
        while time.monotonic() < deadline:
            try:
                snapshot = self.snapshot()
                validate_after(
                    self.original_snapshot,
                    snapshot,
                    self.original_core_audio,
                    self.service_identity(),
                )
                if snapshot.get("routingDevices"):
                    raise RuntimeError("Routing devices remain after rollback")
                for field in ("clockPeriodFrames", "inputLatencyFrames"):
                    if snapshot.get(field) != self.original_snapshot.get(field):
                        raise RuntimeError("Legacy timing was not restored")
                peer = self.legacy_peer()
                if not peer.get("driverAttached") or peer.get("helperAttached"):
                    raise RuntimeError("The restored driver has not attached")
                driver_host.inspect(peer.get("driverPID", 0), BINARY)
                return
            except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
                last = str(error)
            time.sleep(0.2)
        raise RuntimeError("Legacy driver restoration was not verified: " + last)

    def activation_diagnostics(self):
        result = {
            **super().activation_diagnostics(),
            "originalDriverHost": self.original_host,
            "isolatedDriverSignalled": self.signalled,
        }
        if self.signalled:
            try:
                result["replacementDriverHost"] = driver_host.discover(BINARY)
            except (RuntimeError, OSError, subprocess.SubprocessError) as error:
                result["replacementHostInspectionError"] = str(error)
        return result
