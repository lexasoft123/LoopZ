"""Capture-free validation-mode argument and telemetry checks."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

engine = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="loopz-routing-cli-") as temporary:
    path = Path(temporary) / "routes.json"
    path.write_text('{"version":2,"devices":[]}')
    result = subprocess.run(
        [
            str(engine),
            "--routing-document",
            str(path),
            "--installation-validation",
            "true",
            "--seconds",
            "1",
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 0, (result.stdout, result.stderr)
    states = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    assert states and all(
        state["mode"] == "routingDocument"
        and state["installationValidation"] is True
        and state["inputResamplingFilter"] == "linear-phase"
        and state["physicalInputDelayFrames"] is None
        and state["fallbackSourceDelayFrames"] == 1024
        and state["inputTimingPolicy"] == "automatic"
        and state["automaticApplicationTiming"] is True
        and isinstance(state["serviceSession"], int)
        and state["devices"] == []
        and state["sources"] == []
        and state["monitors"] == []
        for state in states
    )
    assert states[0]["started"] and not states[-1]["started"]
    assert all("audio" not in state and "audioStatusError" not in state for state in states)
    detailed = subprocess.run(
        [
            str(engine),
            "--routing-document",
            str(path),
            "--installation-validation",
            "true",
            "--status-levels",
            "true",
            "--seconds",
            "1",
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert detailed.returncode == 0, (detailed.stdout, detailed.stderr)
    levels = [json.loads(line) for line in detailed.stdout.splitlines() if line.startswith("{")]
    assert levels and all(
        "audioStatusError" not in state
        and state["audio"]["devices"] == []
        and state["audio"]["sources"] == []
        and state["audio"]["monitors"] == []
        and isinstance(state["audio"]["renderBlocks"], str)
        and "microphone" in state["audio"]["permissions"]
        for state in levels
    )
    assert levels[0]["audio"]["started"] and not levels[-1]["audio"]["started"]
    minimum_phase = subprocess.run(
        [
            str(engine),
            "--routing-document",
            str(path),
            "--installation-validation",
            "true",
            "--experimental-input-filter",
            "minimum-phase",
            "--source-delay-frames",
            "192",
            "--seconds",
            "1",
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert minimum_phase.returncode == 0, (minimum_phase.stdout, minimum_phase.stderr)
    selections = [
        json.loads(line) for line in minimum_phase.stdout.splitlines() if line.startswith("{")
    ]
    assert selections and all(
        state["inputResamplingFilter"] == "minimum-phase"
        and state["installationValidation"] is True
        and state["sources"] == []
        and state["automaticApplicationTiming"] is False
        for state in selections
    )
    scoped_arguments = list(minimum_phase.args)
    scoped_arguments[scoped_arguments.index("--source-delay-frames")] = (
        "--physical-input-delay-frames"
    )
    scoped = subprocess.run(scoped_arguments, capture_output=True, text=True, timeout=10)
    assert scoped.returncode == 0, (scoped.stdout, scoped.stderr)
    scoped_states = [
        json.loads(line) for line in scoped.stdout.splitlines() if line.startswith("{")
    ]
    assert scoped_states and all(
        state["physicalInputDelayFrames"] == 192
        and state["fallbackSourceDelayFrames"] == 1024
        and state["inputResamplingFilter"] == "minimum-phase"
        and state["sources"] == []
        and state["inputTimingPolicy"] == "physical-override"
        for state in scoped_states
    )
    automatic_arguments = list(scoped_arguments)
    index = automatic_arguments.index("--physical-input-delay-frames")
    del automatic_arguments[index : index + 2]
    automatic = subprocess.run(automatic_arguments, capture_output=True, text=True, timeout=10)
    assert automatic.returncode == 0, (automatic.stdout, automatic.stderr)
    automatic_states = [
        json.loads(line) for line in automatic.stdout.splitlines() if line.startswith("{")
    ]
    assert automatic_states and all(
        state["inputResamplingFilter"] == "minimum-phase"
        and state["inputTimingPolicy"] == "automatic"
        and state["physicalInputDelayFrames"] is None
        and state["fallbackSourceDelayFrames"] == 1024
        and state["sources"] == []
        for state in automatic_states
    )
    for profile, error in (
        (["--automatic-application-timing", "yes"], "requires true or false"),
        (
            ["--automatic-application-timing", "true", "--source-delay-frames", "256"],
            "conflicts with a global source-delay override",
        ),
        (
            ["--source-delay-frames", "256", "--automatic-application-timing", "true"],
            "conflicts with a global source-delay override",
        ),
        (["--experimental-input-filter", "other"], "requires linear-phase or minimum-phase"),
        (["--physical-input-delay-frames", "16385"], "Invalid numeric routing option"),
    ):
        invalid = subprocess.run(
            [str(engine), "--routing-document", str(path), *profile],
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert invalid.returncode == 2 and error in invalid.stderr
    adaptive = subprocess.run(
        [*automatic_arguments, "--automatic-application-timing", "true"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert adaptive.returncode == 0, (adaptive.stdout, adaptive.stderr)
    app_states = [json.loads(line) for line in adaptive.stdout.splitlines() if line.startswith("{")]
    assert app_states and all(
        s["automaticApplicationTiming"] and s["sources"] == [] for s in app_states
    )
    disabled = subprocess.run(
        [*automatic_arguments, "--automatic-application-timing", "false"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert disabled.returncode == 0, (disabled.stdout, disabled.stderr)
    disabled_states = [
        json.loads(line) for line in disabled.stdout.splitlines() if line.startswith("{")
    ]
    assert disabled_states and all(
        not s["automaticApplicationTiming"] and s["sources"] == [] for s in disabled_states
    )
    for wrong in ("false", "yes", ""):
        invalid = subprocess.run(
            [str(engine), "--routing-document", str(path), "--status-levels", wrong],
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert invalid.returncode == 2 and "Unknown routing option" in invalid.stderr
    result = subprocess.run(
        [
            str(engine),
            "--routing-document",
            str(path),
            "--installation-validation",
            "false",
            "--seconds",
            "1",
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2 and "Unknown routing option" in result.stderr
print(
    json.dumps(
        {
            "passed": True,
            "captureOpened": False,
            "checks": [
                "explicit validation policy",
                "bounded CLI run and structured endpoint telemetry",
                "optional per-source levels and missing-frame counters",
                "invalid policy rejected",
                "experimental input filter supports automatic or explicit physical-input budgets",
                "default and experimental selections reported without capture acquisition",
            ],
        }
    )
)
