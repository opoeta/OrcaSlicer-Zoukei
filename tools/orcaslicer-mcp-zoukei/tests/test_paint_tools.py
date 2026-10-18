"""Tests of the Zoukei painting tools (G3) against a fake Remote API."""
import unittest

from fakeapi import REPLIES, ToolTestCase


class PaintToolsTest(ToolTestCase):
    MODULES = ("tools_paint",)

    def test_tool_list(self):
        self.assertEqual(sorted(self.mcp.tools), sorted([
            "paint_color", "paint_seam", "paint_fuzzy_skin", "paint_clear", "paint_summary"]))

    def test_color_height_band(self):
        self.run_tool("paint_color", object_id=3, filament=2, z_min=0.0, z_max=10.0)
        method, path, body, token, _ = self.last()
        self.assertEqual((method, path, token), ("POST", "/api/v1/objects/3/paint", "test-token"))
        self.assertEqual(body, {"kind": "color", "state": 2, "rule": {"z_min": 0.0, "z_max": 10.0}})

    def test_color_normal_direction(self):
        self.run_tool("paint_color", object_id=3, filament=4, normal=[0, 0, 1], normal_angle=20.0)
        self.assertEqual(self.last()[2]["rule"], {"normal": [0, 0, 1], "normal_angle": 20.0})

    def test_color_fill_volume(self):
        self.run_tool("paint_color", object_id=3, filament=1, fill=True, volume=0)
        self.assertEqual(self.last()[2], {"kind": "color", "state": 1, "volume": 0, "rule": {"fill": True}})

    def test_color_overhang(self):
        self.run_tool("paint_color", object_id=3, filament=3, overhang_angle=50.0)
        self.assertEqual(self.last()[2]["rule"], {"overhang_angle": 50.0})

    def test_seam(self):
        self.run_tool("paint_seam", object_id=3, state="block", box_min=[0, 0, 0], box_max=[10, 10, 10])
        body = self.last()[2]
        self.assertEqual(body["kind"], "seam")
        self.assertEqual(body["state"], "block")
        self.assertEqual(body["rule"], {"box_min": [0, 0, 0], "box_max": [10, 10, 10]})

    def test_fuzzy(self):
        self.run_tool("paint_fuzzy_skin", object_id=3, z_min=2.0)
        self.assertEqual(self.last()[2], {"kind": "fuzzy_skin", "state": "on", "rule": {"z_min": 2.0}})

    def test_clear_all_and_with_rule(self):
        self.run_tool("paint_clear", object_id=3, kind="color")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/objects/3/paint/clear", {"kind": "color"}))
        self.run_tool("paint_clear", object_id=3, kind="seam", z_max=1.0, volume=2)
        self.assertEqual(self.last()[2], {"kind": "seam", "volume": 2, "rule": {"z_max": 1.0}})

    def test_summary_query(self):
        self.run_tool("paint_summary", object_id=3, kind="color")
        method, path, _, _, query = self.last()
        self.assertEqual((method, path, query), ("GET", "/api/v1/objects/3/paint", {"kind": "color"}))
        self.run_tool("paint_summary", object_id=3)
        self.assertEqual(self.last()[4], {})

    def test_error_returned(self):
        REPLIES[("POST", "/api/v1/objects/3/paint")] = (422, {"error": "unknown_filament"})
        self.assertIn("error", self.run_tool("paint_color", object_id=3, filament=99, fill=True))


if __name__ == "__main__":
    unittest.main()
