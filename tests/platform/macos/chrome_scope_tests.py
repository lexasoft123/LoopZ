"""Chrome capture must stay inside both its process and virtual-output scope."""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from check_chrome_routing import capture_scope


class ChromeScopeTests(unittest.TestCase):
    def setUp(self):
        self.selected = {
            "success": True,
            "producing": True,
            "processes": [{"pid": 101}, {"pid": 102}],
            "outputs": [{"uid": "owned-sink"}],
        }

    def scope(self, selected=None, allow_existing=False):
        return capture_scope(
            self.selected if selected is None else selected,
            {100, 101, 102}, "owned-sink", allow_existing,
        )

    def test_owned_active_profile_is_accepted(self):
        self.assertEqual(self.scope(), self.selected)

    def test_idle_or_unavailable_selection_cannot_start_capture(self):
        for change in ({"success": False}, {"producing": False}, {"processes": []}):
            selected = {**self.selected, **change}
            self.assertFalse(self.scope(selected))

    def test_other_profile_requires_explicit_scope_extension(self):
        self.selected["processes"].append({"pid": 999})
        with self.assertRaisesRegex(RuntimeError, "outside the owned browser"):
            self.scope()
        self.assertEqual(self.scope(allow_existing=True), self.selected)

    def test_broader_process_permission_never_allows_other_outputs(self):
        for outputs in ([], [{"uid": "hardware"}],
                        [{"uid": "owned-sink"}, {"uid": "hardware"}]):
            selected = copy.deepcopy(self.selected)
            selected["outputs"] = outputs
            for allow_existing in (False, True):
                with self.assertRaisesRegex(RuntimeError, "outside the owned virtual sink"):
                    self.scope(selected, allow_existing)


if __name__ == "__main__":
    unittest.main()
