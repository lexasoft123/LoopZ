"""The first-install helper never accepts caller-selected commands or paths."""

import subprocess
import sys

helper = sys.argv[1]
for arguments in ([], ["--install", "/tmp"], ["--verify-payload", "/tmp"], ["--verify-payload"]):
    result = subprocess.run([helper, *arguments], capture_output=True, text=True, timeout=5)
    assert result.returncode == 1, (arguments, result.stdout, result.stderr)
    assert "administrator authorization" in result.stderr or "Run audio setup from" in result.stderr
