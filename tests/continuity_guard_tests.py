"""Console-session continuity guard; all filesystem targets are temporary."""

import json
import os
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from install_desktop import DesktopHost
from install_routing import ContinuityGuard


class GuardTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.owner = os.geteuid()
        self.host = DesktopHost.__new__(DesktopHost)
        self.host.uid = self.owner
        self.host.user = SimpleNamespace(pw_gid=os.getegid())
        self.host.checker = Path("/fixed/checker")
        self.host.guard_uid = None

    def host_guard(self):
        return self.host.start_guard(self.work, {"defaultOutput": "physical-device"})

    def test_user_report_and_admin_stop_paths_are_separate(self):
        with patch("install_routing.subprocess.Popen") as spawn:
            guard = self.host_guard()
        self.addCleanup(guard.stderr.close)
        args = spawn.call_args.args[0]
        self.assertEqual(args[:3], ["/fixed/checker", "--guard", "physical-device"])
        self.assertEqual(guard.report.parent, self.work / "hardware-guard")
        self.assertEqual(guard.stop_file.parent, self.work)
        self.assertEqual(guard.report.parent.stat().st_mode & 0o777, 0o700)

    def test_root_launches_only_the_fixed_checker_as_the_console_user(self):
        with (
            patch("install_desktop.os.geteuid", return_value=0),
            patch("install_desktop.os.chown") as chown,
            patch("install_routing.subprocess.Popen") as spawn,
        ):
            guard = self.host_guard()
        self.addCleanup(guard.stderr.close)
        self.assertEqual(
            spawn.call_args.args[0][:11],
            [
                "/bin/launchctl",
                "asuser",
                str(self.owner),
                "/usr/bin/sudo",
                "-n",
                "-H",
                "-u",
                f"#{self.owner}",
                "/fixed/checker",
                "--guard",
                "physical-device",
            ],
        )
        chown.assert_called_once_with(self.work / "hardware-guard", self.owner, os.getegid())
        self.assertEqual(guard.report_owner, self.owner)

    def test_another_user_cannot_start_the_check(self):
        self.host.uid += 1
        with patch("install_routing.subprocess.Popen") as spawn:
            with self.assertRaisesRegex(RuntimeError, "console user"):
                self.host_guard()
        spawn.assert_not_called()

    def make_guard(self):
        with patch("install_routing.subprocess.Popen") as spawn:
            guard = ContinuityGuard(Path("/fixed/checker"), self.work, "physical-device")
        self.addCleanup(guard.stderr.close)
        spawn.return_value.poll.return_value = None
        spawn.return_value.returncode = 0
        return guard

    def write_report(self, guard, **changes):
        value = dict(
            uid="physical-device",
            state="stopped",
            passed=True,
            stoppedByRequest=True,
            frames=48000,
            timestampErrors=0,
        )
        value.update(changes)
        guard.report.write_text(json.dumps(value))
        guard.report.chmod(0o600)

    def test_stop_requires_actual_success_and_matching_device(self):
        guard = self.make_guard()
        for changes in (
            {"uid": "other-device"},
            {"passed": "yes"},
            {"frames": True},
            {"timestampErrors": 1},
            {"stoppedByRequest": False},
            {"state": "running"},
            {"frames": 0},
        ):
            with self.subTest(changes=changes):
                self.write_report(guard, **changes)
                with self.assertRaises(RuntimeError):
                    guard.stop()
        self.write_report(guard)
        self.assertTrue(guard.stop()["passed"])
        self.assertTrue(guard.stop_file.is_file())

    def test_report_rejects_links_unsafe_owner_mode_and_size(self):
        guard = self.make_guard()
        other = self.work / "other.json"
        other.write_text('{"uid":"physical-device"}')
        guard.report.symlink_to(other)
        with self.assertRaises(OSError):
            guard.read_report()
        guard.report.unlink()
        os.link(other, guard.report)
        with self.assertRaises(RuntimeError):
            guard.read_report()
        guard.report.unlink()
        self.write_report(guard)
        guard.report_owner = self.owner + 1
        with self.assertRaises(RuntimeError):
            guard.read_report()
        guard.report_owner = self.owner
        guard.report.chmod(0o666)
        with self.assertRaises(RuntimeError):
            guard.read_report()
        guard.report.chmod(0o600)
        guard.report.write_text(" " * 65537)
        with self.assertRaises(RuntimeError):
            guard.read_report()


if __name__ == "__main__":
    unittest.main()
