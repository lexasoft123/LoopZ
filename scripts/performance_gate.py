#!/usr/bin/env python3
"""Compare exact Git trees in silent, isolated, same-machine Release workloads."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import statistics
import subprocess
import sys
import tempfile


HARNESS = "tests/performance/routing_benchmark.cpp"
PREFIXES = ("native/", "cmake/", "vendor/", "tests/performance/")
FILES = {
    "CMakeLists.txt",
    "scripts/performance_gate.py",
    "tests/performance_gate_tests.py",
    ".github/workflows/performance.yml",
}
PROFILES = {
    (channels, frames, limiter)
    for channels in (2, 64)
    for frames in (32, 64, 256)
    for limiter in (False, True)
}
PAIRS = 5
RELATIVE_LIMIT = 0.15
ABSOLUTE_LIMIT_US = 0.25


class GateError(RuntimeError):
    pass


def run(command, *, cwd=None, env=None, timeout=600):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, timeout=timeout)
    if result.returncode:
        raise GateError(
            f"Command failed ({result.returncode}): {command!r}\n"
            + result.stderr.decode(errors="replace")[-4000:]
            + result.stdout.decode(errors="replace")[-2000:]
        )
    return result.stdout


def git(root, *args, env=None):
    return run(["git", "-C", str(root), *args], env=env)


def clean_git_environment(root):
    # A caller can supply a temporary GIT_INDEX_FILE. Honor it when
    # capturing the candidate, but never leak it into either isolated clone.
    names = git(root, "rev-parse", "--local-env-vars").decode().splitlines()
    return {key: value for key, value in os.environ.items() if key not in names}


def relevant(paths):
    return any(path in FILES or path.startswith(PREFIXES) for path in paths)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(root, tree, destination, env):
    # Shared object storage is read-only input; each snapshot has its own index,
    # working files and build. No synthetic commits or original-index changes.
    run(["git", "clone", "--shared", "--no-checkout", str(root), str(destination)], env=env)
    git(destination, "read-tree", tree, env=env)
    git(destination, "checkout-index", "--all", env=env)
    entries = git(destination, "ls-files", "--stage", "-z", env=env).split(b"\0")
    for entry in filter(None, entries):
        metadata, raw_path = entry.split(b"\t", 1)
        mode, oid, stage = metadata.decode().split()
        if mode != "160000":
            continue
        path = raw_path.decode()
        relative = PurePosixPath(path)
        if relative.is_absolute() or ".." in relative.parts or "\\" in path or stage != "0":
            raise GateError(f"Invalid submodule path: {path!r}")
        source = root / path
        if not source.is_dir():
            raise GateError(f"Initialize the pinned submodule before committing: {path}")
        child = destination / path
        # checkout-index leaves gitlink directories empty, if it creates them.
        child.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--shared", "--no-checkout", str(source), str(child)], env=env)
        git(child, "checkout", "--detach", oid, env=env)
        if any(
            row.startswith(b"160000 ")
            for row in git(child, "ls-files", "--stage", "-z", env=env).split(b"\0")
        ):
            raise GateError("Nested submodules require explicit performance snapshot support")


def build(snapshot_root, folder, harness, env):
    folder.mkdir()
    # Use the exact same staged workload in both dependency/core versions.
    # Configuring the product as a subdirectory also exercises its real source
    # selection, overlays, immutable dependency checks and compiler policy.
    (folder / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(LoopZPerformance LANGUAGES C CXX)\n"
        "set(CMAKE_CXX_STANDARD 20)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        'set(LOOPZ_BUILD_MACOS_BACKEND OFF CACHE BOOL "" FORCE)\n'
        'set(LOOPZ_BUILD_WINDOWS_BACKEND OFF CACHE BOOL "" FORCE)\n'
        'set(LOOPZ_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)\n'
        'set(BUILD_TESTING OFF CACHE BOOL "" FORCE)\n'
        'add_subdirectory("${LOOPZ_SNAPSHOT}" loopz)\n'
        'add_executable(loopz-perf-gate "${LOOPZ_PERF_HARNESS}")\n'
        "target_link_libraries(loopz-perf-gate PRIVATE LoopZ::Core)\n"
        "get_target_property(loopz_crt loopz_core MSVC_RUNTIME_LIBRARY)\n"
        "if(loopz_crt)\n"
        '  set_property(TARGET loopz-perf-gate PROPERTY MSVC_RUNTIME_LIBRARY "${loopz_crt}")\n'
        "endif()\n"
    )
    binary_dir = folder / "build"
    for name, command in (
        (
            "configure",
            [
                "cmake",
                "-S",
                str(folder),
                "-B",
                str(binary_dir),
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DLOOPZ_SNAPSHOT:PATH={snapshot_root.as_posix()}",
                f"-DLOOPZ_PERF_HARNESS:FILEPATH={harness.as_posix()}",
            ],
        ),
        (
            "build",
            [
                "cmake",
                "--build",
                str(binary_dir),
                "--config",
                "Release",
                "--target",
                "loopz-perf-gate",
                "--parallel",
                "4",
            ],
        ),
    ):
        try:
            output = run(command, env=env)
        except GateError as error:
            (folder / f"{name}.log").write_text(str(error))
            raise
        (folder / f"{name}.log").write_bytes(output)
    suffix = ".exe" if os.name == "nt" else ""
    candidates = [
        binary_dir / ("loopz-perf-gate" + suffix),
        binary_dir / "Release" / ("loopz-perf-gate" + suffix),
    ]
    binaries = [path for path in candidates if path.is_file()]
    if len(binaries) != 1:
        raise GateError("Could not identify the exact Release benchmark executable")
    return binaries[0]


def parse_measurement(output):
    profiles = {}
    for line in output.decode().splitlines():
        row = json.loads(line)
        key = (row["channels"], row["frames"], row["limiter"])
        metric = row["medianUs"]
        if (
            type(row["channels"]) is not int
            or type(row["frames"]) is not int
            or type(row["limiter"]) is not bool
            or key not in PROFILES
            or key in profiles
            or type(metric) not in (int, float)
            or not math.isfinite(metric)
            or metric <= 0
            or row["verified"] is not True
            or not isinstance(row["hash"], str)
            or not row["hash"].isdigit()
        ):
            raise GateError("Invalid, duplicate, unverified or unexpected benchmark profile")
        profiles[key] = {"medianUs": metric, "hash": row["hash"]}
    if profiles.keys() != PROFILES:
        raise GateError("Missing benchmark profiles")
    return profiles


def compare(measurements):
    result = []
    for key in sorted(PROFILES):
        before = [pair["baseline"][key]["medianUs"] for pair in measurements]
        after = [pair["candidate"][key]["medianUs"] for pair in measurements]
        hashes = {
            pair[variant][key]["hash"]
            for pair in measurements
            for variant in ("baseline", "candidate")
        }
        if len(hashes) != 1:
            raise GateError(f"Audio output changed across runs/trees: {key}")
        baseline, candidate = statistics.median(before), statistics.median(after)
        slower_pairs = sum(
            b - a > max(RELATIVE_LIMIT * a, ABSOLUTE_LIMIT_US) for a, b in zip(before, after)
        )
        regression = (
            candidate - baseline > max(RELATIVE_LIMIT * baseline, ABSOLUTE_LIMIT_US)
            and slower_pairs >= 4
        )
        result.append(
            {
                "channels": key[0],
                "frames": key[1],
                "limiter": key[2],
                "baselineUs": baseline,
                "candidateUs": candidate,
                "deltaPercent": (candidate / baseline - 1) * 100,
                "baselineRunsUs": before,
                "candidateRunsUs": after,
                "slowerPairs": slower_pairs,
                "regression": regression,
            }
        )
    return result


def execute(root, baseline, candidate, force):
    env = clean_git_environment(root)
    baseline_tree = git(root, "rev-parse", "--verify", f"{baseline}^{{tree}}").decode().strip()
    candidate_tree = (
        (
            git(root, "write-tree")
            if candidate is None
            else git(root, "rev-parse", "--verify", f"{candidate}^{{tree}}")
        )
        .decode()
        .strip()
    )
    paths = (
        git(root, "diff", "--name-only", "-z", baseline_tree, candidate_tree).decode().split("\0")
    )
    if not force and not relevant(paths):
        print(
            "Performance gate: no audio-core/build/dependency changes in this comparison; skipped."
        )
        return 0
    artifacts = root / "artifacts/performance-gate"
    artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(
        tempfile.mkdtemp(
            prefix=datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S-"), dir=artifacts
        )
    ).resolve()
    print(f"Performance gate: isolated Release builds; evidence: {work}", flush=True)
    report = {
        "baselineTree": baseline_tree,
        "candidateTree": candidate_tree,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version,
        "pairs": PAIRS,
        "discardedWarmupPairs": 1,
        "buildConfiguration": "Release",
        "relativeLimit": RELATIVE_LIMIT,
        "absoluteLimitUs": ABSOLUTE_LIMIT_US,
        "audioDevicesOpened": False,
        "passed": False,
    }
    try:
        harness = work / "routing_benchmark.cpp"
        harness.write_bytes(git(root, "show", f"{candidate_tree}:{HARNESS}"))
        report["workloadSHA256"] = digest(harness)
        binaries = {}
        for variant, tree in (("baseline", baseline_tree), ("candidate", candidate_tree)):
            source = work / f"{variant}-source"
            snapshot(root, tree, source, env)
            binaries[variant] = build(source, work / variant, harness, env)
        report["executables"] = {
            name: {"path": str(path), "sha256": digest(path)} for name, path in binaries.items()
        }
        pairs = []
        for pair_index in range(PAIRS + 1):
            pair = {}
            order = ("baseline", "candidate") if pair_index % 2 == 0 else ("candidate", "baseline")
            for variant in order:
                raw = run([str(binaries[variant])], env=env, timeout=120)
                (work / f"{variant}-{pair_index}.jsonl").write_bytes(raw)
                pair[variant] = parse_measurement(raw)
            # Check warm-up outputs as well, without counting their timings.
            for key in PROFILES:
                if pair["baseline"][key]["hash"] != pair["candidate"][key]["hash"]:
                    raise GateError(f"Baseline/candidate audio mismatch: {key}")
            if pair_index:
                pairs.append(pair)
        report["profiles"] = compare(pairs)
        report["passed"] = not any(row["regression"] for row in report["profiles"])
        for row in report["profiles"]:
            if row["regression"]:
                print(
                    f"REGRESSION: {row['channels']} ch / {row['frames']} frames / "
                    f"limiter={row['limiter']}: {row['baselineUs']:.3f} -> "
                    f"{row['candidateUs']:.3f} us ({row['deltaPercent']:+.1f}%)"
                )
    except (
        GateError,
        OSError,
        subprocess.TimeoutExpired,
        ValueError,
        KeyError,
        TypeError,
    ) as error:
        report["error"] = str(error)
        print(f"Performance gate failed: {error}", file=sys.stderr)
    finally:
        (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Performance gate: {'PASS' if report['passed'] else 'FAIL'}; {work / 'report.json'}")
    return 0 if report["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--baseline", default="HEAD", help="Git revision/tree; default: HEAD")
    parser.add_argument("--candidate", help="Git revision/tree; default: exact current Git index")
    parser.add_argument("--force", action="store_true", help="Run even for non-audio changes")
    args = parser.parse_args()
    root = args.root.resolve()
    actual = Path(git(root, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if actual != root:
        raise GateError("--root must be the repository's top-level directory")
    return execute(root, args.baseline, args.candidate, args.force)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (GateError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Performance gate failed: {error}", file=sys.stderr)
        sys.exit(1)
