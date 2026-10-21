"""Remove only a verified isolated LoopZ host after quarantining its bundle."""

import os
from pathlib import Path
import time

import driver_host

HAL = Path("/Library/Audio/Plug-Ins/HAL")


def inspect(pid, binary, allowed):
    before = driver_host.identity(pid)
    result = driver_host.command("/usr/sbin/lsof", "-nP", "-a", "-p", pid, "-F0pfDin")
    if result.stderr.strip() or len(result.stdout) > 8 * 1024 * 1024:
        raise RuntimeError("Driver mapping inspection emitted warnings")
    records, row, process = [], None, False
    for field in result.stdout.split(b"\0"):
        field = field.lstrip(b"\n")
        if not field:
            continue
        key, value = chr(field[0]), field[1:].decode("utf-8", errors="strict")
        if key == "p":
            if int(value) != pid:
                raise RuntimeError("Driver mapping inventory contains another process")
            process = True
        elif key == "f":
            row = {"f": value}
            records.append(row)
        elif row is not None:
            row[key] = value
    expected = binary.stat()
    found, bundles = False, set()
    for row in records:
        name = row.get("n", "").removesuffix(" (deleted)")
        if "/Audio/Plug-Ins/HAL/" in name and not name.startswith(str(HAL) + "/"):
            raise RuntimeError("LoopZ host also maps another HAL root")
        if not name.startswith(str(HAL) + "/"):
            continue
        bundle = name[len(str(HAL)) + 1 :].split("/", 1)[0]
        if bundle not in allowed:
            raise RuntimeError("LoopZ host also maps another HAL component")
        bundles.add(bundle)
        if name.endswith("/Contents/MacOS/" + binary.name) and row.get("f") in (
            "txt",
            "mem",
            "DEL",
        ):
            if "D" not in row or "i" not in row:
                raise RuntimeError("Mapped driver lacks its kernel file identity")
            if (int(row["D"], 16), int(row["i"])) != (expected.st_dev, expected.st_ino):
                raise RuntimeError("Another LoopZ image is mapped in the driver host")
            found = True
    if not process or not found or driver_host.identity(pid) != before:
        raise RuntimeError("Driver host identity or mapped image changed")
    return {
        **before,
        "device": expected.st_dev,
        "inode": expected.st_ino,
        "HALBundles": sorted(bundles),
    }


class DriverRemoval:
    def __init__(self, removal):
        self.configure(removal)
        self.original = self.discover()

    def configure(self, removal):
        names = [name for name in ("driver", "routingDriver") if removal.originals.get(name)]
        if len(names) != 1:
            raise RuntimeError("Removal requires exactly one installed routing-driver slot")
        self.name = names[0]
        self.bundle = removal.targets[self.name]
        self.executable = "LoopZAudio" if self.name == "driver" else "LoopZRoutingAudio"
        self.binary = self.bundle / "Contents/MacOS" / self.executable
        self.original = None
        self.signalled = False

    @classmethod
    def for_restoration(cls, removal):
        result = cls.__new__(cls)
        result.configure(removal)
        return result

    def pids(self):
        output = driver_host.command("/bin/ps", "-axo", "pid=,args=").stdout.decode()
        result = []
        for line in output.splitlines():
            parts = line.strip().split(maxsplit=1)
            if len(parts) == 2 and parts[1] == f"Core Audio Driver ({self.bundle.name})":
                result.append(int(parts[0]))
        return result

    def discover(self):
        pids = self.pids()
        if len(pids) != 1:
            raise RuntimeError("Cannot identify one isolated LoopZ driver host")
        return inspect(pids[0], self.binary, {self.bundle.name})

    def deactivate(self, removal):
        if self.original is None:
            raise RuntimeError("A restoration-only observer cannot signal a driver host")
        pid = self.original["pid"]
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass  # macOS already unloaded the quarantined plug-in.
        else:
            saved = removal.quarantined(self.name)
            audited = inspect(
                pid,
                saved / "Contents/MacOS" / self.executable,
                {self.bundle.name, saved.parent.name},
            )
            if {k: v for k, v in audited.items() if k != "HALBundles"} != {
                k: v for k, v in self.original.items() if k != "HALBundles"
            }:
                raise RuntimeError("The isolated driver host changed before removal")
            driver_host.signal_checked(audited)
            self.signalled = True
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if not self.pids():
                return
            time.sleep(0.1)
        raise RuntimeError("The isolated LoopZ host did not unload; no global restart attempted")

    def restore(self):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                self.discover()
                return
            except RuntimeError:
                time.sleep(0.1)
        raise RuntimeError(
            "The restored driver host did not return; retained installation needs repair"
        )
