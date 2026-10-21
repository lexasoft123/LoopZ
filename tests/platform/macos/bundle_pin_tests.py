"""Validate the real bundle-pin loader in isolated, locally signed fixtures."""

import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile


def main(binary, runner):
    source = binary.parents[2]
    expected = 'identifier "com.lexasoft.loopz.bundle-pin-test.broker"'
    valid = {"protocol": 2, "brokerRequirement": expected}
    malformed = [
        {},
        {**valid, "extra": True},
        {**valid, "protocol": True},
        {**valid, "protocol": "2"},
        {**valid, "protocol": 1},
        {**valid, "protocol": 3},
        {**valid, "protocol": 2.5},
        {**valid, "brokerRequirement": None},
        {**valid, "brokerRequirement": ""},
        {**valid, "brokerRequirement": "x" * 4097},
        {**valid, "brokerRequirement": expected + "\0discard"},
        {**valid, "brokerRequirement": "this is not a signing requirement"},
    ]
    checks = []
    cases = [
        ("valid sealed pin", valid, "valid"),
        ("valid sealed pin in legacy slot", valid, "legacy-valid"),
        *[
            (f"malformed signed pin {index}", value, "invalid")
            for index, value in enumerate(malformed)
        ],
        ("modified after signing", valid, "tamper"),
        ("missing pin", valid, "missing"),
        ("unsigned bundle", valid, "unsigned"),
        ("wrong signed bundle identity", valid, "identity"),
        ("symlink pin", valid, "symlink"),
        ("oversized pin", valid, "oversized"),
    ]
    with tempfile.TemporaryDirectory(prefix="loopz-bundle-pin-") as directory:
        folder = Path(directory)
        for index, (name, value, mode) in enumerate(cases):
            bundle = folder / str(index) / source.name
            shutil.copytree(source, bundle)
            resources = bundle / "Contents/Resources"
            resources.mkdir(exist_ok=True)
            pin = resources / "routing-service.json"
            pin.write_text(json.dumps(value))
            if mode == "missing":
                pin.unlink()
            if mode == "oversized":
                pin.write_bytes(b" " * 16385)
            if mode == "symlink":
                target = folder / "outside.json"
                target.write_text(json.dumps(valid))
                pin.unlink()
                pin.symlink_to(target)
            identifier = "com.lexasoft.loopz.driver.routing"
            if mode == "legacy-valid":
                identifier = "com.lexasoft.loopz.driver"
            if mode == "identity":
                identifier = "com.lexasoft.loopz.bundle-pin-test.wrong"
            subprocess.run(
                [
                    "codesign",
                    "--force",
                    "--sign",
                    "-",
                    "--timestamp=none",
                    "--identifier",
                    identifier,
                    str(bundle),
                ],
                check=True,
                capture_output=True,
                timeout=20,
            )
            if mode == "tamper":
                pin.write_text(json.dumps({**valid, "brokerRequirement": 'identifier "changed"'}))
            if mode == "unsigned":
                subprocess.run(
                    ["codesign", "--remove-signature", str(bundle)],
                    check=True,
                    capture_output=True,
                    timeout=20,
                )
            result = subprocess.run(
                [
                    str(runner),
                    str(bundle / "Contents/MacOS/LoopZRoutingAudio"),
                ],
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert result.returncode == 0, (name, result.stdout, result.stderr)
            report = json.loads(result.stdout)
            if mode in ("valid", "legacy-valid"):
                assert report["status"] == 0 and report["value"] == expected, (name, report)
                assert report["stage"] == "loader"
            else:
                assert report["status"] == 1, (name, report)
            checks.append({"name": name, "stage": report["stage"]})
        info = plistlib.loads((source / "Contents/Info.plist").read_bytes())
        assert info["AudioServerPlugIn_MachServices"] == ["com.lexasoft.loopz.routing.bridge"]
    print(
        json.dumps(
            {
                "passed": True,
                "checks": checks,
                "scope": "Real sealed-resource loader; locally signed fixtures, no installed HAL activation",
            }
        )
    )


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
