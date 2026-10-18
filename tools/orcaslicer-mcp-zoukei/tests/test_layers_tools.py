"""Tests of the Zoukei layers and plates tools (G4) against a fake Remote API."""
import unittest

from fakeapi import REPLIES, ToolTestCase


class LayersToolsTest(ToolTestCase):
    MODULES = ("tools_layers",)

    def test_tool_list(self):
        self.assertEqual(sorted(self.mcp.tools), sorted([
            "layer_gcode_list", "layer_gcode_add", "layer_gcode_remove", "object_layers", "layer_height_profile",
            "height_range_remove", "plate_settings", "plate_set", "plate_move"]))

    def test_list_default_and_plate(self):
        self.run_tool("layer_gcode_list")
        self.assertEqual(self.last()[1], "/api/v1/layer_gcodes")
        self.assertEqual(self.last()[4], {})
        self.run_tool("layer_gcode_list", plate=1)
        self.assertEqual(self.last()[4], {"plate": "1"})

    def test_add_color_change(self):
        self.run_tool("layer_gcode_add", type="color_change", height=4.2, extruder=2, color="#FF0000")
        method, path, body, _, _ = self.last()
        self.assertEqual((method, path), ("POST", "/api/v1/layer_gcodes"))
        self.assertEqual(body, {"type": "color_change", "height": 4.2, "color": "#FF0000", "extruder": 2})

    def test_add_pause_has_no_extruder(self):
        self.run_tool("layer_gcode_add", type="pause", height=10.0, message="insert magnet", plate=0)
        self.assertEqual(self.last()[2], {"type": "pause", "height": 10.0, "plate": 0, "message": "insert magnet"})

    def test_add_custom_with_replace(self):
        self.run_tool("layer_gcode_add", type="custom", height=5.0, gcode="M117 hi\nM300 S440 P200", replace=True)
        body = self.last()[2]
        self.assertEqual(body["gcode"], "M117 hi\nM300 S440 P200")
        self.assertTrue(body["replace"])

    def test_remove_by_height_and_all(self):
        self.run_tool("layer_gcode_remove", height=4.2)
        self.assertEqual(self.last()[:3], ("DELETE", "/api/v1/layer_gcodes", {"height": 4.2}))
        self.run_tool("layer_gcode_remove", all=True, type="pause", plate=2)
        self.assertEqual(self.last()[2], {"plate": 2, "type": "pause", "all": True})

    def test_object_layers(self):
        self.run_tool("object_layers", object_id=5)
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/objects/5/layers"))

    def test_profile_modes_use_the_right_endpoint(self):
        self.run_tool("layer_height_profile", object_id=5, mode="adaptive", quality=0.8)
        self.assertEqual(self.last()[:3], ("PUT", "/api/v1/objects/5/layer_height", {"mode": "adaptive", "quality": 0.8}))
        self.run_tool("layer_height_profile", object_id=5, mode="reset")
        self.assertEqual(self.last()[2], {"mode": "reset"})
        self.run_tool("layer_height_profile", object_id=5, mode="smooth", radius=8, keep_min=True)
        self.assertEqual(self.last()[:3], ("PUT", "/api/v1/objects/5/layer_height/smooth", {"radius": 8, "keep_min": True}))

    def test_height_range_remove(self):
        self.run_tool("height_range_remove", object_id=5, min_z=2.0, max_z=4.0)
        self.assertEqual(self.last()[:3], ("DELETE", "/api/v1/objects/5/height_range", {"min_z": 2.0, "max_z": 4.0}))

    def test_plate_settings_and_set(self):
        self.run_tool("plate_settings", plate=1)
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/plates/1/settings"))
        self.run_tool("plate_set", plate=1, name="Lid", locked=False, bed_type="textured_plate",
                      print_sequence="by_object", spiral_vase=False)
        method, path, body, _, _ = self.last()
        self.assertEqual((method, path), ("PUT", "/api/v1/plates/1/settings"))
        self.assertEqual(body, {"name": "Lid", "locked": False, "bed_type": "textured_plate",
                                "print_sequence": "by_object", "spiral_vase": False})

    def test_plate_set_only_sends_given(self):
        self.run_tool("plate_set", plate=0, locked=True)
        self.assertEqual(self.last()[2], {"locked": True})

    def test_plate_move(self):
        self.run_tool("plate_move", plate=2, to=0)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/plates/2/move", {"to": 0}))

    def test_policy_error_returned(self):
        REPLIES[("POST", "/api/v1/layer_gcodes")] = (403, {"error": "blocked_by_remote_api_policy"})
        self.assertIn("error", self.run_tool("layer_gcode_add", type="custom", height=1.0, gcode="M104 S0"))


if __name__ == "__main__":
    unittest.main()
