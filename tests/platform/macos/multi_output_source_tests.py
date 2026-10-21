"""The dual-output fixture rejects invalid requests before acquiring IO."""

import subprocess
import sys

base = [
    "--device-a",
    "com.lexasoft.loopz.device.missing-a",
    "--device-b",
    "com.lexasoft.loopz.device.missing-b",
]
for arguments in [
    [],
    ["--device-a", "default", "--device-b", "default"],
    ["--device-a", "BuiltInSpeakerDevice", "--device-b", "BuiltInMicrophoneDevice"],
    [*base, "--seconds", "0"],
    [*base, "--seconds", "2"],
    [*base, "--seconds", "86401"],
    [*base, "--seconds", "nan"],
    [*base, "--signal", "unknown"],
    [*base, "--signal", "marker"],
    [*base, "--seconds", "999999999999999999999999"],
    [*base, "--auto-start-b-ms", "30001"],
    [*base, "--buffer-frames", "0"],
    [*base, "--buffer-frames", "16"],
    [*base, "--buffer-frames", "96"],
    [*base, "--buffer-frames", "4096"],
    [*base, "--buffer-b-frames", "0"],
    [*base, "--buffer-b-frames", "96"],
    [*base, "--buffer-b-frames", "999999999999999999999"],
    [*base, "--device-b"],
    base,
]:
    result = subprocess.run(
        [sys.argv[1], "--multi-output-source", *arguments],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2, (arguments, result.returncode, result.stderr)
    assert not result.stdout, (arguments, result.stdout)
print("Multi-output fixture bounds and virtual-only guard passed without IO")
