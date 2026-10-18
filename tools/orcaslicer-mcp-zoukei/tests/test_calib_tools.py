"""Tests of the Zoukei calibration tools (G6) against a fake Remote API."""
import unittest

from fakeapi import REPLIES, ToolTestCase


class CalibToolsTest(ToolTestCase):
    MODULES = ("tools_calib",)

    def test_tool_list(self):
        self.assertEqual(sorted(self.mcp.tools), ["calibration_list", "calibration_start"])

    def test_list(self):
        self.run_tool("calibration_list")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/calibrations"))

    def test_start_defaults_send_empty_body(self):
        self.run_tool("calibration_start", kind="retraction")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/calibrations/retraction/start", {}))

    def test_temperature(self):
        self.run_tool("calibration_start", kind="temperature", filament_type="petg", start=250, end=230, auto_scale=False,
                      discard_changes=True)
        self.assertEqual(self.last()[1:3], ("/api/v1/calibrations/temperature/start",
                                            {"discard_changes": True, "start": 250, "end": 230, "filament_type": "petg", "auto_scale": False}))

    def test_pressure_advance_pattern(self):
        self.run_tool("calibration_start", kind="pressure_advance", method="pattern", accelerations=[500, 1000], speeds=[100, 200])
        self.assertEqual(self.last()[2], {"method": "pattern", "accelerations": [500, 1000], "speeds": [100, 200]})

    def test_flow_type_is_named_type_in_the_body(self):
        self.run_tool("calibration_start", kind="flow_rate", flow_type="yolo_perfectionist", pattern="monotonic")
        self.assertEqual(self.last()[2], {"type": "yolo_perfectionist", "pattern": "monotonic"})

    def test_input_shaping(self):
        self.run_tool("calibration_start", kind="input_shaping_freq", freq_start_x=20, freq_end_x=100, damping=0.1,
                      shaper_type="mzv", test_model=1)
        self.assertEqual(self.last()[1], "/api/v1/calibrations/input_shaping_freq/start")
        self.assertEqual(self.last()[2], {"freq_start_x": 20, "freq_end_x": 100, "damping": 0.1, "shaper_type": "mzv", "test_model": 1})
        self.run_tool("calibration_start", kind="input_shaping_damp", freq_x=40, freq_y=40, damping_start=0.0, damping_end=0.3)
        self.assertEqual(self.last()[2], {"freq_x": 40, "freq_y": 40, "damping_start": 0.0, "damping_end": 0.3})

    def test_conflict_returned(self):
        REPLIES[("POST", "/api/v1/calibrations/vfa/start")] = (409, {"error": "unsaved_changes"})
        self.assertIn("error", self.run_tool("calibration_start", kind="vfa"))


if __name__ == "__main__":
    unittest.main()
