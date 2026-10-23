import ctypes
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

from windows_acceptance_report import write_report


def locked_error():
    error = PermissionError("reader prevents replacement")
    error.winerror = 5
    return error


class AcceptanceReportTests(unittest.TestCase):
    def test_reader_lock_retries_without_changing_previous_report(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "report.json"
            path.write_text('{"previous": true}', encoding="utf-8")
            original_replace = Path.replace
            calls = []

            def replace(source, target):
                calls.append(1)
                self.assertEqual(json.loads(path.read_text()), {"previous": True})
                if len(calls) == 1:
                    raise locked_error()
                return original_replace(source, target)

            with (
                patch.object(Path, "replace", replace),
                patch("windows_acceptance_report.time.sleep") as sleep,
            ):
                write_report(path, {"next": "Москва"})
            self.assertEqual(json.loads(path.read_text()), {"next": "Москва"})
            self.assertEqual(len(calls), 2)
            sleep.assert_called_once_with(0.05)
            self.assertFalse(path.with_suffix(".pending").exists())

    def test_persistent_lock_has_a_deadline(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "report.json"
            with (
                patch.object(Path, "replace", side_effect=locked_error()),
                patch("windows_acceptance_report.time.monotonic", side_effect=[0, 0, 5]),
                patch("windows_acceptance_report.time.sleep") as sleep,
            ):
                with self.assertRaises(PermissionError):
                    write_report(path, {"ok": False})
            sleep.assert_called_once()
            self.assertFalse(path.exists())
            self.assertTrue(path.with_suffix(".pending").exists())

    def test_other_errors_are_not_retried(self):
        for error in (PermissionError("permissions"), FileNotFoundError("missing")):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as folder:
                with (
                    patch.object(Path, "replace", side_effect=error),
                    patch("windows_acceptance_report.time.sleep") as sleep,
                ):
                    with self.assertRaises(type(error)):
                        write_report(Path(folder) / "report.json", {})
                sleep.assert_not_called()

    @unittest.skipUnless(sys.platform == "win32", "requires Windows file sharing")
    def test_real_windows_reader_lock(self):
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_void_p,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        ]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "report.json"
            path.write_text('{"old": true}', encoding="utf-8")
            handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
            if handle == wintypes.HANDLE(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            released = threading.Event()

            def release():
                kernel.CloseHandle(handle)
                released.set()

            try:
                with self.assertRaises(PermissionError):
                    path.with_suffix(".pending").write_text("{}", encoding="utf-8")
                    path.with_suffix(".pending").replace(path)
                timer = threading.Timer(0.2, release)
                timer.start()
                try:
                    write_report(path, {"new": True})
                finally:
                    timer.join()
                self.assertEqual(json.loads(path.read_text()), {"new": True})
            finally:
                if not released.is_set():
                    release()


if __name__ == "__main__":
    unittest.main()
