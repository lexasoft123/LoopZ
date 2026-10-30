"""The performance gate must reject regressions and invalid evidence."""

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import performance_gate as gate


def output(metric=10):
    return "\n".join(
        json.dumps(
            {
                "channels": channels,
                "frames": frames,
                "limiter": limiter,
                "verified": True,
                "medianUs": metric,
                "hash": "1234",
            }
        )
        for channels, frames, limiter in sorted(gate.PROFILES)
    ).encode()


def pairs(before, after):
    return [
        {
            "baseline": gate.parse_measurement(output(a)),
            "candidate": gate.parse_measurement(output(b)),
        }
        for a, b in zip(before, after)
    ]


class EvidenceTests(unittest.TestCase):
    def test_reproducible_slowdown_blocks_even_with_one_baseline_outlier(self):
        rows = gate.compare(pairs([10, 10, 90, 10, 10], [15, 15, 15, 15, 15]))
        self.assertTrue(all(row["regression"] and row["slowerPairs"] == 4 for row in rows))

    def test_noise_and_sub_microsecond_changes_do_not_block(self):
        for before, after in (
            ([10] * 5, [10, 20, 10, 20, 10]),
            ([0.1] * 5, [0.2] * 5),
            ([10] * 5, [11] * 5),
        ):
            self.assertFalse(any(row["regression"] for row in gate.compare(pairs(before, after))))

    def test_audio_mismatch_blocks_even_when_candidate_is_faster(self):
        samples = pairs([10] * 5, [1] * 5)
        samples[0]["candidate"][(64, 256, True)]["hash"] = "555"
        with self.assertRaisesRegex(gate.GateError, "Audio output changed"):
            gate.compare(samples)

    def test_incomplete_duplicate_or_invalid_profiles_are_rejected(self):
        rows = [json.loads(line) for line in output().decode().splitlines()]
        invalid = [rows[:-1], rows + [rows[0]]]
        for field, value in (
            ("medianUs", 0),
            ("medianUs", float("nan")),
            ("medianUs", True),
            ("verified", False),
            ("hash", 1234),
            ("channels", 3),
            ("limiter", 1),
        ):
            sample = copy.deepcopy(rows)
            sample[0][field] = value
            invalid.append(sample)
        for sample in invalid:
            with self.subTest(sample=sample[0]):
                with self.assertRaises(gate.GateError):
                    gate.parse_measurement("\n".join(map(json.dumps, sample)).encode())

    def test_nonzero_native_exit_is_not_accepted(self):
        with self.assertRaises(gate.GateError):
            gate.run([sys.executable, "-c", "raise SystemExit(7)"])

    def test_only_audio_build_and_gate_changes_trigger_benchmark_builds(self):
        self.assertFalse(gate.relevant(["README.md", "interface/src/App.jsx"]))
        for path in (
            "native/core/routing.cpp",
            "vendor/zcore",
            "cmake/zcore/CMakeLists.txt",
            "CMakeLists.txt",
            gate.HARNESS,
            "scripts/performance_gate.py",
        ):
            self.assertTrue(gate.relevant([path]))


class SnapshotTests(unittest.TestCase):
    def test_staged_and_pinned_bytes_are_used_without_touching_original_index(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            root, child = base / "repo", base / "child"
            env = os.environ.copy()
            for name in list(env):
                if name.startswith("GIT_"):
                    env.pop(name)
            env.update(
                {
                    "GIT_AUTHOR_NAME": "Fixture",
                    "GIT_AUTHOR_EMAIL": "fixture@example.test",
                    "GIT_COMMITTER_NAME": "Fixture",
                    "GIT_COMMITTER_EMAIL": "fixture@example.test",
                }
            )

            def git(path, *args):
                return gate.git(path, *args, env=env).decode().strip()

            for path in (root, child):
                path.mkdir()
                git(path, "init")
                git(path, "config", "core.hooksPath", str(base / "no-hooks"))
            (child / "pinned.txt").write_text("pinned\n")
            git(child, "add", ".")
            git(child, "commit", "-m", "fixture")
            oid = git(child, "rev-parse", "HEAD")
            (root / "file.txt").write_text("committed\n")
            git(root, "add", ".")
            git(root, "commit", "-m", "fixture")
            (root / "file.txt").write_text("staged\n")
            git(root, "add", "file.txt")
            git(root, "update-index", "--add", "--cacheinfo", f"160000,{oid},vendor/core")
            (root / "vendor").mkdir()
            gate.run(["git", "clone", "--shared", str(child), str(root / "vendor/core")], env=env)
            (root / "file.txt").write_text("unstaged\n")
            (root / "vendor/core/pinned.txt").write_text("dirty child\n")
            tree = git(root, "write-tree")
            index_before = (root / ".git/index").read_bytes()
            # Simulate a caller supplying an alternate index.
            original = os.environ.get("GIT_INDEX_FILE")
            try:
                os.environ["GIT_INDEX_FILE"] = str(root / ".git/index")
                clean = gate.clean_git_environment(root)
                self.assertNotIn("GIT_INDEX_FILE", clean)
                gate.snapshot(root, tree, base / "snapshot", clean)
            finally:
                if original is None:
                    os.environ.pop("GIT_INDEX_FILE", None)
                else:
                    os.environ["GIT_INDEX_FILE"] = original
            self.assertEqual((base / "snapshot/file.txt").read_text(), "staged\n")
            self.assertEqual((base / "snapshot/vendor/core/pinned.txt").read_text(), "pinned\n")
            self.assertEqual(index_before, (root / ".git/index").read_bytes())
            self.assertEqual((root / "file.txt").read_text(), "unstaged\n")


if __name__ == "__main__":
    unittest.main()
