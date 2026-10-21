"""Reject invalid private capture periods before connecting or requesting privacy."""

import subprocess
import sys

for value in ("0", "16", "96", "256", "-1", "nan", "64x", "999999999999999999999"):
    result = subprocess.run(
        [
            sys.argv[1],
            "--routing-document",
            "/not-an-existing-loopz-test-document.json",
            "--application-buffer-frames",
            value,
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2 and not result.stdout, (value, result)
print("Invalid application capture periods rejected before runtime startup")

for value in ("0", "1", "yes", "", "TRUE"):
    result = subprocess.run(
        [sys.argv[1], "--routing-document", "/not-an-existing-loopz-test-document.json",
         "--capture-delivery-diagnostics", value],
        capture_output=True, text=True, timeout=10,
    )
    assert result.returncode == 2 and not result.stdout, (value, result)
print("Invalid capture delivery diagnostic switches rejected before runtime startup")
