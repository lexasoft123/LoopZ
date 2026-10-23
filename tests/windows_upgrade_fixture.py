"""Opt-in admin fixture for a separately authorized multi-mix driver upgrade.

prepare journals the saved document/defaults before adding one isolated mix.
cleanup removes only that mix/root and verifies restoration. Keep the journal
across the upgrade; use windows_upgrade_acceptance.py for identity assertions.
Requires an already running engine owned by the invoking Windows user.
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

mode, folder_arg, journal_arg = sys.argv[1:]
if mode not in ("prepare", "cleanup"):
    raise ValueError("Use prepare or cleanup APPDIR JOURNAL")
folder, journal = Path(folder_arg).resolve(), Path(journal_arg).resolve()
control = folder / "loopz-windows-control.exe"
probe = folder / "loopz-windows-cable-probe.exe"


def run(arguments):
    result = subprocess.run([str(a) for a in arguments], capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).decode("utf-8", errors="replace"))
    return result.stdout


def state():
    return json.loads(run([control, "--state"]))["snapshot"]


def owner_sid():
    return run(["whoami.exe", "/user", "/fo", "csv", "/nh"]).strip().split(b",")[-1].strip(b'"').decode("ascii")


def defaults():
    return json.loads(run([probe, "--defaults"]))


def identities(snapshot):
    return {d["id"]: d.get("microphone", {}).get("uid") for d in snapshot["runtime"]["devices"]}


def write():
    pending = journal.with_suffix(".pending")
    pending.write_text(json.dumps(record, indent=2), encoding="utf-8")
    pending.replace(journal)


def edit(commands):
    current = state()
    with tempfile.TemporaryDirectory(prefix="loopz-upgrade-fixture-") as temporary:
        request = Path(temporary) / "request.json"
        request.write_text(json.dumps({"version": 1, "operation": "edit", "request": {
            "version": 1, "expected": current["revision"], "commands": commands}}), encoding="utf-8")
        reply = json.loads(run([control, "--request", request]))
        assert reply["ok"], reply


def adapter_exists(key):
    locate = ctypes.WinDLL("cfgmgr32").CM_Locate_DevNodeW
    locate.argtypes = (ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD)
    locate.restype = wintypes.DWORD
    node = wintypes.DWORD()
    status = locate(ctypes.byref(node), "ROOT\\LOOPZAUDIO\\LZ_" + key, 1)
    if status == 0x0D:
        return False
    if status:
        raise RuntimeError(f"Cannot inspect fixture adapter: CONFIGRET 0x{status:x}")
    return True


if mode == "prepare":
    assert not journal.exists(), "Use a new fixture journal"
    original = state()
    assert original["document"]["devices"], "Requires an existing saved mix"
    owner = owner_sid()
    identifier = "upgrade-identity-" + str(uuid.uuid4())
    record = {"version": 1, "mixId": identifier,
              "mixKey": hashlib.sha256((owner + "\0" + identifier).encode()).hexdigest(),
              "originalDocument": original["document"], "originalDefaults": defaults(),
              "originalMicrophones": identities(original), "prepared": False, "cleanup": False}
    assert record["originalMicrophones"] and all(record["originalMicrophones"].values())
    write()  # Durable recovery data precedes every mutation.
    edit([{"type": "createDevice", "id": identifier, "name": "Upgrade identity acceptance",
           "passThrough": True, "channels": 2, "sampleRate": 48000}])
    run([folder / "loopz-driver-setup.exe", "--sync",
         Path(os.environ["LOCALAPPDATA"]) / "LoopZ" / "routing.json", owner])
    deadline = time.monotonic() + 45
    while True:
        current = state()
        active = next(d for d in current["runtime"]["devices"] if d["id"] == identifier)
        if active.get("microphone", {}).get("connected"):
            break
        assert time.monotonic() < deadline, active
        time.sleep(.25)
    assert defaults() == record["originalDefaults"]
    assert all(identities(current).get(i) == uid for i, uid in record["originalMicrophones"].items())
    record["prepared"] = True
    record["fixtureMicrophones"] = identities(current)
    write()
else:
    record = json.loads(journal.read_text(encoding="utf-8"))
    assert record["version"] == 1 and not record["cleanup"]
    key = record["mixKey"]
    assert len(key) == 64 and all(c in "0123456789abcdef" for c in key)
    assert record["mixId"].startswith("upgrade-identity-")
    uuid.UUID(record["mixId"].removeprefix("upgrade-identity-"))
    assert key == hashlib.sha256((owner_sid() + "\0" + record["mixId"]).encode()).hexdigest()
    assert all(d["id"] != record["mixId"] for d in record["originalDocument"]["devices"])
    current = state()
    before_cleanup = identities(current)
    if any(d["id"] == record["mixId"] for d in current["document"]["devices"]):
        edit([{"type": "deleteDevice", "device": record["mixId"]}])
    if adapter_exists(key):
        run(["pnputil.exe", "/remove-device", "ROOT\\LOOPZAUDIO\\LZ_" + key])
    deadline = time.monotonic() + 10
    while adapter_exists(key):
        assert time.monotonic() < deadline, "Fixture adapter remains installed"
        time.sleep(.2)
    final = state()
    assert final["document"] == record["originalDocument"], "Saved document did not restore"
    assert defaults() == record["originalDefaults"], "Audio defaults changed"
    assert all(identities(final).get(i) and identities(final).get(i) == before_cleanup.get(i)
               for i in record["originalMicrophones"]), "Cleanup changed a saved microphone"
    record["cleanup"] = True
    record["finalMicrophones"] = identities(final)
    write()
print(json.dumps({"ok": True, "mode": mode, "mixId": record["mixId"],
                  "prepared": record["prepared"], "cleanup": record["cleanup"]}))
