"""Tests of the Zoukei colour tools against a fake Remote API (no OrcaSlicer needed).

Run with the bridge's own interpreter:
    D:/orca/orcaslicer-mcp-venv/Scripts/python.exe -m unittest discover -s tools/orcaslicer-mcp-zoukei/tests
"""
import asyncio
import json
import os
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

SEEN = []
REPLIES = {}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _do(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}") if n else {}
        SEEN.append((self.command, self.path, body, self.headers.get("X-Api-Token")))
        status, payload = REPLIES.get((self.command, self.path.split("?")[0]), (200, {"ok": True}))
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_GET = do_POST = do_PUT = do_DELETE = _do


class StubMcp:
    def __init__(self):
        self.tools = {}

    def tool(self, *a, **k):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


class ColorToolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        os.environ["ORCA_API_URL"] = f"http://127.0.0.1:{cls.srv.server_port}"
        os.environ["ORCA_API_TOKEN"] = "test-token"
        from orcaslicer_mcp_zoukei import tools_color
        cls.mcp = StubMcp()
        tools_color.register(cls.mcp)

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def setUp(self):
        SEEN.clear()
        REPLIES.clear()

    def run_tool(self, name, **kw):
        return asyncio.run(self.mcp.tools[name](**kw))

    def test_tool_list(self):
        self.assertEqual(
            sorted(self.mcp.tools),
            sorted(["filament_list", "filament_set", "mixed_filament_list", "mixed_filament_add",
                    "mixed_filament_update", "mixed_filament_remove", "color_mix_solve", "color_mix_apply",
                    "model_colors", "import_colored_model", "set_object_filament"]))

    def test_add_sends_pattern_and_mode(self):
        REPLIES[("POST", "/api/v1/mixed_filaments")] = (201, {"slot": 5, "mixed": True})
        out = self.run_tool("mixed_filament_add", components=[1, 2], pattern="AAB", mode="sublayers")
        self.assertEqual(out["slot"], 5)
        method, path, body, token = SEEN[-1]
        self.assertEqual((method, path, token), ("POST", "/api/v1/mixed_filaments", "test-token"))
        self.assertEqual(body["pattern"], "AAB")
        self.assertEqual(body["mode"], "sublayers")
        self.assertNotIn("ratios", body)

    def test_update_only_sends_given_fields(self):
        self.run_tool("mixed_filament_update", slot=7, ratios=[60, 40])
        method, path, body, _ = SEEN[-1]
        self.assertEqual((method, path), ("PUT", "/api/v1/mixed_filaments/7"))
        self.assertEqual(body["ratios"], [60, 40])
        self.assertNotIn("components", body)

    def test_remove(self):
        self.run_tool("mixed_filament_remove", slot=6)
        self.assertEqual(SEEN[-1][:2], ("DELETE", "/api/v1/mixed_filaments/6"))

    def test_solve_apply_roundtrip(self):
        REPLIES[("POST", "/api/v1/color_mix/solve")] = (200, {"solve_id": 3, "clusters": []})
        r = self.run_tool("color_mix_solve", targets=["#FF0000", {"hex": "#00FF00", "weight": 2}],
                          loaded=[1, 2, 3, 4], max_mixes=2, allow_triples=True, method="layers")
        self.assertEqual(r["solve_id"], 3)
        body = SEEN[-1][2]
        self.assertEqual(body["loaded"], [1, 2, 3, 4])
        self.assertTrue(body["allow_triples"])
        self.assertEqual(body["targets"][1]["weight"], 2)
        self.run_tool("color_mix_apply", solve_id=3, overrides={"1": {"filament": 2}})
        self.assertEqual(SEEN[-1][2], {"solve_id": 3, "overrides": {"1": {"filament": 2}}})

    def test_import_colored_defaults(self):
        self.run_tool("import_colored_model", path="C:/m.obj")
        self.assertEqual(SEEN[-1][:2], ("POST", "/api/v1/model/import_colored"))
        self.assertEqual(SEEN[-1][2], {"path": "C:/m.obj", "max_colors": 0, "smooth": 5})

    def test_api_error_is_returned_not_raised(self):
        REPLIES[("PUT", "/api/v1/filaments/9")] = (404, {"error": "unknown_filament"})
        out = self.run_tool("filament_set", slot=9, colour="#112233")
        self.assertIn("error", out)

    def test_set_object_filament_volume(self):
        self.run_tool("set_object_filament", object_id=12, filament=3, volume=1)
        self.assertEqual(SEEN[-1][:3], ("PUT", "/api/v1/objects/12/filament", {"filament": 3, "volume": 1}))


if __name__ == "__main__":
    unittest.main()
