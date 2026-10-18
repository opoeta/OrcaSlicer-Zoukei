"""Tests of the Zoukei supports tools (G1) against a fake Remote API.

    D:/orca/orcaslicer-mcp-venv/Scripts/python.exe -m unittest discover -s tools/orcaslicer-mcp-zoukei/tests
"""
import unittest

from fakeapi import REPLIES, SEEN, ToolTestCase


class SupportToolsTest(ToolTestCase):
    MODULES = ("tools_support",)

    def test_tool_list(self):
        self.assertEqual(sorted(self.mcp.tools), sorted([
            "support_volume_list", "support_volume_add", "support_volume_remove",
            "support_paint", "support_paint_clear", "support_paint_summary"]))

    def test_list(self):
        self.run_tool("support_volume_list", object_id=7)
        method, path, _, token, _ = self.last()
        self.assertEqual((method, path, token), ("GET", "/api/v1/objects/7/support_volumes", "test-token"))

    def test_add_box(self):
        REPLIES[("POST", "/api/v1/objects/7/support_volumes")] = (201, {"added": True, "volume": 2})
        out = self.run_tool("support_volume_add", object_id=7, kind="blocker", shape="box",
                            size=[10, 10, 5], position=[50, 50, 2.5], name="No support here")
        self.assertEqual(out["volume"], 2)
        method, path, body, _, _ = self.last()
        self.assertEqual((method, path), ("POST", "/api/v1/objects/7/support_volumes"))
        self.assertEqual(body, {"kind": "blocker", "shape": "box", "size": [10, 10, 5],
                                "position": [50, 50, 2.5], "name": "No support here"})

    def test_add_from_file_sends_scale(self):
        self.run_tool("support_volume_add", object_id=7, kind="enforcer", file="C:/m/x.stl", scale=0.5)
        body = self.last()[2]
        self.assertEqual(body["file"], "C:/m/x.stl")
        self.assertEqual(body["scale"], 0.5)
        self.assertNotIn("shape", body)

    def test_remove_one_and_all(self):
        self.run_tool("support_volume_remove", object_id=7, volume=3)
        self.assertEqual(self.last()[:3], ("DELETE", "/api/v1/objects/7/support_volumes", {"volume": 3}))
        self.run_tool("support_volume_remove", object_id=7, all=True, kind="blocker")
        self.assertEqual(self.last()[2], {"kind": "blocker", "all": True})

    def test_paint_overhang(self):
        self.run_tool("support_paint", object_id=7, state="enforcer", overhang_angle=45.0)
        method, path, body, _, _ = self.last()
        self.assertEqual((method, path), ("POST", "/api/v1/objects/7/support_paint"))
        self.assertEqual(body, {"state": "enforcer", "rule": {"overhang_angle": 45.0}})

    def test_paint_fill_volume(self):
        self.run_tool("support_paint", object_id=7, state="blocker", fill=True, volume=1)
        self.assertEqual(self.last()[2], {"state": "blocker", "volume": 1, "rule": {"fill": True}})

    def test_paint_band_and_coverage(self):
        self.run_tool("support_paint", object_id=7, state="blocker", z_min=0.0, z_max=5.0, coverage="any")
        self.assertEqual(self.last()[2]["rule"], {"z_min": 0.0, "z_max": 5.0, "coverage": "any"})

    def test_clear_without_rule_has_no_rule(self):
        self.run_tool("support_paint_clear", object_id=7)
        method, path, body, _, _ = self.last()
        self.assertEqual((method, path, body), ("POST", "/api/v1/objects/7/support_paint/clear", {}))

    def test_clear_with_rule(self):
        self.run_tool("support_paint_clear", object_id=7, z_min=10.0)
        self.assertEqual(self.last()[2], {"rule": {"z_min": 10.0}})

    def test_summary(self):
        self.run_tool("support_paint_summary", object_id=7)
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/objects/7/support_paint"))

    def test_api_error_is_returned_not_raised(self):
        REPLIES[("POST", "/api/v1/objects/9/support_paint")] = (404, {"error": "unknown_object"})
        out = self.run_tool("support_paint", object_id=9, state="enforcer", fill=True)
        self.assertIn("error", out)


if __name__ == "__main__":
    unittest.main()
