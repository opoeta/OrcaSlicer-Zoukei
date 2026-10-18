#!/usr/bin/env python3
"""Tests of mock_u1_moonraker.py: the firmware rules the Orca Zoukei U1 integration relies on.

    python tools/zoukei/test_mock_u1.py

Starts the mock on a free 127.0.0.1 port in this process; nothing else is touched.
"""
import json
import os
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mock_u1_moonraker as mock  # noqa: E402


class MockU1Tests(unittest.TestCase):
    def setUp(self):
        self.printer = mock.Printer(official=(0,), empty=(3,), record=None, fast=True)
        mock.Handler.printer = self.printer
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), mock.Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.printer.running = False
        self.server.shutdown()
        self.server.server_close()

    def post(self, path, body):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def script(self, text):
        return self.post("/printer/gcode/script", {"script": text})

    def ptc(self):
        _, r = self.post("/printer/objects/query", {"objects": {"print_task_config": None}})
        return r["result"]["status"]["print_task_config"]

    def test_query_shape(self):
        status, r = self.post("/printer/objects/query", {"objects": {"print_task_config": None, "filament_feed left": None, "extruder1": ["temperature"]}})
        self.assertEqual(status, 200)
        st = r["result"]["status"]
        self.assertEqual(len(st["print_task_config"]["filament_type"]), 4)
        self.assertEqual(sorted(st["filament_feed left"]), ["extruder0", "extruder1"])
        self.assertEqual(list(st["extruder1"]), ["temperature"])

    def test_official_head_is_locked_until_force(self):
        code, r = self.script("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=Generic FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=NONE FILAMENT_COLOR_RGBA=112233FF")
        self.assertEqual(code, 400)
        self.assertIn("official", r["error"]["message"])
        self.assertTrue(self.ptc()["filament_official"][0])
        code, _ = self.script('SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR="My Brand" FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=NONE FILAMENT_COLOR_RGBA=112233FF FORCE=1')
        self.assertEqual(code, 200)
        p = self.ptc()
        self.assertFalse(p["filament_official"][0])
        self.assertEqual(p["filament_vendor"][0], "My Brand")
        self.assertEqual(p["filament_color_rgba"][0], "112233FF")
        self.assertTrue(p["filament_edit"][0])

    def test_type_needs_vendor_and_subtype(self):
        code, _ = self.script("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_TYPE=PLA")
        self.assertEqual(code, 400)
        code, _ = self.script("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=ABCDEF")   # colour only, rgb gets alpha FF
        self.assertEqual(code, 200)
        self.assertEqual(self.ptc()["filament_color_rgba"][1], "ABCDEFFF")

    def test_multi_colour(self):
        code, _ = self.script("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 COLOR_NUMS=2 COLORS=FF0000,00FF00 MULTI_MODE=1 ALPHA=255")
        self.assertEqual(code, 200)
        multi = self.ptc()["filament_color_multi"][2]
        self.assertEqual((multi["nums"], multi["colors"], multi["mode"]), (2, ["FF0000", "00FF00"], 1))

    def test_extruder_map_and_used(self):
        code, _ = self.script("SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2")
        self.assertEqual(code, 200)
        p = self.ptc()
        self.assertEqual(p["extruder_map_table"][:2], [2, 0])
        self.assertEqual(p["extruders_used"], [True, False, True, False])
        code, _ = self.script("SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=7")
        self.assertEqual(code, 400)

    def test_writes_refused_while_printing(self):
        self.post("/mock/set", {"print_state": "printing"})
        for line in ("SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1", "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0", "SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=0"):
            code, r = self.script(line)
            self.assertEqual(code, 400, line)
            self.assertIn("not allow", r["error"]["message"])
        self.assertEqual(self.ptc()["extruder_map_table"][:4], [0, 1, 2, 3])

    def test_load_unload(self):
        code, _ = self.script("AUTO_FEEDING EXTRUDER=1 UNLOAD=1")
        self.assertEqual(code, 200)
        p = self.ptc()
        self.assertFalse(p["filament_exist"][1])
        self.assertEqual(p["filament_type"][1], "NONE")
        code, r = self.script("AUTO_FEEDING EXTRUDER=3 LOAD=1")   # nothing at the feeder of head 3
        self.assertEqual(code, 400)
        self.assertIn("no_filament", r["error"]["message"])

    def test_scripts_are_recorded(self):
        self.script("M104 S200 T1")
        self.script("SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=9 MAP_EXTRUDER=9")
        with urllib.request.urlopen(self.base + "/mock/scripts", timeout=5) as r:
            scripts = json.loads(r.read())["scripts"]
        self.assertEqual([s["ok"] for s in scripts], [True, False])
        self.assertEqual(scripts[0]["script"], "M104 S200 T1")


if __name__ == "__main__":
    unittest.main()
