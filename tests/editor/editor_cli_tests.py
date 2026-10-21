"""Editor commands cannot be enabled in the installer-only Engine mode."""

from pathlib import Path
import json
import subprocess
import sys
import tempfile

capabilities = subprocess.run(
    [sys.argv[1], "--runtime-capabilities"], capture_output=True, text=True, timeout=10
)
assert capabilities.returncode == 0
assert json.loads(capabilities.stdout)["backgroundSession"] == 1


with tempfile.TemporaryDirectory(prefix="loopz-editor-cli-") as temporary:
    document = Path(temporary) / "routes.json"
    document.write_text('{"version":2,"devices":[]}')
    result = subprocess.run(
        [
            sys.argv[1],
            "--routing-document",
            str(document),
            "--installation-validation",
            "true",
            "--editor-service",
            "true",
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2, (result.stdout, result.stderr)
    assert "Installation validation cannot expose editor commands" in result.stderr
    assert not result.stdout
    assert document.read_text() == '{"version":2,"devices":[]}'
