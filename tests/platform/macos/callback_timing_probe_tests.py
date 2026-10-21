"""Malformed or physical-device requests cannot reach device inspection."""

import subprocess
import sys

owned = "com.lexasoft.loopz.device.fixture"
cases = [
    [],
    ["--device"],
    ["--unknown", owned],
    ["--device", "BuiltInSpeakerDevice"],
    ["--device", "default"],
    ["--device", owned, "--device", "BuiltInMicrophoneDevice"],
    ["--device", owned, "--device", owned],
    ["--device", owned + "x" * 4096],
    [item for index in range(33) for item in ("--device", owned + str(index))],
    [b"--device", owned.encode() + b"\xff"],
]
for arguments in cases:
    result = subprocess.run([sys.argv[1], *arguments], capture_output=True, timeout=5)
    assert result.returncode == 2 and not result.stdout, (
        arguments,
        result.returncode,
        result.stdout,
        result.stderr,
    )
print("Callback timing probe rejects malformed, duplicate and physical-device scope")
