"""Real WebKitGTK acceptance with an isolated, silent native Engine fixture."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) != 6:
        raise SystemExit("Usage: linux_desktop_smoke.py ENGINE CLI DESKTOP INDEX_HTML ARTIFACTS")
    engine, cli, desktop, index, artifacts = [Path(arg).resolve() for arg in sys.argv[1:]]
    xvfb = shutil.which("xvfb-run")
    if not xvfb:
        raise RuntimeError("The real Linux desktop check requires xvfb-run")
    if not index.is_file():
        raise RuntimeError("Build the interface bundle before the Linux desktop check")
    artifacts.mkdir(parents=True, exist_ok=True)
    # Freeze each run so a later build cannot silently replace the measured
    # executables or assets, and reruns preserve earlier evidence.
    artifacts = Path(tempfile.mkdtemp(prefix="run-", dir=artifacts))
    binary_directory = artifacts / "bin"
    binary_directory.mkdir()
    frozen = []
    for binary in (engine, cli, desktop):
        destination = binary_directory / binary.name
        shutil.copy2(binary, destination)
        frozen.append(destination)
    engine, cli, desktop = frozen
    shutil.copytree(index.parent, artifacts / "interface")
    index = artifacts / "interface" / index.name
    with tempfile.TemporaryDirectory(prefix="loopz-ui-") as temporary:
        root = Path(temporary)
        runtime = root / "run"
        runtime.mkdir(mode=0o700)
        address = runtime / "loopz" / "control.sock"
        state = root / "state" / "engine.json"
        environment = dict(
            os.environ,
            XDG_RUNTIME_DIR=str(runtime),
            XDG_CONFIG_HOME=str(root / "config"),
            XDG_CACHE_HOME=str(root / "cache"),
            XDG_DATA_HOME=str(root / "data"),
            HOME=str(root),
            LANG="C.UTF-8",
        )
        process = subprocess.Popen(
            [str(engine), "--socket", str(address), "--state", str(state)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 5
            while not address.exists():
                if process.poll() is not None:
                    raise AssertionError(process.communicate()[1].decode())
                if time.monotonic() >= deadline:
                    raise AssertionError("Fixture Engine did not expose its private socket")
                time.sleep(0.02)

            def request(value):
                result = subprocess.run(
                    [str(cli), "--socket", str(address), "--request", json.dumps(value)],
                    capture_output=True,
                    text=True,
                    timeout=5,
                    env=environment,
                )
                assert result.returncode == 0, result.stderr + result.stdout
                return json.loads(result.stdout)

            initial = request({"version": 1, "operation": "state"})
            assert initial["snapshot"]["canCreateDocument"] is True
            result = request(
                {
                    "version": 1,
                    "operation": "initialize",
                    "request": {
                        "version": 1,
                        "expected": initial["snapshot"]["revision"],
                        "commands": [
                            {
                                "type": "createDevice",
                                "id": "linux-smoke-mix",
                                "name": "Linux smoke mix",
                                "channels": 2,
                                "sampleRate": 48000,
                                "passThrough": False,
                            }
                        ],
                    },
                }
            )
            assert result["ok"]
            expected = request({"version": 1, "operation": "state"})
            (artifacts / "fixture-state.json").write_bytes(state.read_bytes())
            # No source, monitor or physical device is opened by this fixture.
            document = expected["snapshot"]["document"]
            assert document["devices"][0]["routing"]["sources"] == []
            assert document["devices"][0]["monitors"] == []
            host = subprocess.Popen(
                [
                    xvfb,
                    "-a",
                    "-s",
                    "-screen 0 1280x800x24",
                    str(desktop),
                    "--interface",
                    str(index),
                    "--smoke",
                    str(artifacts),
                ],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
            try:
                stdout, stderr = host.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(host.pid, signal.SIGKILL)
                host.communicate()
                raise AssertionError("Linux desktop exceeded its outer acceptance deadline")
            (artifacts / "host.log").write_bytes(stdout + stderr)
            assert host.returncode == 0, (stdout + stderr).decode(errors="replace")
            report = json.loads((artifacts / "report.json").read_text())
            assert report["ok"] and report["valid"], report
            assert report["state"]["snapshot"]["revision"] == expected["snapshot"]["revision"]
            assert report["state"]["snapshot"]["document"] == document
            assert "Linux smoke mix" in report["visibleText"]
            assert report["heading"]["width"] > 0 and report["heading"]["height"] > 0
            assert report["outputSockets"] >= 2
            assert (artifacts / "interface.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
            receipt = {
                "verification": "real WebKitGTK + isolated Engine; silent fixture",
                "files": {
                    str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in (engine, cli, desktop, *(artifacts / "interface").rglob("*"))
                    if path.is_file()
                },
                "revision": expected["snapshot"]["revision"],
            }
            (artifacts / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
            print(
                f"Real Linux WebKitGTK bridge, rendered routing and screenshot checks passed: {artifacts}"
            )
        finally:
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    main()
