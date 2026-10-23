"""Atomic acceptance reports that tolerate short Windows reader locks."""

import json
import time


def write_report(path, report):
    temporary = path.with_suffix(".pending")
    temporary.write_text(json.dumps(report, indent=2), encoding="utf-8")
    deadline = time.monotonic() + 5
    while True:
        try:
            temporary.replace(path)
            return
        except PermissionError as error:
            # Windows readers may omit FILE_SHARE_DELETE. Keep the old report
            # intact and retry only the rename, never a partial in-place write.
            if getattr(error, "winerror", None) not in (5, 32, 33):
                raise
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)
