"""Private/mock multi-user removal tests; never run another account's app."""

import copy
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from uninstall_sessions import (
    LABELS,
    MAX_OUTPUT,
    SessionInspector,
    require_sessions_off,
    validate_session_preservation,
    validate_sessions,
)
import uninstall_host
from uninstall_tests import RemovalFixture, Host
from uninstall_policy import digest, preflight, remove_product
from uninstall_files import RemovalFiles


def response(text="", status=0, error=""):
    return SimpleNamespace(stdout=text, stderr=error, returncode=status)


def identity(uid=501, asid=100):
    return {"uid": uid, "asid": asid, "handle": asid}


def account(uid=501, asid=100):
    return {
        **identity(uid, asid),
        "jobs": {label: False for label in LABELS},
        "login": {"registered": False, "requiresApproval": False},
    }


def state(*accounts, available=True):
    return {
        "version": 1,
        "coverage": "activeGuiSessions",
        "desktopAvailable": available,
        "accounts": list(accounts or [account()]),
    }


class Launchd:
    def __init__(self):
        self.identities = [identity()]
        self.jobs = set()
        self.calls = []
        self.overrides = {}

    def command(self, *args, **kwargs):
        self.calls.append(args)
        self.assert_read_only(args)
        domain = args[-1]
        if domain in self.overrides:
            return self.overrides[domain]
        if domain == "system":
            # A network user is discoverable here without appearing in passwd.
            users = [0, 202] + [a["uid"] for a in self.identities]
            children = "\n".join(f"\t\tuser/{uid}" for uid in users)
            return response(
                f"system = {{\n\ttype = system\n\thandle = 0\n\tsubdomains = {{\n{children}\n\t\tp id/1\n\t}}\n}}\n".replace(
                    "p id", "pid"
                )
            )
        if domain.count("/") == 1:
            uid = int(domain.split("/")[1])
            match = next((a for a in self.identities if a["uid"] == uid), None)
            if match is None:
                return response(
                    status=125,
                    error="Could not print domain: 125: Domain does not support specified action\n",
                )
            return response(
                f"{domain} = {{\n\ttype = login\n\thandle = {match['handle']}\n\tsecurity context = {{\n\t\tuid = {uid}\n\t\tasid = {match['asid']}\n\t}}\n}}\n"
            )
        return (
            response("loaded but not running")
            if domain in self.jobs
            else response(status=113, error="Could not find service\n")
        )

    @staticmethod
    def assert_read_only(args):
        if args[:2] != ("/bin/launchctl", "print"):
            raise AssertionError("Only launchd print is authorized")


class SessionDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.launchd = Launchd()
        self.login = Mock(return_value={"registered": False, "requiresApproval": False})
        self.inspector = SessionInspector(self.launchd.command, self.login)

    def test_two_users_including_network_account_are_inspected_twice(self):
        self.launchd.identities.append(identity(123456, 200))
        result = self.inspector.snapshot(501, True)
        self.assertEqual([a["uid"] for a in result["accounts"]], [501, 123456])
        self.assertEqual(self.login.call_count, 2)
        for uid in (501, 123456):
            self.assertEqual(self.launchd.calls.count(("/bin/launchctl", "print", f"gui/{uid}")), 2)
        require_sessions_off(result)

    def test_no_account_database_enumeration(self):
        with patch("pwd.getpwall", side_effect=AssertionError("No passwd enumeration")):
            self.inspector.snapshot(501, True)

    def test_permissions_error_is_not_an_absent_session(self):
        self.launchd.overrides["gui/202"] = response(status=1, error="Operation not permitted")
        with self.assertRaisesRegex(RuntimeError, "Cannot inspect"):
            self.inspector.snapshot(501, True)
        self.login.assert_not_called()

    def test_unknown_error_is_not_an_absent_session(self):
        self.launchd.overrides["gui/202"] = response(status=125, error="Unexpected")
        with self.assertRaises(RuntimeError):
            self.inspector.discover()

    def test_vanished_session_requires_retry(self):
        self.launchd.overrides["gui/501"] = response(status=113, error="Could not find domain")
        with self.assertRaises(RuntimeError):
            self.inspector.discover()

    def test_changed_login_generation_is_detected(self):
        def login(_):
            self.launchd.identities[0]["asid"] += 1
            return {"registered": False, "requiresApproval": False}

        self.inspector.login_for_uid = login
        with self.assertRaisesRegex(RuntimeError, "sessions changed"):
            self.inspector.snapshot(501, True)

    def test_new_user_during_scan_is_detected(self):
        self.inspector.login_for_uid = lambda _: (
            self.launchd.identities.append(identity(502, 200))
            or {"registered": False, "requiresApproval": False}
        )
        with self.assertRaisesRegex(RuntimeError, "sessions changed"):
            self.inspector.snapshot(501, True)

    def test_loaded_job_blocks_without_a_pid(self):
        self.launchd.jobs.add("gui/501/com.lexasoft.loopz.login")
        result = self.inspector.snapshot(501, True)
        with self.assertRaisesRegex(RuntimeError, "Stop LoopZ jobs"):
            require_sessions_off(result)

    def test_unknown_job_error_fails(self):
        self.launchd.overrides["gui/501/com.lexasoft.loopz.session"] = response(
            status=1, error="Denied"
        )
        with self.assertRaisesRegex(RuntimeError, "jobs could not"):
            self.inspector.snapshot(501, True)

    def test_unknown_domain_format_fails(self):
        for text in (
            "system = {\n}\n",
            "system = {\n\ttype = system\n\thandle = 0\n\tsubdomains = {\n\t\tforeign/501\n\t}\n}\n",
            "x" * (MAX_OUTPUT + 1),
        ):
            self.launchd.overrides["system"] = response(text)
            with self.assertRaises(RuntimeError):
                self.inspector.discover()

    def test_uid_mismatch_or_duplicate_security_context_fails(self):
        original = self.launchd.command("/bin/launchctl", "print", "gui/501").stdout
        for text in (
            original.replace("uid = 501", "uid = 502"),
            original.replace("\t\tasid = 100", "\t\tasid = 100\n\t\tasid = 101"),
        ):
            self.launchd.overrides["gui/501"] = response(text)
            with self.assertRaises(RuntimeError):
                self.inspector.discover()

    def test_missing_console_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Console user"):
            self.inspector.snapshot(502, True)

    def test_missing_desktop_never_runs_status(self):
        result = self.inspector.snapshot(501, False)
        self.login.assert_not_called()
        self.assertIsNone(result["accounts"][0]["login"])


