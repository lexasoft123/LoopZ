"""Build a real parent consuming LoopZ Runtime and one pinned zCore checkout."""

import argparse
from pathlib import Path
import subprocess
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--zcore", type=Path)
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    zcore = (args.zcore or root / "vendor/zcore").resolve()
    build = (args.build or root / "artifacts/sdk-consumer").resolve()
    source = root / "tests/sdk_consumer"
    common = [
        "cmake",
        "-S",
        str(source),
        f"-DLOOPZ_SOURCE:PATH={root}",
        f"-DSHARED_ZCORE_SOURCE:PATH={zcore}",
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    subprocess.run([*common, "-B", str(build)], check=True, timeout=120)
    subprocess.run(
        ["cmake", "--build", str(build), "--target", "loopz-sdk-consumer", "--parallel", "2"],
        check=True,
        timeout=300,
    )
    subprocess.run(
        ["ctest", "--test-dir", str(build), "--output-on-failure"], check=True, timeout=30
    )
    wrong = subprocess.run(
        [
            *common,
            "-B",
            str(build.with_name(build.name + "-wrong-source")),
            f"-DLOOPZ_SHARED_ZCORE_ROOT:PATH={root}",
        ],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert wrong.returncode != 0 and "SOURCE_DIR does not match" in wrong.stderr, (
        wrong.stdout + wrong.stderr
    )
    overlays = build.parent / "sdk-consumer-overlays"
    overlays.mkdir(parents=True, exist_ok=True)
    for basename, option in (
        ("graph_runner.cpp", "ZCORE_GRAPH_RUNNER_SOURCE"),
        ("builtin_nodes.cpp", "ZCORE_BUILTIN_NODES_SOURCE"),
    ):
        overlay = overlays / basename
        shutil.copyfile(zcore / "zdsp/src/runtime" / basename, overlay)
        for reset in (False, True):
            case = basename.replace(".cpp", "") + ("-cache-reset" if reset else "-overlay")
            configured = subprocess.run(
                [
                    *common,
                    "-B",
                    str(build.with_name(build.name + "-" + case)),
                    f"-D{option}:FILEPATH={overlay}",
                    f"-DSDK_RESET_OVERLAY_CACHE={'ON' if reset else 'OFF'}",
                ],
                capture_output=True,
                text=True,
                timeout=120,
            )
            assert (
                configured.returncode != 0
                and "DSP source is outside pinned zCore" in configured.stderr
            ), (configured.stdout + configured.stderr)
    for suffix, selected in (
        ("late-overlay", str(overlays / "graph_runner.cpp")),
        ("late-genexpr", f"$<$<CONFIG:Release>:{overlays / 'graph_runner.cpp'}>"),
    ):
        late = build.with_name(build.name + "-" + suffix)
        subprocess.run(
            [*common, "-B", str(late), f"-DSDK_POST_INCLUDE_OVERLAY:STRING={selected}"],
            check=True,
            timeout=120,
        )
        rejected = subprocess.run(
            ["cmake", "--build", str(late), "--target", "loopz_verify_zcore"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert (
            rejected.returncode != 0 and "DSP source is outside pinned zCore" in rejected.stderr
        ), (rejected.stdout + rejected.stderr)
    # The regular build above exercises an unset parent deployment policy.
    # This explicit sentinel must also survive child project configuration.
    subprocess.run(
        [
            *common,
            "-B",
            str(build.with_name(build.name + "-deployment")),
            "-DSDK_PARENT_DEPLOYMENT_TARGET=13.1",
        ],
        check=True,
        timeout=120,
    )
    print(
        "SDK parent retains tests/deployment, runs native sources, and rejects constructed/late DSP overlays"
    )


if __name__ == "__main__":
    main()
