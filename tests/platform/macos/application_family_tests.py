"""Prepare the owned app/helper fixture and check it without acquiring audio IO.

The default invocation exercises real app identities and helper restarts. CTest
uses --bounds-only; real LaunchServices metadata requires an interactive macOS
user session. prepare() also builds the identical fixture for later signal tests.
"""

import json
import os
from pathlib import Path
import plistlib
import select
import shutil
import subprocess
import sys
import tempfile
import time


def prepare(executable, destination):
    destination = Path(destination)
    destination.mkdir()
    parent = destination / "Family.app"
    bundles = [(parent, "com.lexasoft.loopz.fixture.family", True)]
    for name in ("a", "b"):
        bundles.append(
            (
                parent / "Contents" / "Helpers" / f"{name}.app",
                f"com.lexasoft.loopz.fixture.family.helper.{name}",
                False,
            )
        )
    for bundle, identity, visible in bundles:
        binary = bundle / "Contents" / "MacOS" / "Fixture"
        binary.parent.mkdir(parents=True)
        shutil.copy2(executable, binary)
        info = {
            "CFBundleExecutable": "Fixture",
            "CFBundleIdentifier": identity,
            "CFBundleName": identity,
            "CFBundlePackageType": "APPL",
            "CFBundleVersion": "1",
            "LSMinimumSystemVersion": "14.2",
        }
        info["LSUIElement" if visible else "LSBackgroundOnly"] = True
        with (bundle / "Contents" / "Info.plist").open("wb") as stream:
            plistlib.dump(info, stream)
    # Sign children before their parent. These generated-only fixtures neither
    # record audio nor carry recording entitlements or production app identity.
    for bundle, _, _ in reversed(bundles):
        subprocess.run(
            ["/usr/bin/codesign", "--force", "--sign", "-", str(bundle)],
            check=True,
            capture_output=True,
        )
    return parent / "Contents" / "MacOS" / "Fixture"


def bounds(executable):
    cases = [
        [],
        ["--metadata-only", "--seconds", "2"],
        ["--metadata-only", "--seconds", "121"],
        ["--metadata-only", "--seconds", "nan"],
        ["--metadata-only", "--seconds", "99999999999999999999999"],
        ["--metadata-only", "--device-a", "default"],
        ["--device-a", "default", "--device-b", "default"],
        ["--child", "--metadata-only"],
        ["--unknown"],
    ]
    for arguments in cases:
        result = subprocess.run(
            [executable, *arguments], capture_output=True, text=True, timeout=10
        )
        assert result.returncode == 2, (arguments, result.returncode, result.stderr)
        assert not result.stdout, (arguments, result.stdout)


def read(process):
    deadline = time.monotonic() + 8
    data = bytearray()
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], 0.1)[0]:
            byte = os.read(process.stdout.fileno(), 1)
            assert byte, (process.poll(), data)
            if byte == b"\n":
                return json.loads(data)
            data += byte
            assert len(data) < 16384
    raise TimeoutError("Family fixture did not acknowledge control")


def metadata(executable):
    with tempfile.TemporaryDirectory(prefix="loopz-family-") as folder:
        binary = prepare(executable, Path(folder) / "fixture")
        with subprocess.Popen(
            [str(binary), "--metadata-only", "--seconds", "30"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ) as process:
            reports = []
            try:
                for event, count in (
                    (None, 1),
                    ("start-b", 2),
                    ("restart-b", 2),
                    ("stop-b", 1),
                    ("start-b", 2),
                    ("quit", 0),
                ):
                    if event:
                        process.stdin.write((event + "\n").encode())
                        process.stdin.flush()
                    report = read(process)
                    reports.append(report)
                    assert report["parentPID"] == process.pid, report
                    assert report["coherent"] and report["roots"] == 1, report
                    assert report["metadataOnly"] and not report["parentOpenedAudio"], report
                    assert report["selectedCount"] == len(report["children"]) == count, report
                    assert report["event"] == (event or "ready") or (
                        event == "quit" and report["event"] == "complete"
                    ), report
                before = reports[1]["children"][1]
                after = reports[2]["children"][1]
                assert (before["pid"], before["generation"]) != (after["pid"], after["generation"])
                assert reports[0]["children"][0] == reports[4]["children"][0]
                assert process.wait(timeout=5) == 0, process.stderr.read().decode()
                return reports
            finally:
                if process.poll() is None:
                    process.stdin.close()
                    process.wait(timeout=8)


if __name__ == "__main__":
    bounds(sys.argv[1])
    if "--bounds-only" in sys.argv[2:]:
        print("Family fixture rejects invalid requests before opening IO")
    else:
        print(json.dumps({"status": "passed", "reports": metadata(sys.argv[1])}, indent=2))
