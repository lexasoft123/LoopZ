"""Opt-in admin/desktop: legacy private client against the installed driver.

Creates one unique root from an isolated document, outside the running engine's
desired state. Never uses --reconcile (which could prune other mixes). Playback
is restricted by the native probe to that root's virtual Feed. Cleanup removes
only that root and checks saved mixes, microphone IDs and all six defaults.
Run only after other audio acceptance processes have finished.

LOOPZ_PRIVATE_TRANSPORT_ACCEPTANCE=1 SCRIPT APPDIR FRESH_REPORT PROBE
"""

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

if sys.platform != "win32" or os.environ.get("LOOPZ_PRIVATE_TRANSPORT_ACCEPTANCE") != "1":
    raise RuntimeError("Requires explicit opt-in on a Windows workbench")
if not ctypes.windll.shell32.IsUserAnAdmin():
    raise RuntimeError("Run in an elevated interactive workbench session")
if len(sys.argv) != 4:
    raise ValueError("Use SCRIPT APPDIR FRESH_REPORT PROBE")
folder, report_path, probe = (Path(arg).resolve() for arg in sys.argv[1:])
if report_path.exists():
    raise RuntimeError("Use a fresh report")
control, helper = folder / "loopz-windows-control.exe", folder / "loopz-driver-setup.exe"
for executable in (control, helper, probe):
    if not executable.is_file():
        raise RuntimeError(f"Missing acceptance executable: {executable}")


def run(arguments, timeout=90, codes=(0,)):
    child = subprocess.run([str(a) for a in arguments], capture_output=True, timeout=timeout)
    if child.returncode not in codes:
        raise RuntimeError((child.stdout + child.stderr).decode("utf-8", errors="replace"))
    return child.stdout


def state():
    return json.loads(run([control, "--state"], timeout=30).decode("utf-8-sig"))["snapshot"]


def defaults():
    return json.loads(run([probe, "--defaults"], timeout=30).decode("utf-8-sig"))


def microphones(snapshot):
    return {d["id"]: d.get("microphone", {}).get("uid") for d in snapshot["runtime"]["devices"]}


owner = run(["whoami.exe", "/user", "/fo", "csv", "/nh"], timeout=10)
owner = owner.strip().split(b",")[-1].strip(b'"').decode("ascii")
identifier = "private-v2-acceptance-" + str(uuid.uuid4())
key = hashlib.sha256((owner + "\0" + identifier).encode()).hexdigest()
instance = "ROOT\\LOOPZAUDIO\\LZ_" + key


def adapter_exists():
    locate = ctypes.WinDLL("cfgmgr32").CM_Locate_DevNodeW
    locate.argtypes = (ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD)
    locate.restype = wintypes.DWORD
    node = wintypes.DWORD()
    result = locate(ctypes.byref(node), instance, 1)  # Also find non-present nodes.
    if result == 0x0D:
        return False
    if result:
        raise RuntimeError(f"Cannot inspect isolated adapter: CONFIGRET 0x{result:x}")
    return True


assert not adapter_exists(), "Unique fixture identity already exists"
before = state()
original_defaults, original_microphones = defaults(), microphones(before)
routing = Path(os.environ["LOCALAPPDATA"]) / "LoopZ" / "routing.json"
original_bytes = routing.read_bytes()
report = {
    "ok": False,
    "mixId": identifier,
    "instance": instance,
    "cleanup": False,
    "savedDocumentSha256": hashlib.sha256(original_bytes).hexdigest(),
    "defaultsBefore": original_defaults,
    "microphonesBefore": original_microphones,
    "binarySha256": {
        str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (control, helper, probe)
    },
}


def save():
    pending = report_path.with_suffix(".pending")
    pending.write_text(json.dumps(report, indent=2), encoding="utf-8")
    pending.replace(report_path)


provision_attempted = False
save()  # Recovery identity is durable before the first mutation.
try:
    with tempfile.TemporaryDirectory(prefix="loopz-private-v2-") as temporary:
        fixture = {
            "version": before["document"]["version"],
            "devices": [
                {
                    "id": identifier,
                    "name": "Private transport compatibility acceptance",
                    "sampleRate": 48000,
                    "formatRequestReceipt": "",
                    "bindings": [],
                    "monitors": [],
                    "routing": {
                        "enabled": True,
                        "gain": 1.0,
                        "muted": False,
                        "outputChannels": 2,
                        "safetyLimiter": False,
                        "sources": [],
                    },
                }
            ],
        }
        path = Path(temporary) / "routing.json"
        path.write_text(json.dumps(fixture), encoding="utf-8")
        provision_attempted = True
        setup = json.loads(run([helper, "--sync", path, owner]).decode("utf-8-sig"))
        assert setup["ok"] and not setup.get("rebootRequired"), setup
        assert adapter_exists(), "Helper did not create isolated adapter"
        signal = json.loads(run([probe, "--private-roundtrip", key]).decode("utf-8-sig"))
        report["signal"] = signal
        save()
        assert signal["ok"] and signal["privateRoundtrip"]
        assert signal["privateTransportVersion"] == 2 and signal["privateBitsPerSample"] == 16
        assert len(signal["phases"]) == 5  # Signal, stop, restart, disconnect silence, reconnect.
        report["ok"] = True
except BaseException as error:
    report["error"] = f"{type(error).__name__}: {error}"
    raise
finally:
    cleanup_errors = []
    try:
        if provision_attempted and adapter_exists():
            run(["pnputil.exe", "/remove-device", instance], timeout=60, codes=(0, 3010))
        deadline = time.monotonic() + 45
        while adapter_exists():
            assert time.monotonic() < deadline, "Isolated adapter remains"
            time.sleep(0.2)
    except Exception as error:
        cleanup_errors.append(str(error))
    try:
        after = state()
        report["microphonesAfter"] = microphones(after)
        report["defaultsAfter"] = defaults()
        report["savedDocumentSha256After"] = hashlib.sha256(routing.read_bytes()).hexdigest()
        assert after["document"] == before["document"], "Saved mixes changed"
        assert routing.read_bytes() == original_bytes, "Saved document bytes changed"
        assert (
            report["microphonesAfter"] == original_microphones
        ), "Existing microphone identities changed"
        assert report["defaultsAfter"] == original_defaults, "Audio default roles changed"
    except Exception as error:
        cleanup_errors.append(str(error))
    report["cleanup"] = not cleanup_errors
    report["cleanupErrors"] = cleanup_errors
    report["ok"] = report["ok"] and report["cleanup"] and not report.get("error")
    save()
    if cleanup_errors:
        raise RuntimeError("Private compatibility cleanup failed: " + "; ".join(cleanup_errors))
print(json.dumps({"ok": report["ok"], "cleanup": report["cleanup"], "report": str(report_path)}))
