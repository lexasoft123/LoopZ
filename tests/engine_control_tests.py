import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

engine, cli = map(str, map(Path, sys.argv[1:]))
with tempfile.TemporaryDirectory(prefix="loopz-control-") as temporary:
    root = Path(temporary)
    address = root / "run" / "control.sock"
    state = root / "state" / "engine.json"

    def launch():
        process = subprocess.Popen(
            [engine, "--socket", str(address), "--state", str(state)], stderr=subprocess.PIPE
        )
        for _ in range(100):
            if address.exists():
                return process
            if process.poll() is not None:
                raise AssertionError(process.stderr.read().decode())
            time.sleep(0.02)
        raise AssertionError("Engine socket did not become ready")

    def request(message):
        completed = subprocess.run(
            [cli, "--socket", str(address), "--request", json.dumps(message)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert completed.returncode in (0, 2), completed.stderr
        return json.loads(completed.stdout)

    def snapshot():
        return request({"version": 1, "operation": "state"})["snapshot"]

    process = launch()
    try:
        assert request({"version": 2, "operation": "hello"})["code"] == "unsupported-version"
        assert request({"version": 1, "operation": "hello"})["protocol"] == "loopz.engine"
        initial = snapshot()
        assert initial["document"] is None
        revision = initial["revision"]
        commands = [
            {
                "type": "createDevice",
                "id": "mix",
                "name": "Test",
                "channels": 2,
                "sampleRate": 48000,
                "passThrough": False,
            }
        ]
        # Use the same typed reducer command schema as the desktop.
        edit = {
            "version": 1,
            "operation": "initialize",
            "request": {"version": 1, "expected": revision, "commands": commands},
        }
        result = request(edit)
        assert result["ok"], result
        assert result["revision"]["generation"] == "1"
        assert request(edit)["code"] == "conflict"
        saved = snapshot()
        assert saved["document"]["devices"][0]["id"] == "mix"
        assert os.stat(state).st_mode & 0o777 == 0o600
        unsupported = {
            "version": 1,
            "operation": "edit",
            "request": {
                "version": 1,
                "expected": saved["revision"],
                "commands": [{"type": "setDeviceRate", "device": "mix", "sampleRate": 44100}],
            },
        }
        assert request(unsupported)["code"] == "unsupported-configuration"
        assert snapshot()["revision"] == saved["revision"]
        # Force pre-publication storage failure without relying on permission
        # bits (the Linux build container can run as root).
        original = state.read_bytes()
        state.unlink()
        state.mkdir()
        rename = {
            "version": 1,
            "operation": "edit",
            "request": {
                "version": 1,
                "expected": saved["revision"],
                "commands": [{"type": "renameDevice", "device": "mix", "name": "Updated"}],
            },
        }
        assert request(rename)["code"] == "storage-error"
        assert snapshot()["revision"] == saved["revision"]
        state.rmdir()
        state.write_bytes(original)
        state.chmod(0o600)
        assert request(rename)["ok"]
        saved = snapshot()
        assert saved["revision"]["generation"] == "2"
        duplicate = subprocess.run(
            [engine, "--socket", str(address), "--state", str(state)],
            capture_output=True,
            timeout=5,
        )
        assert duplicate.returncode != 0
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(3)
            connection.connect(str(address))
            connection.sendall(b'{"version":1,broken}\n')
            assert json.loads(connection.recv(8192))["code"] == "invalid-request"
        assert snapshot()["revision"] == saved["revision"]
    finally:
        process.terminate()
        process.wait(timeout=5)
    process = launch()
    try:
        restored = snapshot()
        assert restored["revision"]["epoch"] != saved["revision"]["epoch"]
        assert restored["revision"]["generation"] == "2"
        assert restored["document"] == saved["document"]
        assert request(edit)["code"] == "conflict"
    finally:
        process.terminate()
        process.wait(timeout=5)
    # A bounded saved document may produce larger telemetry because source
    # selectors also appear in connection status. Canonical state must remain
    # readable; the response budget is independent from the request budget.
    buses = []
    for bus_index in range(32):
        bus = json.loads(json.dumps(saved["document"]["devices"][0]))
        bus["id"] = f"wide-{bus_index}"
        bus["name"] = "n" * 4096
        bus["routing"]["sources"] = []
        bus["bindings"] = []
        for source_index in range(5):
            identity = f"source-{source_index}"
            bus["routing"]["sources"].append(
                {
                    "id": identity,
                    "channels": 2,
                    "gain": 1,
                    "enabled": False,
                    "muted": False,
                    "connections": [],
                }
            )
            bus["bindings"].append(
                {
                    "id": identity,
                    "kind": "input-device",
                    "selector": f"{bus_index}:{source_index}:"
                    + "x" * (4096 - len(f"{bus_index}:{source_index}:")),
                    "muteOriginal": False,
                }
            )
        buses.append(bus)
    wide = {"version": 1, "generation": "3", "document": {"version": 2, "devices": buses}}
    payload = json.dumps(wide)
    assert len(payload) < 1048576
    state.write_text(payload)
    process = launch()
    try:
        canonical = snapshot()
        assert len(canonical["document"]["devices"]) == 32
        assert canonical["revision"]["generation"] == "3"
        # This is an activatable graph within aggregate budgets, rather than
        # an oversized desired document merely retained after failed activation.
        assert canonical["restoration"]["activeGeneration"] == "3"
        assert not canonical["restoration"]["error"]
        assert len(json.dumps(canonical)) > 1048576
    finally:
        process.terminate()
        process.wait(timeout=5)
print("Standalone Engine control persistence, ownership and stale-revision checks passed")