class SessionPolicyTests(unittest.TestCase):
    def test_registered_other_user_blocks(self):
        other = account(502, 200)
        other["login"]["registered"] = True
        with self.assertRaisesRegex(RuntimeError, "502"):
            require_sessions_off(state(account(), other))

    def test_pending_approval_blocks(self):
        value = state()
        value["accounts"][0]["login"]["requiresApproval"] = True
        with self.assertRaises(RuntimeError):
            require_sessions_off(value)

    def test_unknown_login_cannot_pass_as_false(self):
        value = state()
        for login in (None, {}, {"registered": 0, "requiresApproval": False}):
            value["accounts"][0]["login"] = login
            with self.assertRaises(ValueError):
                require_sessions_off(value)

    def test_duplicate_uid_invalid(self):
        with self.assertRaises(ValueError):
            validate_sessions(state(account(), account()))

    def test_quarantined_app_uses_only_same_generation_grant(self):
        old = state()
        current = copy.deepcopy(old)
        current["desktopAvailable"] = False
        current["accounts"][0]["login"] = None
        require_sessions_off(current, old)
        self.assertIsNone(current["accounts"][0]["login"])
        current["accounts"][0]["asid"] += 1
        with self.assertRaises(RuntimeError):
            require_sessions_off(current, old)
        require_sessions_off(current, old, allow_new_generation=True)

    def test_new_user_cannot_borrow_another_grant_even_in_recovery(self):
        other = account(502, 200)
        other["login"] = None
        with self.assertRaisesRegex(RuntimeError, "502"):
            require_sessions_off(state(other, available=False), state(), allow_new_generation=True)

    def test_recorded_registration_never_becomes_off(self):
        old = state()
        old["accounts"][0]["login"]["registered"] = True
        current = state(available=False)
        current["accounts"][0]["login"] = None
        with self.assertRaises(RuntimeError):
            require_sessions_off(current, old)

    def test_session_fence_accepts_missing_app_but_rejects_new_generation(self):
        old = state()
        current = state(available=False)
        current["accounts"][0]["login"] = None
        validate_session_preservation(old, current)
        current["accounts"][0]["handle"] += 1
        with self.assertRaisesRegex(RuntimeError, "sessions changed"):
            validate_session_preservation(old, current)

    def test_new_job_or_login_after_review_blocks(self):
        old = state()
        for field in ("jobs", "login"):
            current = state()
            current["accounts"][0][field][LABELS[0] if field == "jobs" else "registered"] = True
            with self.assertRaises(RuntimeError):
                validate_session_preservation(old, current)


