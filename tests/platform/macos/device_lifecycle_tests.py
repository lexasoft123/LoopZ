"""Visibility acceptance distinguishes source/device enablement from mute/gain."""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from check_background_routing import definition
from check_device_lifecycle import public_endpoints, same_intent


class LifecycleExpectations(unittest.TestCase):
    def setUp(self):
        self.device = definition(2)
        self.uid = "com.lexasoft.loopz.device." + self.device["id"]

    def endpoint(self, device):
        return public_endpoints({"devices": [device]})

    def test_pass_through_visibility_changes_only_with_enablement(self):
        self.assertEqual(self.endpoint(self.device)[self.uid]["outputs"], 2)
        for field, value in (("muted", True), ("gain", 0)):
            device = copy.deepcopy(self.device)
            device["routing"]["sources"][0][field] = value
            self.assertEqual(self.endpoint(device)[self.uid]["outputs"], 2)
        self.device["routing"]["sources"][0]["enabled"] = False
        endpoint = self.endpoint(self.device)[self.uid]
        self.assertEqual((endpoint["inputs"], endpoint["outputs"]), (2, 0))

    def test_device_mute_and_zero_gain_retain_both_directions(self):
        self.device["routing"].update(muted=True, gain=0)
        self.assertEqual(self.endpoint(self.device)[self.uid]["outputs"], 2)

    def test_disable_withdraws_publication_but_preserves_saved_uid(self):
        self.device["routing"]["enabled"] = False
        self.assertEqual(self.endpoint(self.device), {})
        self.device["routing"]["enabled"] = True
        self.assertEqual(set(self.endpoint(self.device)), {self.uid})

    def test_unbound_and_non_pass_through_sources_cannot_publish_output(self):
        for binding in ([], [{"id": "pass-through", "kind": "application"}]):
            device = copy.deepcopy(self.device)
            device["bindings"] = binding
            self.assertEqual(self.endpoint(device)[self.uid]["outputs"], 0)

    def test_only_rate_receipts_may_change_without_changing_intent(self):
        expected = {"version": 2, "devices": [self.device]}
        actual = copy.deepcopy(expected)
        actual["devices"][0]["formatRequestReceipt"] = "driver-origin/2"
        self.assertTrue(same_intent(expected, actual))
        actual["devices"][0]["routing"]["gain"] = 0.25
        self.assertFalse(same_intent(expected, actual))
        actual = copy.deepcopy(expected)
        actual["devices"][0]["sampleRate"] = 44100
        self.assertFalse(same_intent(expected, actual))


if __name__ == "__main__":
    unittest.main()
