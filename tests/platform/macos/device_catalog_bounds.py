"""Invalid observer requests fail before any native device lookup or IO."""

import subprocess
import sys

prefix = "com.lexasoft.loopz.device."
for arguments in (
    [], ["default"], [prefix + "x" * 129], [prefix + "same"] * 2,
    [prefix + str(index) for index in range(17)],
):
    result = subprocess.run(
        [sys.argv[1], *arguments], capture_output=True, text=True, timeout=5
    )
    assert result.returncode == 2 and not result.stdout, (arguments, result)
print("Observer request bounds passed before native queries")
