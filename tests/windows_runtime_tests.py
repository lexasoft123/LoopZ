"""Exercise Windows persistence/revision/history with an isolated document."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="loopz-runtime-\u044f-") as temporary:
    result = subprocess.run(
        [sys.argv[1], "--self-test", temporary],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=60,
    )
    report = json.loads(result.stdout)
    assert report["ok"] is True, report
    assert set(report["checks"]) == {
        "create",
        "stale revision",
        "undo",
        "redo",
        "save",
        "reopen",
        "recover unsupported format",
        "app playback edits",
    }, report
    assert (Path(temporary) / "test-routing.json").is_file()
    assert (Path(temporary) / "test-routing.json.bak").is_file()
    print(result.stdout, end="")
