"""A fake Orca Zoukei Remote API for the tool tests (no OrcaSlicer needed), shared by the G1-G4 test files.

Each test file subclasses ToolTestCase, names its tool modules in MODULES and drives the tools with
run_tool(); SEEN holds the requests the fake server received as (method, path, body, token, query).
"""
import asyncio
import importlib
import json
import os
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

SEEN = []
REPLIES = {}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _do(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}") if n else {}
        parts = urlsplit(self.path)
        query = {k: v[0] for k, v in parse_qs(parts.query).items()}
        SEEN.append((self.command, parts.path, body, self.headers.get("X-Api-Token"), query))
        status, payload = REPLIES.get((self.command, parts.path), (200, {"ok": True}))
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


class ToolTestCase(unittest.TestCase):
    MODULES = ()   # e.g. ("tools_support",)

    @classmethod
    def setUpClass(cls):
        cls.srv = HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        os.environ["ORCA_API_URL"] = f"http://127.0.0.1:{cls.srv.server_port}"
        os.environ["ORCA_API_TOKEN"] = "test-token"
        cls.mcp = StubMcp()
        for m in cls.MODULES:
            importlib.import_module(f"orcaslicer_mcp_zoukei.{m}").register(cls.mcp)

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def setUp(self):
        SEEN.clear()
        REPLIES.clear()

    def run_tool(self, _tool, **kw):
        return asyncio.run(self.mcp.tools[_tool](**kw))

    def last(self):
        return SEEN[-1]
