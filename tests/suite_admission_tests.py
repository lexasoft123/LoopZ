"""Coordinated update admission: read-only GUI review and protected preservation."""

import copy
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from suite_admission import SuiteAdmission, document_uids, user_documents
from uninstall_policy import require_idle
from uninstall_session_tests import Launchd, identity


class Host(SuiteAdmission):
    def __init__(self):
        self.uid = 501
        self.session_admission = None
        self.recovering_sessions = False
        self.document_users = []
        self.console = 501
        self.launchd = Launchd()
        self.login_for_uid = Mock(return_value={"registered": False, "requiresApproval": False})
        self.catalog = {
            "defaultInput": "physical-input",
            "defaultOutput": "physical-output",
            "defaultSystemOutput": "system-output",
            "routingDevices": [],
            "routingBlockingRoutes": [],
            "loopzBlockingRoutes": [],
            "loopzVisible": False,
            "loopzRunning": False,
        }
        self.core = "modeled-core-audio-generation"
        self.protected = {"pin": "protected-pin", "documents": {}}

    def snapshot(self):
        return copy.deepcopy(self.catalog)

    def require_idle(self, catalog):
        require_idle(catalog)

    def service_identity(self):
        return self.core

    def preserved(self, users):
        return {
            "components": {"pin": self.protected["pin"]},
            "documents": {
                str(uid): self.protected["documents"].get(uid, "unchanged") for uid in users
            },
        }


class AdmissionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="loopz-suite-admission-", dir="/private/tmp")
        self.addCleanup(temporary.cleanup)
        self.desktop = Path(temporary.name) / "Desktop.app"
        self.desktop.mkdir()
        self.host = Host()
        real_stat = os.stat

        def observed_stat(path, *args, **kwargs):
            if path == "/dev/console":
                return SimpleNamespace(st_uid=self.host.console)
            return real_stat(path, *args, **kwargs)

        for context in (
            patch("suite_admission.os.stat", observed_stat),
            patch("suite_admission.command", self.host.launchd.command),
            patch("suite_admission.TARGETS", {"LoopZ.app": self.desktop}),
        ):
            context.start()
            self.addCleanup(context.stop)

    def test_all_gui_accounts_are_observed_without_service_controls(self):
        self.host.launchd.identities.append(identity(502, 200))
        baseline = self.host.capture()
        self.assertEqual(set(baseline["preserved"]["documents"]), {"501", "502"})
        self.host.admit(baseline)
        self.assertEqual(
            {call.args[0] for call in self.host.login_for_uid.call_args_list}, {501, 502}
        )
        self.assertTrue(
            all(call[:2] == ("/bin/launchctl", "print") for call in self.host.launchd.calls)
        )

    def test_audio_service_defaults_documents_and_protected_files_cannot_drift(self):
        for mutate in (
            lambda: self.host.catalog.update(defaultOutput="another-output"),
            lambda: setattr(self.host, "core", "replacement-core-audio"),
            lambda: self.host.protected.update(pin="foreign-pin"),
            lambda: self.host.protected["documents"].update({501: "modified-document"}),
            lambda: setattr(self.host, "console", 502),
        ):
            baseline = self.host.capture()
            saved = copy.deepcopy(
                (self.host.catalog, self.host.protected, self.host.core, self.host.console)
            )
            mutate()
            with self.assertRaises(RuntimeError):
                self.host.admit(baseline)
            self.host.catalog, self.host.protected, self.host.core, self.host.console = saved

    def test_running_graph_job_or_login_choice_blocks_admission(self):
        baseline = self.host.capture()
        for mutate, restore in (
            (
                lambda: self.host.catalog.update(routingDevices=[{"uid": "new-device"}]),
                lambda: self.host.catalog.update(routingDevices=[]),
            ),
            (
                lambda: self.host.launchd.jobs.add("gui/501/com.lexasoft.loopz.session"),
                lambda: self.host.launchd.jobs.clear(),
            ),
            (
                lambda: setattr(
                    self.host.login_for_uid,
                    "return_value",
                    {"registered": True, "requiresApproval": False},
                ),
                lambda: setattr(
                    self.host.login_for_uid,
                    "return_value",
                    {"registered": False, "requiresApproval": False},
                ),
            ),
        ):
            mutate()
            with self.assertRaises(RuntimeError):
                self.host.admit(baseline)
            restore()

    def test_recovery_preserves_fresh_hardware_and_documents_after_reboot(self):
        self.host.launchd.identities.append(identity(502, 200))
        original = self.host.capture()
        self.host.launchd.identities = [identity(501, 900)]
        self.host.catalog["defaultOutput"] = "current-hardware-choice"
        self.host.protected["documents"][502] = "edited-while-logged-out"
        self.host.core = "core-audio-after-reboot"
        self.desktop.rmdir()
        self.host.login_for_uid.reset_mock()
        current = self.host.recovery_baseline(original)
        self.assertFalse(current["sessions"]["desktopAvailable"])
        self.assertIsNone(current["sessions"]["accounts"][0]["login"])
        self.assertEqual(current["defaults"]["defaultOutput"], "current-hardware-choice")
        self.assertEqual(current["preserved"]["documents"]["502"], "edited-while-logged-out")
        self.host.admit(current)
        self.host.login_for_uid.assert_not_called()
        self.host.launchd.identities = [identity(501, 901)]
        with self.assertRaisesRegex(RuntimeError, "GUI sessions changed"):
            self.host.admit(current)

    def test_missing_desktop_cannot_supply_login_choices_for_new_accounts(self):
        original = self.host.capture()
        self.desktop.rmdir()
        self.host.launchd.identities.append(identity(502, 200))
        with self.assertRaisesRegex(RuntimeError, "No verified login choice"):
            self.host.recovery_baseline(original)

    def test_recovery_rejects_invalid_document_accounts_and_wrong_console(self):
        original = self.host.capture()
        for key in ("../../etc", "0", "0501", "4294967296"):
            value = copy.deepcopy(original)
            value["preserved"]["documents"] = {key: "untrusted"}
            with self.assertRaises(ValueError):
                self.host.recovery_baseline(value)
        self.host.uid = 502
        with self.assertRaisesRegex(RuntimeError, "original console"):
            self.host.recovery_baseline(original)


class DocumentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="loopz-suite-documents-", dir="/private/tmp")
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        self.uid = os.geteuid()
        context = patch(
            "suite_admission.pwd.getpwuid", return_value=SimpleNamespace(pw_dir=str(self.home))
        )
        context.start()
        self.addCleanup(context.stop)
        self.support = self.home / "Library/Application Support/LoopZ"

    def test_missing_documents_are_observed_without_creating_a_directory(self):
        result = user_documents(self.uid)
        self.assertFalse(result["directoryExists"])
        self.assertTrue(all(value is None for value in result["files"].values()))
        self.assertFalse(self.support.exists())

    def test_descriptor_inventory_detects_edits_and_rejects_links(self):
        self.support.mkdir(parents=True, mode=0o700)
        document = self.support / "routes.json"
        document.write_text("original")
        document.chmod(0o600)
        before = user_documents(self.uid)
        document.write_text("changed")
        self.assertNotEqual(before, user_documents(self.uid))
        document.unlink()
        document.symlink_to(self.home / "outside.json")
        with self.assertRaises((ValueError, RuntimeError, OSError)):
            user_documents(self.uid)

    def test_bounded_account_ids_reject_root_and_nonintegers(self):
        self.assertEqual(document_uids([502, 501, 501]), [501, 502])
        for values in ([0], [True], ["501"], [2**32], range(1, 514)):
            with self.assertRaises(ValueError):
                document_uids(values)


if __name__ == "__main__":
    unittest.main()
