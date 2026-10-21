"""The desktop rejects altered UI resources before enabling native controls."""

from pathlib import Path
import plistlib
import json
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="loopz-desktop-seal-") as temporary:
    app = Path(temporary) / "LoopZ.app"
    shutil.copytree(source, app)
    executable = app / "Contents/MacOS/LoopZ"
    helper = app / "Contents/MacOS/LoopZLogin"
    definition = plistlib.loads(
        (app / "Contents/Library/LaunchAgents/com.lexasoft.loopz.login.plist").read_bytes()
    )
    assert definition["BundleProgram"] == "Contents/MacOS/LoopZLogin"
    assert definition["RunAtLoad"] is True and definition["KeepAlive"] is False
    assert (
        subprocess.run([str(helper), "--invalid"], capture_output=True, timeout=10).returncode == 2
    )
    assert subprocess.run([str(executable), "--verify-interface"], timeout=10).returncode == 0
    status = subprocess.run(
        [str(executable), "--background-status"], capture_output=True, text=True, timeout=10
    )
    assert status.returncode == 0
    assert json.loads(status.stdout)["login"]["registered"] is False
    assert json.loads(status.stdout)["login"]["available"] is False
    assert (
        subprocess.run(
            [str(executable), "--fixture-endpoint", "untrusted"], capture_output=True, timeout=10
        ).returncode
        == 2
    )
    script = app / "Contents/Resources/interface/loopz.js"
    script.write_bytes(script.read_bytes() + b"\n/* altered */\n")
    assert subprocess.run([str(executable), "--verify-interface"], timeout=10).returncode == 3
