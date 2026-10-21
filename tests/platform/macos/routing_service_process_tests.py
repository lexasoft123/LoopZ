"""Three-process XPC test using a temporary user-domain launchd job."""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import uuid


source = Path(sys.argv[1]).resolve()
label = f"com.lexasoft.loopz.routing-test.{uuid.uuid4().hex}"
domain = f"gui/{os.getuid()}"


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


with tempfile.TemporaryDirectory(prefix="loopz-routing-xpc-") as directory:
    folder = Path(directory)
    executable = folder / source.name
    shutil.copy2(source, executable)
    job = folder / "broker.plist"
    broker_log = folder / "broker.log"
    job.write_bytes(
        plistlib.dumps(
            {
                "Label": label,
                "ProgramArguments": [str(executable), "--broker-fixture", label],
                "MachServices": {label: True},
                "KeepAlive": False,
                "RunAtLoad": False,
                "StandardOutPath": str(broker_log),
                "StandardErrorPath": str(broker_log),
            }
        )
    )
    job.chmod(0o600)
    bootstrapped = False
    driver = None
    try:
        subprocess.run(["launchctl", "bootstrap", domain, str(job)], check=True, timeout=15)
        bootstrapped = True
        driver = subprocess.Popen(
            [str(executable), "--driver-fixture", label],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        driver_pids = [driver.pid]
        helper_pids = []
        for index in range(3):
            if index == 2:
                stop(driver)
                driver = subprocess.Popen(
                    [str(executable), "--driver-fixture", label],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                driver_pids.append(driver.pid)
            result = subprocess.run(
                [str(executable), "--helper-fixture", label],
                text=True,
                capture_output=True,
                timeout=45,
            )
            assert result.returncode == 0, (result.stdout, result.stderr)
            report = json.loads(result.stdout.strip().splitlines()[-1])
            assert report["passed"] and report["devices"] == 3
            helper_pids.append(report["pid"])
        assert len(set(helper_pids + driver_pids)) == 5
        print(
            json.dumps(
                {
                    "passed": True,
                    "helperPIDs": helper_pids,
                    "driverPIDs": driver_pids,
                    "cases": [
                        "three-process signed XPC relay",
                        "2/8/64-channel shared-memory aliases across processes",
                        "helper process replacement against surviving driver",
                        "driver process replacement and full configuration restoration",
                    ],
                }
            )
        )
    except BaseException:
        if broker_log.exists():
            print(broker_log.read_text()[-4000:], file=sys.stderr)
        raise
    finally:
        stop(driver)
        if bootstrapped:
            cleanup = subprocess.run(
                ["launchctl", "bootout", f"{domain}/{label}"],
                text=True,
                capture_output=True,
                timeout=15,
            )
            assert cleanup.returncode == 0, cleanup.stderr
