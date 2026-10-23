"""Opt-in supervisor crash/restart/shutdown test; requires an empty routing document."""

import ctypes
import json
from pathlib import Path
import subprocess
import sys
import time

folder = Path(sys.argv[1]).resolve()
control = str(folder / "loopz-windows-control.exe")


def state():
    result = subprocess.run([control, "--state"], capture_output=True, timeout=15)
    return json.loads(result.stdout.decode("utf-8")) if result.returncode == 0 else None


before = state()
if before:
    assert not before["snapshot"]["document"]["devices"], "Requires an empty routing document"
    subprocess.run([control, "--stop-engine"], check=True, capture_output=True, timeout=15)
    time.sleep(0.5)
background = subprocess.Popen([str(folder / "LoopZ.exe"), "--background"])
try:
    for _ in range(50):
        initial = state()
        if initial:
            break
        time.sleep(0.2)
    assert initial and background.poll() is None, "Background engine failed to start"
    assert not initial["snapshot"]["document"]["devices"]
    query = (
        "Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq "
        + str(background.pid)
        + " -and $_.Name -eq 'LoopZ.exe' } | Select-Object -ExpandProperty ProcessId"
    )
    child = int(
        subprocess.check_output(
            ["powershell.exe", "-NoProfile", "-Command", query], text=True, timeout=15
        ).strip()
    )
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    process = kernel.OpenProcess(1, False, child)
    assert process, "Could not open our supervisor's own child"
    try:
        assert kernel.TerminateProcess(process, 42), "Could not simulate engine crash"
    finally:
        kernel.CloseHandle(process)
    for _ in range(50):
        recovered = state()
        if (
            recovered
            and recovered["snapshot"]["revision"]["epoch"]
            != initial["snapshot"]["revision"]["epoch"]
        ):
            break
        time.sleep(0.2)
    assert (
        recovered
        and recovered["snapshot"]["revision"]["epoch"] != initial["snapshot"]["revision"]["epoch"]
    )
    assert background.poll() is None
    subprocess.run([control, "--stop-engine"], check=True, capture_output=True, timeout=15)
    assert background.wait(timeout=10) == 0
    time.sleep(1)
    assert state() is None, "Explicit stop restarted routing"
    print(json.dumps({"ok": True, "crashRecovered": True, "explicitStopHonored": True}))
finally:
    if background.poll() is None:
        subprocess.run([control, "--stop-engine"], capture_output=True, timeout=15)
        background.wait(timeout=10)
