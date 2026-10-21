#!/usr/bin/env python3
"""Signing isolation and cleanup without accessing real private keys."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import stage_signed


class SigningTests(unittest.TestCase):
    def fake_security(self, *arguments):
        self.operations.append(arguments)
        if arguments[0] == "create-keychain":
            self.keychain = Path(arguments[-1])
            self.keychain.touch()
        if arguments[0] == "delete-keychain":
            Path(arguments[-1]).unlink()
        if arguments[0] == "find-identity":
            return f'  1) {"A" * 40} "{self.identity}"\n  1 valid identities found\n'
        if arguments == ("list-keychains", "-d", "user"):
            return '"/original/login keychain-db"\n"/original/signing.keychain-db"\n'
        return ""

    def setUp(self):
        self.operations = []
        self.identity = "Developer ID Application: Test (TESTTEAM)"
        self.keychain = None

    def test_child_failure_removes_keychain_and_never_inherits_password(self):
        with (
            patch.object(stage_signed, "security", side_effect=self.fake_security),
            patch.dict(os.environ, {"LOOPZ_MAC_P12_PASSWORD": "export-password"}),
        ):
            with self.assertRaisesRegex(RuntimeError, "child failed"):
                with stage_signed.signing_environment(Path("test.p12"), "export-password") as env:
                    self.assertEqual(env["LOOPZ_ENGINE_SIGNING_IDENTITY"], "A" * 40)
                    self.assertNotIn("LOOPZ_MAC_P12_PASSWORD", env)
                    self.assertNotIn("SINGZ_MAC_P12_PASSWORD", env)
                    raise RuntimeError("child failed")
        self.assertFalse(self.keychain.exists())
        created_password = next(x[2] for x in self.operations if x[0] == "create-keychain")
        partition = next(x for x in self.operations if x[0] == "set-key-partition-list")
        self.assertEqual(partition[partition.index("-k") + 1], created_password)
        self.assertNotEqual(created_password, "export-password")
        search_list = next(
            x for x in self.operations if x[:4] == ("list-keychains", "-d", "user", "-s")
        )
        self.assertEqual(
            search_list[4:],
            (str(self.keychain), "/original/login keychain-db", "/original/signing.keychain-db"),
        )

    def test_wrong_certificate_class_cannot_reach_staging(self):
        self.identity = "Apple Distribution: Test (TESTTEAM)"
        with patch.object(stage_signed, "security", side_effect=self.fake_security):
            with self.assertRaisesRegex(stage_signed.SigningError, "Developer ID Application"):
                with stage_signed.signing_environment(Path("test.p12"), "export-password"):
                    self.fail("Wrong certificate reached staging")
        self.assertFalse(self.keychain.exists())

    def test_import_failure_still_removes_created_keychain(self):
        def import_failure(*arguments):
            if arguments[0] == "import":
                raise stage_signed.SigningError("import failed")
            return self.fake_security(*arguments)

        with patch.object(stage_signed, "security", side_effect=import_failure):
            with self.assertRaisesRegex(stage_signed.SigningError, "import failed"):
                with stage_signed.signing_environment(Path("test.p12"), "export-password"):
                    self.fail("Failed import reached staging")
        self.assertFalse(self.keychain.exists())

    def test_security_failure_does_not_disclose_password_or_tool_output(self):
        result = subprocess.CompletedProcess([], 1, "private output", "export-password")
        with patch.object(stage_signed.subprocess, "run", return_value=result):
            with self.assertRaises(stage_signed.SigningError) as caught:
                stage_signed.security("import", "test.p12", "-P", "export-password")
        self.assertNotIn("export-password", str(caught.exception))
        self.assertNotIn("private output", str(caught.exception))

    def test_encrypted_store_failure_does_not_disclose_plaintext(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Path(directory) / "secrets.enc.yaml"
            store.touch()
            result = subprocess.CompletedProcess([], 0, '{"private":"export-password"}', "")
            with (
                patch.dict(os.environ, {}, clear=True),
                patch.object(stage_signed.subprocess, "run", return_value=result),
            ):
                with self.assertRaises(stage_signed.SigningError) as caught:
                    stage_signed.certificate_password(store)
            self.assertNotIn("export-password", str(caught.exception))

    def test_password_json_preserves_special_characters_without_exporting_other_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Path(directory) / "secrets.enc.yaml"
            store.touch()
            password = 'quotes" and \\ and a newline\n'
            result = subprocess.CompletedProcess(
                [], 0, json.dumps({"mac_p12_password": password, "other_key": "unused"}), ""
            )
            with (
                patch.dict(os.environ, {}, clear=True),
                patch.object(stage_signed.subprocess, "run", return_value=result),
            ):
                self.assertEqual(stage_signed.certificate_password(store), password)
            self.assertNotIn("other_key", os.environ)


if __name__ == "__main__":
    unittest.main()
