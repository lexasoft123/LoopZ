"""Capture-free timing diagnostic rejects invalid requests before IO."""

import subprocess
import sys

base = ["--device-uid", "com.lexasoft.loopz.device.missing-timing-check"]
for arguments in (
    [],
    ["--device-uid", "default"],
    base,
    [*base, "--seconds", "2"],
    [*base, "--seconds", "11"],
    [*base, "--seconds", "-1"],
    [*base, "--seconds", "nan"],
    [*base, "--buffer-frames", "16"],
    [*base, "--buffer-frames", "0"],
    [*base, "--buffer-frames", "96"],
    [*base, "--buffer-frames", "4096"],
    [*base, "--buffer-frames", "999999999999999999"],
    [*base, "--unknown", "1"],
    [*base, "--seconds"],
):
    result = subprocess.run(
        [sys.argv[1], "--routing-timing-check", *arguments],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2 and not result.stdout, (arguments, result.stderr, result.stdout)
print("Timing-check bounds and virtual-only guard passed without IO")
