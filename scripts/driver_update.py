"""Verified paired-app/existing-routing-driver update; no broker or login changes."""

import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import time

import driver_host
from desktop_install import NAMES, TARGETS, bounded_files, verify_package
from install_desktop import DesktopHost, protected_file
from install_transaction import blocked_signals, files
from prepare_routing_install import SERVICE, stable_requirement
from routing_install import TEAM

DRIVER_NAME = "LoopZAudio.driver"
DRIVER = Path("/Library/Audio/Plug-Ins/HAL") / DRIVER_NAME
BINARY = DRIVER / "Contents/MacOS/LoopZAudio"
UPDATE_NAMES = (*NAMES, DRIVER_NAME)
UPDATE_TARGETS = {**TARGETS, DRIVER_NAME: DRIVER}
DRIVER_REQUIREMENT = stable_requirement("com.lexasoft.loopz.driver", TEAM)
BROKER_PIN = {
    "protocol": 2,
    "brokerRequirement": stable_requirement("com.lexasoft.loopz.routing.broker", TEAM),
}


def verify_driver(path, verify_code):
    bounded_files(path)
    verify_code(path, "com.lexasoft.loopz.driver", DRIVER_REQUIREMENT, False)
    info = plistlib.loads((path / "Contents/Info.plist").read_bytes())
    expected = {
        "CFBundleIdentifier": "com.lexasoft.loopz.driver",
        "CFBundleExecutable": "LoopZAudio",
        "CFPlugInFactories": {"F2F8AC9D-AE84-4550-955C-E35A027F56B2": "LoopZAudio_Create"},
        "CFPlugInTypes": {
            "443ABAB8-E7B3-491A-B985-BEB9187030DB": ["F2F8AC9D-AE84-4550-955C-E35A027F56B2"]
        },
        "AudioServerPlugIn_MachServices": [SERVICE],
    }
    if any(info.get(key) != value for key, value in expected.items()):
        raise ValueError("Driver update must preserve the installed routing slot identity")
    if json.loads((path / "Contents/Resources/routing-service.json").read_text()) != BROKER_PIN:
        raise ValueError("Driver update must retain the fixed routing broker trust pin")


def verify_update(package, verify_code, expected_manifest=None):
    package = Path(package)
    actual = bounded_files(package)
    encoded = (package / "manifest.json").read_bytes()
    digest = hashlib.sha256(encoded).hexdigest()
    if len(encoded) > 1024 * 1024 or expected_manifest and digest != expected_manifest:
        raise ValueError("Update manifest changed or exceeds its size limit")
    manifest = json.loads(encoded)
    if (
        not isinstance(manifest, dict)
        or type(manifest.get("schema")) is not int
        or manifest["schema"] != 1
        or manifest.get("kind") != "loopz-desktop-driver-update"
        or manifest.get("installed") is not False
        or not isinstance(manifest.get("desktopManifestSHA256"), str)
        or not re.fullmatch(r"[a-f0-9]{64}", manifest["desktopManifestSHA256"])
    ):
        raise ValueError("Unsupported paired driver update manifest")
    actual.pop("manifest.json", None)
    if (
        not actual
        or manifest.get("files") != actual
        or any(path.split("/", 1)[0] not in ("desktop", "driver") for path in actual)
    ):
        raise ValueError("Update payload differs from its complete manifest")
    verify_package(package / "desktop", verify_code, manifest.get("desktopManifestSHA256"))
    if set(files(package / "driver")) != {
        str(Path(DRIVER_NAME) / path) for path in files(package / "driver" / DRIVER_NAME)
    }:
        raise ValueError("Unexpected files in driver payload")
    verify_driver(package / "driver" / DRIVER_NAME, verify_code)
    return manifest, digest


def assemble(package, work):
    destination = work / "assembled"
    destination.mkdir(mode=0o700)
    for name in UPDATE_NAMES:
        source = package / ("driver" if name == DRIVER_NAME else "desktop") / name
        shutil.copytree(source, destination / name, symlinks=True)
        if files(source) != files(destination / name):
            raise RuntimeError("Assembled update differs from its verified snapshot")
    return destination


class DriverUpdateHost(DesktopHost):
    def require_idle(self, snapshot):
        super().require_idle(snapshot)
        for field in ("defaultInput", "defaultOutput", "defaultSystemOutput"):
            uid = snapshot.get(field)
            if (
                not isinstance(uid, str)
                or uid.startswith("com.lexasoft.loopz.device.")
                or uid == "com.lexasoft.loopz.audio.feasibility.v1"
            ):
                raise RuntimeError("LoopZ must not be a default device during driver replacement")
        for field in ("routingDevices", "routingBlockingRoutes", "loopzBlockingRoutes"):
            if field not in snapshot or snapshot[field]:
                raise RuntimeError(
                    "Driver update requires a complete, empty and idle routing catalog"
                )
        if snapshot.get("loopzVisible") is not False or snapshot.get("loopzRunning") is not False:
            raise RuntimeError(
                "Use initial migration for the old stereo driver, not routing update"
            )
        if (DRIVER.parent / "LoopZRoutingAudio.driver").exists() or snapshot.get(
            "routingPlugIn", {}
        ).get("registered"):
            raise RuntimeError("A separate routing-driver slot must not coexist with this update")

    def protected_state(self):
        state = super().protected_state()
        state.pop(str(DRIVER))
        return state

    def validate_installed_driver(self, verify_code):
        protected_file(DRIVER, directory=True)
        for path in DRIVER.rglob("*"):
            info = path.lstat()
            if (
                not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode))
                or info.st_uid != 0
                or info.st_mode & 0o022
            ):
                raise RuntimeError("Installed driver files must be protected and root-owned")
        verify_driver(DRIVER, verify_code)

    def validate_apps(self):
        result = super().validate_apps()
        if (
            type(result["engine"].get("publicTimingProbe")) is not int
            or result["engine"]["publicTimingProbe"] != 1
        ):
            raise RuntimeError("Updated Engine lacks the capture-free driver timing check")
        return result


