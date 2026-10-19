"""Tests of the Zoukei printers and printing tools (G5) against a fake Remote API."""
import unittest

from fakeapi import REPLIES, ToolTestCase


class DevicesToolsTest(ToolTestCase):
    MODULES = ("tools_devices",)

    def test_tool_list(self):
        self.assertEqual(sorted(self.mcp.tools), sorted([
            "device_list", "device_status", "device_slots", "print_hosts", "printer_control_status", "print_send",
            "print_pause", "print_resume", "print_stop", "printer_set_temperature", "filament_load", "filament_unload",
            "slot_write", "creality_cloud_status", "creality_cloud_filaments", "creality_cloud_upload",
            "creality_cloud_upload_status", "creality_cloud_upload_cancel"]))

    def test_reads(self):
        self.run_tool("device_list")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/devices"))
        self.run_tool("device_status")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/devices/selected"))
        self.run_tool("device_status", device="192.168.1.5:7125")
        self.assertEqual(self.last()[1], "/api/v1/devices/192.168.1.5:7125")
        self.run_tool("device_slots", device="abc")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/devices/abc/slots"))
        self.run_tool("print_hosts")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/print_hosts"))
        self.run_tool("printer_control_status")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/printer_control"))

    def test_creality_cloud(self):
        self.run_tool("creality_cloud_status")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/creality_cloud"))
        self.run_tool("creality_cloud_filaments")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/creality_cloud/filaments"))
        self.run_tool("creality_cloud_upload", presets=["My PLA"])
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/creality_cloud/upload", {"presets": ["My PLA"], "dry_run": True}))
        self.run_tool("creality_cloud_upload", presets=["My PLA"], dry_run=False)
        self.assertEqual(self.last()[2], {"presets": ["My PLA"]})
        self.run_tool("creality_cloud_upload", presets=["A", "B"], printer="Creality K2 Plus 0.4 nozzle", dry_run=False, confirm=True)
        self.assertEqual(self.last()[2], {"presets": ["A", "B"], "printer": "Creality K2 Plus 0.4 nozzle", "confirm": True})
        self.run_tool("creality_cloud_upload", presets=["A"], dry_run=False, confirm=True, wait=True)
        self.assertEqual(self.last()[2], {"presets": ["A"], "confirm": True, "wait": True})
        self.run_tool("creality_cloud_upload_status")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/creality_cloud/upload_status"))
        self.run_tool("creality_cloud_upload_cancel")
        self.assertEqual(self.last()[:2], ("POST", "/api/v1/creality_cloud/upload_cancel"))

    def test_control_without_confirm_sends_no_confirm(self):
        # The app answers 403; the tool must never add confirm on its own.
        REPLIES[("POST", "/api/v1/devices/selected/pause")] = (403, {"error": "blocked_by_remote_api_policy"})
        out = self.run_tool("print_pause")
        self.assertIn("error", out)
        self.assertEqual(self.last()[2], {})

    def test_control_with_confirm(self):
        self.run_tool("print_pause", confirm=True)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/devices/selected/pause", {"confirm": True}))
        self.run_tool("print_resume", device="d1", confirm=True)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/devices/d1/resume", {"confirm": True}))
        self.run_tool("print_stop", confirm=True)
        self.assertEqual(self.last()[:2], ("POST", "/api/v1/devices/selected/stop"))

    def test_print_send(self):
        self.run_tool("print_send", name="benchy", start=True, plate=1, confirm=True)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/print/send", {"name": "benchy", "plate": 1, "start": True, "confirm": True}))
        self.run_tool("print_send")
        self.assertEqual(self.last()[2], {})

    def test_temperature(self):
        self.run_tool("printer_set_temperature", nozzle=210, bed=60, confirm=True)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/devices/selected/temperature",
                                           {"nozzle": 210, "bed": 60, "extruder": 0, "confirm": True}))
        self.run_tool("printer_set_temperature", bed=0, confirm=True)
        self.assertEqual(self.last()[2], {"bed": 0, "confirm": True})

    def test_filament(self):
        self.run_tool("filament_load", slot=2, unit=0, nozzle_temp=230, confirm=True)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/devices/selected/filament",
                                           {"action": "load", "slot": 2, "unit": 0, "nozzle_temp": 230, "confirm": True}))
        self.run_tool("filament_unload", slot=1, device="u1", confirm=True)
        self.assertEqual(self.last()[1:3], ("/api/v1/devices/u1/filament", {"action": "unload", "slot": 1, "confirm": True}))

    def test_slot_write(self):
        self.run_tool("slot_write", slot=3, type="PETG", color="#00FF00", confirm=True)
        self.assertEqual(self.last()[:3], ("PUT", "/api/v1/devices/selected/slots",
                                           {"slot": 3, "type": "PETG", "color": "#00FF00", "confirm": True}))
        self.run_tool("slot_write", slot=0, colors=["#FF0000", "#0000FF"])
        self.assertEqual(self.last()[2], {"slot": 0, "colors": ["#FF0000", "#0000FF"]})

    def test_agent_refusal_returned(self):
        REPLIES[("PUT", "/api/v1/devices/selected/slots")] = (422, {"error": "agent_refused", "detail": "official Snapmaker spool"})
        self.assertIn("error", self.run_tool("slot_write", slot=0, color="#FFFFFF", confirm=True))


if __name__ == "__main__":
    unittest.main()
