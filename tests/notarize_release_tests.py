"""Immutable archive receipts and bounded notarization without Apple credentials."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import notarize_release as notary


class NotarizeReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        self.archive = self.folder / "LoopZ.zip"
        self.archive.write_bytes(b"immutable signed archive fixture")
        self.path = self.folder / "receipt.json"
        self.value = dict(
            schema=1,
            status="prepared",
            archive=str(self.archive),
            archiveSHA256=notary.sha(self.archive),
            notarizationVerified=False,
        )
        notary.write_receipt(self.path, self.value)
        self.submission = "ac5a621a-d1ec-49c3-a4d6-652bf95007ae"
        self.calls = []

    def invoke(self, arguments, timeout=60):
        self.calls.append((arguments, timeout))
        value = {"id": self.submission, "status": "In Progress"}
        return subprocess.CompletedProcess(arguments, 0, json.dumps(value), "")

    def test_submit_records_id_once_without_waiting_or_mutating_archive(self):
        before = self.archive.read_bytes()
        notary.submit(self.path, self.value, ["fixture-auth"], self.invoke)
        saved = notary.read_receipt(self.path)
        self.assertEqual(saved["submissionID"], self.submission)
        self.assertEqual(saved["status"], "submitted")
        self.assertIn("--no-wait", self.calls[0][0])
        self.assertEqual(self.archive.read_bytes(), before)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        with self.assertRaisesRegex(ValueError, "cannot be resubmitted"):
            notary.submit(self.path, saved, [], self.invoke)
        self.assertEqual(len(self.calls), 1)

    def test_upload_timeout_does_not_allow_automatic_resubmission(self):
        def timeout(arguments, timeout=60):
            raise subprocess.TimeoutExpired(arguments, timeout)

        with self.assertRaises(subprocess.TimeoutExpired):
            notary.submit(self.path, self.value, [], timeout)
        saved = notary.read_receipt(self.path)
        self.assertEqual(saved["status"], "uploading")
        self.assertNotIn("submissionID", saved)
        with self.assertRaises(ValueError):
            notary.submit(self.path, saved, [], self.invoke)
        self.assertFalse(self.calls)

    def test_pending_and_accepted_checks_use_same_id_and_never_submit(self):
        self.value.update(status="submitted", submissionID=self.submission)
        notary.check(self.path, self.value, [], self.invoke)
        self.assertEqual(self.value["status"], "In Progress")
        self.assertFalse(self.value["notarizationVerified"])

        def accepted(arguments, timeout=60):
            self.calls.append((arguments, timeout))
            return subprocess.CompletedProcess(
                arguments, 0, json.dumps({"id": self.submission, "status": "Accepted"}), ""
            )

        notary.check(self.path, self.value, [], accepted)
        self.assertTrue(self.value["notarizationVerified"])
        self.assertTrue(all(call[0][2:4] == ["info", self.submission] for call in self.calls))

    def test_archive_change_or_redirection_is_rejected(self):
        self.archive.write_bytes(b"replacement")
        with self.assertRaisesRegex(ValueError, "changed"):
            notary.read_receipt(self.path)
        self.archive.unlink()
        target = self.folder / "different.zip"
        target.write_bytes(b"immutable signed archive fixture")
        self.archive.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "redirected"):
            notary.read_receipt(self.path)

    def test_submission_requires_exact_reviewed_archive_digest(self):
        for digest in (None, "", "a" * 64, "not-a-digest"):
            with self.subTest(digest=digest):
                with self.assertRaisesRegex(ValueError, "reviewed archive"):
                    notary.verify_submission(self.value, digest)

    def test_archive_contains_exact_reviewed_files_without_extra_data(self):
        expected = {"Component": notary.sha(self.archive)}
        payload = self.archive.read_bytes()
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("release/Component", payload)
        self.assertEqual(notary.verify_archive(self.archive, "release", expected), 1)
        with zipfile.ZipFile(self.archive, "a") as archive:
            archive.writestr("release/private-config.json", b"unreviewed fixture")
        with self.assertRaises(ValueError):
            notary.verify_archive(self.archive, "release", expected)

    def test_archive_escape_symlink_and_changed_payload_are_rejected(self):
        expected = {"Component": notary.sha(self.archive)}
        for name, payload, mode in [
            ("release/../Component", b"fixture", 0),
            ("release/Component", b"target", 0o120777 << 16),
            ("release/Component", b"replaced bytes", 0),
        ]:
            with self.subTest(name=name, mode=mode):
                info = zipfile.ZipInfo(name)
                info.external_attr = mode
                with zipfile.ZipFile(self.archive, "w") as archive:
                    archive.writestr(info, payload)
                with self.assertRaises(ValueError):
                    notary.verify_archive(self.archive, "release", expected)

    def test_mismatched_submission_cannot_mark_receipt_accepted(self):
        self.value.update(status="submitted", submissionID=self.submission)

        def wrong(arguments, timeout=60):
            return subprocess.CompletedProcess(
                arguments,
                0,
                json.dumps({"id": "bf588dfe-c5b8-4f9a-a461-cdd80e8dc9b8", "status": "Accepted"}),
                "",
            )

        with self.assertRaises(ValueError):
            notary.check(self.path, self.value, [], wrong)
        self.assertFalse(self.value["notarizationVerified"])

    def test_api_key_requires_private_user_owned_regular_file(self):
        key = self.folder / "key.p8"
        key.write_bytes(b"private fixture")
        key.chmod(0o600)
        environment = dict(
            SINGZ_ASC_KEY_PATH=str(key),
            APP_STORE_CONNECT_API_KEY_ID="TESTKEY123",
            APP_STORE_CONNECT_API_ISSUER_ID=self.submission,
        )
        arguments = notary.api_auth(environment)
        self.assertNotIn("private fixture", " ".join(arguments))
        key.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "private regular"):
            notary.api_auth(environment)
        key.chmod(0o600)
        linked = self.folder / "linked.p8"
        linked.symlink_to(key)
        with self.assertRaises(ValueError):
            notary.api_auth({**environment, "SINGZ_ASC_KEY_PATH": str(linked)})


if __name__ == "__main__":
    unittest.main()
