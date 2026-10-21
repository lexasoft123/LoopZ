"""Input-only comparison rejects invalid requests before IO or permission acquisition."""

import subprocess
import sys

base = ["--read-device", "com.lexasoft.loopz.device.missing-input-comparison"]
for arguments in (
    [],
    ["--read-device", "default"],
    base,
    [*base, "--signal", "sine"],
    [*base, "--signal", "marker"],
    [*base, "--signal", "anything"],
    [*base, "--capture-protocol", "stdio-v1"],
    [*base, "--capture-protocol", "stdio-v2"],
    [*base, "--capture-protocol", "stdio-v1", "--capture-protocol", "stdio-v1"],
    [*base, "--seconds", "2"],
    [*base, "--seconds", "121"],
    [*base, "--seconds", "nan"],
    [*base, "--seconds", "-1"],
    [*base, "--buffer-frames", "16"],
    [*base, "--buffer-frames", "96"],
    [*base, "--buffer-frames", "4096"],
    [*base, "--buffer-frames", "999999999999999999999"],
    [*base, "--frequency-left", "1100x"],
    [*base, "--frequency-left", "nan"],
    [*base, "--send-tone", "true"],
    [*base, "--read-device", "other"],
    [*base, "--seconds"],
):
    result = subprocess.run(
        [sys.argv[1], "--virtual-input-check", *arguments],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2 and not result.stdout, (arguments, result)
print("Virtual input-only diagnostic bounds and device guard passed before IO")