class NativeUserContextTests(unittest.TestCase):
    def setUp(self):
        self.host = uninstall_host.UninstallHost.__new__(uninstall_host.UninstallHost)
        self.host.uid = 501

    def test_non_root_cannot_launch_status_in_another_account(self):
        with (
            patch.object(uninstall_host.os, "geteuid", return_value=501),
            patch.object(uninstall_host, "command") as command,
        ):
            with self.assertRaisesRegex(RuntimeError, "Administrator preflight"):
                self.host.login_for_uid(502)
            command.assert_not_called()

    def test_administrator_queries_fixed_status_entry_in_correct_context(self):
        with (
            patch.object(uninstall_host.os, "geteuid", return_value=0),
            patch.object(uninstall_host, "verify_code") as verify,
            patch.object(
                uninstall_host,
                "command",
                return_value=response(json.dumps({"version": 1, "login": account()["login"]})),
            ) as command,
        ):
            self.host.login_for_uid(123456)
            verify.assert_called_once()
            self.assertEqual(
                command.call_args.args,
                (
                    "/bin/launchctl",
                    "asuser",
                    123456,
                    "/usr/bin/sudo",
                    "-n",
                    "-H",
                    "-u",
                    "#123456",
                    uninstall_host.TARGETS["desktop"] / "Contents/MacOS/LoopZ",
                    "--background-status",
                ),
            )

    def test_unverified_desktop_never_runs_in_other_context(self):
        with (
            patch.object(uninstall_host.os, "geteuid", return_value=0),
            patch.object(uninstall_host, "verify_code", side_effect=RuntimeError("bad signature")),
            patch.object(uninstall_host, "command") as command,
        ):
            with self.assertRaises(RuntimeError):
                self.host.login_for_uid(502)
            command.assert_not_called()


class NativeSessionAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.host = uninstall_host.UninstallHost.__new__(uninstall_host.UninstallHost)
        self.host.uid = 501
        self.host.session_admission = state()
        self.host.recovering_sessions = False
        self.host.require_idle = Mock()
        self.check = patch.object(uninstall_host, "require_idle").start()
        self.addCleanup(patch.stopall)
        patch.object(uninstall_host, "command", return_value=response(status=1)).start()

    @staticmethod
    def unavailable(asid=100):
        item = account(501, asid)
        item["login"] = None
        return {"catalog": {}, "login": None, "sessions": state(item, available=False)}

    def test_same_attempt_grant_checks_missing_app_without_fabricating_status(self):
        current = self.unavailable()
        self.host.require_stopped(current)
        self.assertIsNone(current["login"])
        self.assertIsNone(current["sessions"]["accounts"][0]["login"])

    def test_other_generation_requires_explicit_recovery_mode(self):
        with self.assertRaises(RuntimeError):
            self.host.require_stopped(self.unavailable(200))
        self.host.recovering_sessions = True
        self.host.require_stopped(self.unavailable(200))

    def test_legacy_journal_cannot_supply_other_users_grants(self):
        self.host.session_admission = None
        current = self.unavailable()
        other = account(502, 200)
        other["login"] = None
        current["sessions"]["accounts"].append(other)
        with self.assertRaisesRegex(RuntimeError, "Legacy recovery"):
            self.host.require_recovery_stopped(
                current, {"consoleUID": 501, "login": account()["login"]}
            )

    def test_legacy_journal_is_unnecessary_when_fresh_status_is_available(self):
        self.host.session_admission = None
        current = {
            "catalog": {},
            "login": account()["login"],
            "sessions": state(account(), account(502, 200)),
        }
        self.host.require_recovery_stopped(
            current, {"consoleUID": 501, "login": account()["login"]}
        )
        self.assertEqual(len(self.host.session_admission["accounts"]), 2)

    def test_console_switch_before_scan_rejects_without_status_query(self):
        with (
            patch.object(uninstall_host.os, "stat", return_value=SimpleNamespace(st_uid=502)),
            patch.object(uninstall_host, "SessionInspector") as inspector,
        ):
            with self.assertRaisesRegex(RuntimeError, "Console account changed"):
                self.host.state()
            inspector.assert_not_called()

    def test_console_switch_during_scan_rejects_before_baseline_reads(self):
        desktop = Mock()
        desktop.exists.return_value = True
        self.host.snapshot = Mock()
        with (
            patch.object(
                uninstall_host.os,
                "stat",
                side_effect=[SimpleNamespace(st_uid=501), SimpleNamespace(st_uid=502)],
            ),
            patch.dict(uninstall_host.TARGETS, desktop=desktop),
            patch.object(uninstall_host, "SessionInspector") as inspector,
        ):
            inspector.return_value.snapshot.return_value = state()
            with self.assertRaisesRegex(RuntimeError, "Console account changed"):
                self.host.state()
            self.host.snapshot.assert_not_called()


class RemovalSessionRaceTests(RemovalFixture, unittest.TestCase):
    def test_user_switch_after_quarantine_rolls_back_before_driver_signal(self):
        document = self.root / "routes.json"
        document.write_text("user routing document")
        host = Host(document)
        original_state = host.state
        original_apply = self.transaction.apply
        changed = False

        def snapshot():
            result = original_state()
            result["sessions"] = state(account(501, 101 if changed else 100))
            return result

        def apply():
            nonlocal changed
            original_apply()
            changed = True

        host.state = snapshot
        with (
            patch.object(self.transaction, "apply", apply),
            patch.object(host.transition, "deactivate") as deactivate,
        ):
            with RemovalFiles(self.targets) as review:
                plan = preflight(review, host)
            result = remove_product(self.transaction, host, self.root, digest(plan))
            self.assertEqual(result["status"], "failed")
            self.assertIn("sessions changed", result["error"])
            deactivate.assert_not_called()
            self.assert_originals()


if __name__ == "__main__":
    unittest.main()
