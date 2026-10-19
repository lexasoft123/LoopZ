#!/usr/bin/env python3
"""Remove this prototype's fixed installation paths without restarting audio."""
import os
from pathlib import Path
import shutil
import subprocess

if os.geteuid() != 0:
    raise SystemExit("Administrator privileges are required")
subprocess.run(["launchctl", "bootout", "system/com.lexasoft.loopz.bridge"], check=False)
for path in [
    "/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver",
    "/Applications/LoopZ Engine.app",
    "/Library/Application Support/LoopZ/loopz-broker",
    "/Library/Application Support/LoopZ/loopz-probe",
    "/Library/Application Support/LoopZ/loopz-install-check",
    "/Library/Application Support/LoopZ/install-result.json",
    "/Library/LaunchDaemons/com.lexasoft.loopz.bridge.plist",
]:
    target = Path(path)
    if target.is_symlink() or target.is_file():
        target.unlink()
    elif target.is_dir():
        shutil.rmtree(target)
print("Removed LoopZ prototype files. A loaded driver remains in Core Audio until its next reload.")
