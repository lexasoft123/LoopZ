"""Catalog disappearance races must retry completely and still fail closed."""

import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import check_background_routing as routing


def reply(code=0, output=None, error=""):
    return subprocess.CompletedProcess(
        [routing.CHECKER, "--snapshot"],
        code,
        json.dumps({"routingDevices": []}) if output is None and code == 0 else output or "",
        error,
    )


class CatalogTests(unittest.TestCase):
    def test_stable_snapshot_is_returned_without_retry(self):
        snapshot = {"routingDevices": [{"uid": "actual"}], "defaultOutput": "physical"}
        with patch.object(routing, "run", return_value=reply(output=json.dumps(snapshot))) as run:
            with patch.object(routing.time, "sleep") as sleep:
                self.assertEqual(routing.catalog(), snapshot)
                self.assertEqual(run.call_count, 1)
                sleep.assert_not_called()

    def test_disappearance_retries_the_complete_snapshot(self):
        snapshot = {"routingDevices": [{"uid": "still-present"}]}
        with patch.object(
            routing, "run", side_effect=[reply(1), reply(1), reply(output=json.dumps(snapshot))]
        ) as run:
            with patch.object(routing.time, "sleep") as sleep:
                self.assertEqual(routing.catalog(), snapshot)
                self.assertEqual(run.call_count, 3)
                self.assertEqual(sleep.call_count, 2)
                self.assertTrue(all(call.args == (0.1,) for call in sleep.call_args_list))

    def test_unobservable_catalog_never_means_withdrawn(self):
        with patch.object(routing, "run", return_value=reply(1)) as run:
            with patch.object(routing.time, "sleep") as sleep:
                with self.assertRaises(subprocess.CalledProcessError):
                    routing.catalog()
                self.assertEqual(run.call_count, 5)
                self.assertEqual(sleep.call_count, 4)

    def test_diagnostic_errors_are_not_retried(self):
        for result in (
            reply(1, error="permission denied"),
            reply(1, output="invalid"),
            reply(77),
            reply(-9),
        ):
            with self.subTest(result=result):
                with patch.object(routing, "run", return_value=result) as run:
                    with patch.object(routing.time, "sleep") as sleep:
                        with self.assertRaises(subprocess.CalledProcessError):
                            routing.catalog()
                        self.assertEqual(run.call_count, 1)
                        sleep.assert_not_called()

    def test_malformed_success_is_not_an_empty_catalog(self):
        with patch.object(routing, "run", return_value=reply(output="not JSON")) as run:
            with self.assertRaises(json.JSONDecodeError):
                routing.catalog()
            self.assertEqual(run.call_count, 1)

    def test_timeout_is_not_hidden_by_retries(self):
        with patch.object(
            routing, "run", side_effect=subprocess.TimeoutExpired("snapshot", 20)
        ) as run:
            with self.assertRaises(subprocess.TimeoutExpired):
                routing.catalog()
            self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
