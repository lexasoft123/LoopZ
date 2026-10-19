"""Fail-closed inspection of an isolated, Apple-signed LoopZ HAL host."""

import ctypes
import os
from pathlib import Path
import pwd
import subprocess

HAL = Path("/Library/Audio/Plug-Ins/HAL")
DRIVER = HAL / "LoopZAudio.driver"
IDENTITY = 'anchor apple and identifier "com.apple.audio.Core-Audio-Driver-Service.helper"'


def command(*args):
    return subprocess.run(list(map(str, args)), capture_output=True, check=True, timeout=10)


class BSDInfo(ctypes.Structure):
    _fields_ = (
        [
            (key, ctypes.c_uint32)
            for key in [
                "flags",
                "status",
                "xstatus",
                "pid",
                "ppid",
                "uid",
                "gid",
                "ruid",
                "rgid",
                "svuid",
                "svgid",
                "reserved",
            ]
        ]
        + [("comm", ctypes.c_char * 16), ("name", ctypes.c_char * 32)]
        + [(key, ctypes.c_uint32) for key in ["nfiles", "pgid", "pjobc", "tdev", "tpgid"]]
        + [
            ("nice", ctypes.c_int32),
            ("startSeconds", ctypes.c_uint64),
            ("startMicroseconds", ctypes.c_uint64),
        ]
    )


def process_identity(pid):
    if not isinstance(pid, int) or pid <= 1:
        raise RuntimeError("Invalid isolated host PID")
    lib = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    lib.proc_pidinfo.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    lib.proc_pidinfo.restype = ctypes.c_int
    info = BSDInfo()
    if (
        lib.proc_pidinfo(pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info)) != ctypes.sizeof(info)
        or info.pid != pid
        or info.uid != pwd.getpwnam("_coreaudiod").pw_uid
        or not info.startSeconds
    ):
        raise RuntimeError("Kernel host generation or UID is unavailable")
    lib.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    lib.proc_pidpath.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    if lib.proc_pidpath(pid, buffer, len(buffer)) <= 0:
        raise RuntimeError("Cannot inspect isolated host executable")
    path = buffer.value.decode()
    if (
        not path.startswith("/System/Library/")
        or Path(path).name != "com.apple.audio.Core-Audio-Driver-Service.helper"
    ):
        raise RuntimeError("Refusing to signal a non-isolated audio service")
    return {
        "pid": pid,
        "uid": info.uid,
        "startSeconds": info.startSeconds,
        "startMicroseconds": info.startMicroseconds,
        "executable": path,
    }


def identity(pid):
    before = process_identity(pid)
    command("/usr/bin/codesign", "--verify", "--strict", "-R", "=" + IDENTITY, before["executable"])
    if process_identity(pid) != before:
        raise RuntimeError("Host generation changed during signature verification")
    return before


def signal_checked(host, force=False):
    # Public PID signalling cannot be atomic with process-generation inspection.
    # Minimize that residual race: no subprocess/HAL call separates this precise
    # kernel identity check from the single-PID signal.
    import signal

    current = process_identity(host["pid"])
    if any(host.get(key) != value for key, value in current.items()):
        raise RuntimeError("Host generation changed before signal")
    os.kill(host["pid"], signal.SIGKILL if force else signal.SIGTERM)


def mapped_image(output, pid, binary, allowed_bundles):
    """Parse all lsof file records, including memory-mapped executable images."""
    records = []
    record = None
    found_pid = False
    for raw in output.split(b"\0"):
        raw = raw.lstrip(b"\n")
        if not raw:
            continue
        key, value = chr(raw[0]), raw[1:].decode("utf-8", errors="strict")
        if key == "p":
            if int(value) != pid:
                raise RuntimeError("Mapped inventory contains another process")
            found_pid = True
        elif key == "f":
            record = {"f": value}
            records.append(record)
        elif record is not None:
            record[key] = value
    if not found_pid or not records:
        raise RuntimeError("Mapped image enumeration is incomplete")
    info = Path(binary).stat()
    expected = (info.st_dev, info.st_ino)
    found = False
    bundles = set()
    for record in records:
        name = record.get("n", "")
        roots = [str(HAL), "/System/Library/Audio/Plug-Ins/HAL"]
        root = next((root for root in roots if name.startswith(root + "/")), None)
        if root is None:
            continue
        component = name[len(root) + 1 :].split("/", 1)[0]
        bundles.add(component)
        if root != str(HAL):
            raise RuntimeError("Isolated host also references another HAL root: " + name)
        if component not in allowed_bundles:
            raise RuntimeError("Isolated host also maps another HAL component: " + component)
        if name.endswith("/Contents/MacOS/LoopZAudio") and record.get("f") in ("txt", "mem"):
            if "D" not in record or "i" not in record:
                raise RuntimeError("Mapped driver identity lacks device/inode")
            if (int(record["D"], 16), int(record["i"])) == expected:
                found = True
    if not found:
        raise RuntimeError("Expected driver executable is not mapped in this host")
    return {"device": info.st_dev, "inode": info.st_ino, "HALBundles": sorted(bundles)}


def inspect(pid, binary, allowed_bundles=None):
    before = identity(pid)
    inventory = command("/usr/sbin/lsof", "-nP", "-a", "-p", pid, "-F0pfDin")
    if inventory.stderr.strip():
        raise RuntimeError("Mapped image inventory emitted warnings")
    image = mapped_image(inventory.stdout, pid, binary, allowed_bundles or {DRIVER.name})
    if identity(pid) != before:
        raise RuntimeError("Host identity changed during inspection")
    return {**before, **image}


def discover(binary):
    rows = command("/bin/ps", "-axo", "pid=,args=").stdout.decode().splitlines()
    candidates = []
    for row in rows:
        fields = row.strip().split(maxsplit=1)
        if len(fields) == 2 and fields[1] == "Core Audio Driver (LoopZAudio.driver)":
            candidates.append(int(fields[0]))
    if len(candidates) != 1:
        raise RuntimeError("Cannot identify exactly one isolated LoopZ host")
    return inspect(candidates[0], binary)


def require_idle(snapshot, uid="com.lexasoft.loopz.audio.feasibility.v1"):
    if (
        not snapshot.get("loopzVisible")
        or not snapshot.get("loopzAlive")
        or snapshot.get("loopzRunning")
    ):
        raise RuntimeError("LoopZ must be visible, alive and idle for this experiment")
    if any(
        snapshot.get(key) == uid for key in ["defaultInput", "defaultOutput", "defaultSystemOutput"]
    ):
        raise RuntimeError("LoopZ is a default device; targeted reload refused")
    if "loopzBlockingRoutes" not in snapshot or snapshot["loopzBlockingRoutes"]:
        raise RuntimeError(
            "An active/default aggregate depends on LoopZ, or route inspection is incomplete"
        )
