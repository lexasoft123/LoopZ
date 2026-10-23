"""Opt-in Dell/admin test of owner-scoped microphone repair and cleanup.

Creates only uniquely named temporary mixes/roots. A synthetic foreign SID
checks that repair preserves another owner's adapter. Existing mixes, endpoint
identities and all six system defaults must survive. No boot settings change.
Usage: SCRIPT APPDIR FRESH_REPORT
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

folder = Path(sys.argv[1]).resolve()
report_path = Path(sys.argv[2]).resolve()
if report_path.exists():
    raise RuntimeError("Use a fresh report path")
control = folder / "loopz-windows-control.exe"
setup = folder / "loopz-driver-setup.exe"
probe = folder / "loopz-windows-cable-probe.exe"
routing = Path(os.environ["LOCALAPPDATA"]) / "LoopZ" / "routing.json"
owner = subprocess.check_output(["whoami.exe", "/user", "/fo", "csv", "/nh"],
                                text=True, timeout=10).strip().split(",")[-1].strip('"')
foreign_owner = "S-1-5-21-0-0-0-4294967294"
assert owner != foreign_owner
identifier = "reconcile-acceptance-" + str(uuid.uuid4())
foreign_id = identifier + "-foreign"


def instance(sid, identity):
    key = hashlib.sha256((sid + "\0" + identity).encode("utf-8")).hexdigest()
    return "ROOT\\LOOPZAUDIO\\LZ_" + key


ours = instance(owner, identifier)
foreign = instance(foreign_owner, foreign_id)
report = {"ok": False, "mixId": identifier, "ownRoot": ours, "foreignRoot": foreign,
          "checks": [], "cleanup": False}


def save():
    temporary = report_path.with_suffix(".pending")
    temporary.write_text(json.dumps(report, indent=2), encoding="utf-8")
    temporary.replace(report_path)


def run_json(command):
    result = subprocess.run([str(v) for v in command], capture_output=True, timeout=120)
    assert result.returncode == 0, (result.stdout + result.stderr).decode("utf-8", errors="replace")
    return json.loads(result.stdout.decode("utf-8-sig"))


def state():
    return run_json([control, "--state"])


def exists(root):
    locate = ctypes.WinDLL("cfgmgr32").CM_Locate_DevNodeW
    locate.argtypes = (ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD)
    locate.restype = wintypes.DWORD
    node = wintypes.DWORD()
    code = locate(ctypes.byref(node), root, 1) # include phantom nodes
    if code == 0x0D:
        return False
    assert code == 0, f"Cannot inspect exact test root: CONFIGRET 0x{code:x}"
    return True


def wait_removed(root):
    deadline = time.monotonic() + 15
    while exists(root):
        assert time.monotonic() < deadline, "Test adapter remains installed"
        time.sleep(0.2)


before = state()
original = before["snapshot"]["document"]
original_mics = {d["id"]: d.get("microphone", {}).get("uid")
                 for d in before["snapshot"]["runtime"]["devices"]}
original_defaults = run_json([probe, "--defaults"])
save()
with tempfile.TemporaryDirectory(prefix="loopz-reconcile-") as temporary:
    request_path = Path(temporary) / "request.json"

    def edit(commands):
        current = state()
        request_path.write_text(json.dumps({"version": 1, "operation": "edit", "request": {
            "version": 1, "expected": current["snapshot"]["revision"], "commands": commands,
        }}), encoding="utf-8")
        reply = run_json([control, "--request", request_path])
        assert reply["ok"], reply

    try:
        edit([{"type": "createDevice", "id": identifier, "name": "Reconcile acceptance",
               "passThrough": False},
              {"type": "createDevice", "id": foreign_id, "name": "Foreign owner acceptance",
               "passThrough": False}])
        foreign_document = state()["snapshot"]["document"]
        foreign_document["devices"] = [d for d in foreign_document["devices"] if d["id"] == foreign_id]
        edit([{"type": "deleteDevice", "device": foreign_id}])
        assert run_json([setup, "--sync", routing, owner])["ok"]
        assert exists(ours)
        foreign_path = Path(temporary) / "foreign-routing.json"
        foreign_path.write_text(json.dumps(foreign_document), encoding="utf-8")
        assert run_json([setup, "--sync", foreign_path, foreign_owner])["ok"]
        assert exists(foreign)
        edit([{"type": "deleteDevice", "device": identifier}])
        assert exists(ours), "Expected the provision-only command to retain the deleted root"

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateMutexW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
        kernel.CreateMutexW.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.ReleaseMutex.argtypes = (wintypes.HANDLE,)
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        mutex = kernel.CreateMutexW(None, False, "Global\\LoopZDriverSetup")
        assert mutex, "Cannot hold setup mutex for the concurrency test"
        acquired = False
        try:
            acquired = kernel.WaitForSingleObject(mutex, 0) in (0, 0x80)
            assert acquired, "Another real setup is running"
            blocked = subprocess.run([str(setup), "--reconcile", str(routing), owner],
                                     capture_output=True, timeout=30)
            assert blocked.returncode != 0
            assert "Another microphone setup is running" in blocked.stderr.decode("utf-8")
            assert exists(ours) and exists(foreign)
        finally:
            if acquired:
                kernel.ReleaseMutex(mutex)
            kernel.CloseHandle(mutex)
        report["checks"].append("concurrent setup rejected without removal")
        repaired = run_json([setup, "--reconcile", routing, owner])
        report["repairResult"] = repaired
        assert repaired["ok"] and repaired["removed"] >= 1, repaired
        wait_removed(ours)
        assert exists(foreign), "Reconciliation removed another owner's microphone"
        report["checks"].extend(["deleted mix root removed", "foreign owner preserved"])
        repeated = run_json([setup, "--reconcile", routing, owner])
        assert repeated["ok"] and repeated["removed"] == 0, repeated
        report["checks"].append("repair is idempotent")
        foreign_document["devices"] = []
        foreign_path.write_text(json.dumps(foreign_document), encoding="utf-8")
        empty = run_json([setup, "--reconcile", foreign_path, foreign_owner])
        assert empty["ok"] and empty["microphones"] == 0 and empty["removed"] == 1, empty
        wait_removed(foreign)
        report["checks"].append("empty document removes only its owner's last root")
        save()
    except BaseException as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        errors = []
        try:
            present = {d["id"] for d in state()["snapshot"]["document"]["devices"]}
            commands = [{"type": "deleteDevice", "device": d}
                        for d in (identifier, foreign_id) if d in present]
            if commands:
                edit(commands)
        except Exception as error:
            errors.append(str(error))
        for root in (ours, foreign):
            try:
                if exists(root):
                    subprocess.run(["pnputil.exe", "/remove-device", root], check=True,
                                   capture_output=True, timeout=60)
                wait_removed(root)
            except Exception as error:
                errors.append(str(error))
        try:
            final = state()
            assert final["snapshot"]["document"] == original, "Saved mixes changed"
            assert run_json([probe, "--defaults"]) == original_defaults, "System defaults changed"
            mics = {d["id"]: d.get("microphone", {}).get("uid")
                    for d in final["snapshot"]["runtime"]["devices"]}
            assert mics == original_mics, "Existing microphone identities changed"
            report["checks"].append("saved mixes, endpoint IDs and defaults preserved")
        except Exception as error:
            errors.append(str(error))
        report["cleanupErrors"] = errors
        report["cleanup"] = not errors
        report["ok"] = not errors and not report.get("error") and len(report["checks"]) == 6
        save()
        if errors:
            raise RuntimeError("Reconciliation cleanup failed: " + "; ".join(errors))
assert report["ok"], report
print(json.dumps(report))
