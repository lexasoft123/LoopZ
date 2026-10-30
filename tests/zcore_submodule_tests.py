"""Exercise zCore submodule verification with disposable Git fixtures."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]
VERIFIER = ROOT / "scripts/verify-zcore-submodule.py"
SUBMODULE_PATH = "vendor/zcore"
FIXTURE_ENV = {
    **os.environ,
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_CONFIG_GLOBAL": "NUL" if os.name == "nt" else os.devnull,
    "GIT_AUTHOR_NAME": "Fixture",
    "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
    "GIT_COMMITTER_NAME": "Fixture",
    "GIT_COMMITTER_EMAIL": "fixture@example.invalid",
    "GIT_TERMINAL_PROMPT": "0",
    "GIT_OPTIONAL_LOCKS": "0",
}


def git(repo: Path, *args: str, input: Optional[str] = None) -> str:
    command = ["git", "-c", "protocol.file.allow=always", "-C", str(repo), *args]
    result = subprocess.run(
        command, input=input, capture_output=True, text=True, env=FIXTURE_ENV, check=True
    )
    return result.stdout.strip()


def create_fixture(test: unittest.TestCase) -> tuple[Path, Path, str, str]:
    temporary = tempfile.TemporaryDirectory(prefix="loopz-zcore-submodule-")
    test.addCleanup(temporary.cleanup)
    scratch = Path(temporary.name)
    parent = scratch / "consumer"
    source = scratch / "zcore-origin"
    parent.mkdir()
    source.mkdir()
    (source / "CMakeLists.txt").write_text("# fixture zCore\n")
    git(source, "init", "-q")
    git(source, "add", ".")
    git(source, "commit", "-qm", "initial zCore source")
    first_revision = git(source, "rev-parse", "HEAD")
    git(source, "commit", "--allow-empty", "-qm", "same tree, new pin")
    second_revision = git(source, "rev-parse", "HEAD")

    git(parent, "init", "-q")
    git(parent, "submodule", "add", str(source), SUBMODULE_PATH)
    child = parent / SUBMODULE_PATH
    git(child, "checkout", "--detach", first_revision)
    git(parent, "add", SUBMODULE_PATH)
    git(parent, "add", "-A")
    git(parent, "commit", "-qm", "consumer fixture")
    return parent, child, first_revision, second_revision


def invoke_verifier(
    root: Path, *, print_revision: bool = False
) -> subprocess.CompletedProcess[str]:
    python = "python" if os.name == "nt" else sys.executable
    command = [python, str(VERIFIER), "--root", str(root), "--path", SUBMODULE_PATH]
    if print_revision:
        command.append("--print-revision")
    return subprocess.run(command, capture_output=True, text=True, env=FIXTURE_ENV)


class ZCoreSubmoduleTests(unittest.TestCase):
    def assert_rejected(self, parent: Path, expected: str) -> None:
        result = invoke_verifier(parent)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(expected, result.stderr)

    def test_reports_exact_parent_gitlink_revision(self):
        parent, _, first_revision, _ = create_fixture(self)
        result = invoke_verifier(parent, print_revision=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"{first_revision}\n")
        self.assertEqual(result.stderr, "")

    def test_rejects_uninitialized_gitlink_with_init_command(self):
        parent, child, _, _ = create_fixture(self)
        shutil.rmtree(child)
        self.assert_rejected(parent, "git submodule update --init -- vendor/zcore")

    def test_rejects_wrong_child_revision_even_when_clean(self):
        parent, child, _, second_revision = create_fixture(self)
        git(child, "checkout", "--detach", second_revision)
        self.assert_rejected(parent, "does not match parent gitlink")

    def test_rejects_unstaged_staged_and_untracked_child_changes(self):
        for kind in ("unstaged", "staged", "untracked"):
            with self.subTest(kind=kind):
                parent, child, _, _ = create_fixture(self)
                changed = child / "edited.txt"
                changed.write_text("changed\n")
                if kind == "staged":
                    git(child, "add", "edited.txt")
                elif kind == "unstaged":
                    (child / "CMakeLists.txt").write_text("# dirty\n")
                self.assert_rejected(parent, "is dirty")

    def test_rejects_conflicted_parent_gitlink_index(self):
        parent, _, first_revision, _ = create_fixture(self)
        git(parent, "update-index", "--force-remove", SUBMODULE_PATH)
        stages = "".join(
            f"160000 {first_revision} {stage}\t{SUBMODULE_PATH}\0" for stage in (1, 2, 3)
        )
        git(parent, "update-index", "-z", "--index-info", input=stages)
        self.assert_rejected(parent, "exactly one stage-0 mode-160000 gitlink entry")

    def test_rejects_copied_plain_folder(self):
        parent, child, _, _ = create_fixture(self)
        (child / ".git").unlink()
        self.assert_rejected(parent, "different repository")


if __name__ == "__main__":
    unittest.main()
