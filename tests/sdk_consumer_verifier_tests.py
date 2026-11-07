"""External source reuse must retain all gitlink provenance checks."""

import subprocess
import sys
import unittest
from pathlib import Path

from zcore_submodule_tests import create_fixture, git, FIXTURE_ENV, VERIFIER


class ExternalSourceTests(unittest.TestCase):
    def invoke(self, parent, external):
        return subprocess.run(
            [
                sys.executable,
                str(VERIFIER),
                "--root",
                str(parent),
                "--path",
                "vendor/zcore",
                "--external-source",
                str(external),
                "--print-revision",
            ],
            capture_output=True,
            text=True,
            env=FIXTURE_ENV,
        )

    def test_external_pin_without_initialized_bundled_checkout(self):
        parent, child, first, _ = create_fixture(self)
        external = parent.parent / "external"
        git(parent, "clone", "--quiet", str(child), str(external))
        git(parent, "submodule", "deinit", "-f", "--", "vendor/zcore")
        result = self.invoke(parent, external)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), first)

    def test_external_wrong_revision_and_dirty_source(self):
        parent, child, first, second = create_fixture(self)
        external = parent.parent / "external"
        git(parent, "clone", "--quiet", str(child), str(external))
        git(external, "checkout", "--detach", second)
        result = self.invoke(parent, external)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match parent gitlink", result.stderr)
        git(external, "checkout", "--detach", first)
        (external / "untracked.txt").write_text("dirty")
        result = self.invoke(parent, external)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is dirty", result.stderr)


if __name__ == "__main__":
    unittest.main()