class DriverTransition:
    def __init__(self, host):
        self.host = host
        self.original = self.replacement = None
        self.signalled = False
        self.rollback_bundles = {DRIVER_NAME}

    def begin(self, before, core_audio):
        self.before, self.core_audio = before, core_audio
        self.original = driver_host.discover(BINARY)

    def activate(self, transaction, guard):
        if self.original is None or DRIVER_NAME not in transaction.backups:
            raise RuntimeError("Driver replacement requires its verified original and backup")
        backup = transaction.backups[DRIVER_NAME]
        audited = driver_host.inspect(
            self.original["pid"], backup / "Contents/MacOS/LoopZAudio", {DRIVER.name, backup.name}
        )
        if audited != self.original:
            # The backup bundle name is allowed to appear after its atomic rename.
            if {k: v for k, v in audited.items() if k != "HALBundles"} != {
                k: v for k, v in self.original.items() if k != "HALBundles"
            }:
                raise RuntimeError("Original isolated driver generation changed")
        self.host.require_idle(self.host.snapshot())
        if self.host.service_identity() != self.core_audio:
            raise RuntimeError("Core Audio changed before the isolated driver update")
        guard.check()
        with blocked_signals():
            driver_host.signal_checked(self.original, force=True)
            self.signalled = True

    def validate(self, activation):
        replacement = driver_host.discover(BINARY)
        if replacement["pid"] == self.original["pid"]:
            raise RuntimeError("The replacement routing driver has not loaded")
        report = json.loads(
            self.host.user_command(
                TARGETS["LoopZ Engine.app"] / "Contents/MacOS/LoopZ Engine",
                "--routing-timing-check",
                "--device-uid",
                activation["publicDevice"]["uid"],
                "--seconds",
                "3",
            ).stdout
        )
        if (
            report.get("passed") is not True
            or report.get("inputOpened") is not False
            or report.get("physicalDevicesOpened") is not False
            or report.get("permissionRequested") is not False
        ):
            raise RuntimeError("Driver callback timing did not pass its capture-free check")
        if driver_host.discover(BINARY) != replacement:
            raise RuntimeError("Replacement driver changed during its timing check")
        return {"originalHost": self.original, "replacementHost": replacement, "timing": report}

    def prepare_rollback(self, transaction=None):
        # macOS may replace the host after file promotion even if our explicit
        # reload never ran. Inspect actual mapped state before disk rollback.
        if transaction and DRIVER_NAME in transaction.backups:
            self.rollback_bundles.add(transaction.backups[DRIVER_NAME].name)
        try:
            self.replacement = driver_host.discover(
                BINARY, allowed_bundles=self.rollback_bundles, allow_other_images=True
            )
        except (RuntimeError, OSError, subprocess.SubprocessError):
            self.replacement = None

    def restore(self, work):
        if self.host.service_identity() != self.core_audio:
            raise RuntimeError("Core Audio changed before driver runtime rollback")
        restored = None
        try:
            # The caller verified restored file hashes. Matching that inode
            # now proves the old image is resident, including automatic reload.
            restored = driver_host.discover(BINARY, allowed_bundles=self.rollback_bundles)
        except (RuntimeError, OSError, subprocess.SubprocessError):
            pass
        known_new_image = self.replacement and (
            self.replacement["device"],
            self.replacement["inode"],
        ) != (self.original["device"], self.original["inode"])

        def same_generation(left, right):
            return left is not None and all(
                left[key] == right[key]
                for key in ("pid", "uid", "startSeconds", "startMicroseconds", "executable")
            )

        # A process known to have loaded the new image cannot prove old-code
        # restoration merely by retaining another mapping of the old image.
        if known_new_image and same_generation(restored, self.replacement):
            restored = None
        reload_required = restored is None
        signal_error = None
        if reload_required and self.replacement:
            try:
                with blocked_signals():
                    driver_host.signal_checked(self.replacement, force=True)
            except (RuntimeError, OSError) as error:
                # The OS may already have replaced this generation. Do not
                # guess a PID; wait for a strictly verified old-image host.
                signal_error = str(error)
        deadline = time.monotonic() + 25
        while restored is None:
            try:
                self.host.snapshot()  # Let the HAL observe the restored bundle.
                restored = driver_host.discover(BINARY, allowed_bundles=self.rollback_bundles)
                if known_new_image and same_generation(restored, self.replacement):
                    restored = None
                    raise RuntimeError("The host retaining the new image has not exited")
            except (RuntimeError, OSError, subprocess.SubprocessError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Restored driver runtime was not verified") from None
                time.sleep(0.2)
        validation = work / "rollback-validation"
        validation.mkdir(mode=0o711)
        try:
            activation = self.host.validate_activation(validation)
        finally:
            self.host.cleanup_validation()
        return {
            "reloadRequired": reload_required,
            "host": restored,
            "activation": activation,
            "supersededSignalError": signal_error,
        }
