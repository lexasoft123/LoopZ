"""Invalid diagnostic requests cannot open a capture or physical output."""

import subprocess
import sys

for arguments in [
    [],
    ["--channels", "65"],
    ["--seconds", "31"],
    ["--channels", "-1"],
    ["--expected-gain", "nan"],
    ["--recovery-protocol", "unknown"],
    ["--recovery-protocol", "stdio-v1", "--recovery-protocol", "stdio-v1"],
    ["--recovery-protocol", "stdio-v1", "--seconds", "5"],
    ["--recovery-protocol", "stdio-v1", "--expected-gain", "0"],
    ["--recovery-protocol", "stdio-v2", "--seconds", "5"],
    ["--recovery-protocol", "stdio-v2", "--recovery-protocol", "stdio-v1"],
    ["--capture-protocol", "unknown"],
    ["--capture-protocol", "stdio-v1", "--capture-protocol", "stdio-v1"],
    ["--capture-protocol", "stdio-v1", "--recovery-protocol", "stdio-v2"],
    ["--prepare-timeout-seconds", "90"],
    ["--startup-events", "stdio-v1"],
    ["--capture-protocol", "stdio-v1", "--startup-events", "no"],
    [
        "--capture-protocol",
        "stdio-v1",
        "--startup-events",
        "stdio-v1",
        "--startup-events",
        "stdio-v1",
    ],
    *[
        ["--capture-protocol", "stdio-v1", "--prepare-timeout-seconds", value]
        for value in ("0", "19", "121", "-1", "90x", "nan", "999999999999999999999999")
    ],
    [
        "--capture-protocol",
        "stdio-v1",
        "--prepare-timeout-seconds",
        "90",
        "--prepare-timeout-seconds",
        "90",
    ],
    [
        "--capture-protocol",
        "stdio-v1",
        "--prepare-timeout-seconds",
        "90",
        "--startup-events",
        "stdio-v1",
        "--write-device",
        "BuiltInSpeakerDevice",
        "--read-device",
        "BuiltInMicrophoneDevice",
    ],
    [
        "--capture-protocol",
        "stdio-v1",
        "--write-device",
        "BuiltInSpeakerDevice",
        "--read-device",
        "BuiltInMicrophoneDevice",
    ],
    [
        "--recovery-protocol",
        "stdio-v2",
        "--write-device",
        "BuiltInSpeakerDevice",
        "--read-device",
        "BuiltInMicrophoneDevice",
    ],
    ["--buffer-frames", "0"],
    ["--buffer-frames", "16"],
    ["--buffer-frames", "96"],
    ["--buffer-frames", "4096"],
    ["--buffer-frames", "999999999999999999999999"],
    ["--write-device", "default", "--read-device", "default"],
    ["--write-device", "BuiltInSpeakerDevice", "--read-device", "BuiltInMicrophoneDevice"],
    [
        "--write-device",
        "com.lexasoft.loopz.device.missing",
        "--read-device",
        "com.lexasoft.loopz.device.missing",
    ],
]:
    result = subprocess.run(
        [sys.argv[1], "--routing-io-check", *arguments], capture_output=True, text=True, timeout=10
    )
    assert result.returncode == 2, (arguments, result.returncode, result.stderr)
    assert "permissionRequired" not in result.stdout
    assert '"event"' not in result.stdout
print("Routing diagnostic bounds and physical-device exclusion passed before permission or IO")

for arguments in [
    [],
    ["--seconds", "2"],
    ["--seconds", "86401"],
    ["--seconds", "-1"],
    ["--seconds", "no"],
    ["--expect", "audio"],
    ["--tone-components", "0"],
    ["--tone-components", "3"],
    ["--tone-components", "-1"],
    ["--tone-components", "2", "--expect", "silence"],
    ["--frequency-left", "nan"],
    ["--frequency-right", "20001"],
    ["--frequency-left", "0"],
    ["--frequency-left", "2200"],
    ["--buffer-frames", "0"],
    ["--buffer-frames", "96"],
    ["--read-device", "default"],
    ["--read-device", "BuiltInMicrophoneDevice"],
    ["--read-device", "com.lexasoft.loopz.device.missing"],
]:
    result = subprocess.run(
        [sys.argv[1], "--routing-signal-check", *arguments],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2, (arguments, result.returncode, result.stderr)
    assert "permissionRequired" not in result.stdout
print("Routing signal reader bounds exclude physical IO and permission requests")
