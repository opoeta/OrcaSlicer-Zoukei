#!/usr/bin/env python3
"""Tests of mock_creality_cloud.py: the Creality Cloud protocol the Orca Zoukei client (CrealityCloudClient.cpp)
relies on, replayed here the way the client sends it.

    python tools/zoukei/test_mock_creality_cloud.py

Starts the mock on a free 127.0.0.1 port in this process; nothing else is touched.
"""
import hashlib
import json
import os
import sys
import threading
import unittest
import urllib.error
import urllib.request
import uuid
from urllib.parse import parse_qs, quote, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mock_creality_cloud as mock  # noqa: E402

CLIENT_ID = "f9c302ecc29c59a0a6e921ff39a073ca"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def headers(session=None):
    h = {"Content-Type": "application/json", "__CXY_APP_ID_": "creality_model", "__CXY_PLATFORM_": "11",
         "__CXY_DUID_": "test-duid", "__CXY_REQUESTID_": str(uuid.uuid4()), "__CXY_OS_LANG_": "15", "__CXY_APP_VER_": "2.3.2"}
    if session:
        h["__CXY_UID_"] = session["userId"]
        h["__CXY_TOKEN_"] = session["token"]
    return h


class MockCrealityCloudTests(unittest.TestCase):
    def setUp(self):
        mock.STATE.reset()
        self.server = mock.make_server(0)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = mock.Handler.base

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    # ---- helpers ----
    def get(self, path):
        try:
            with OPENER.open(self.base + path, timeout=5) as r:
                return r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers, e.read()

    def api(self, path, body, session=None):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), headers=headers(session), method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())

    def login(self):
        redirect = self.base + "/oauth?back_url=" + mock.CALLBACK
        st, _, page = self.get(f"/id?lang=en-US&client_id={CLIENT_ID}&app_id=creality_model&redirect_uri={quote(redirect, safe='')}&platform=2")
        self.assertEqual(st, 200)
        self.assertIn(b"never type a real password", page)
        st, h, _ = self.get("/id/authorize?redirect_uri=" + quote(redirect, safe=""))
        self.assertEqual(st, 302)
        st, h, _ = self.get(urlparse(h["Location"]).path + "?" + urlparse(h["Location"]).query)
        self.assertEqual(st, 302)
        cb = h["Location"]
        self.assertTrue(cb.startswith(mock.CALLBACK + "?"))
        code = parse_qs(urlparse(cb).query)["code"][0]
        r = self.api("/api/cxy/account/v2/oauthLogin", {"code": code, "clientId": CLIENT_ID, "redirecturi": redirect})
        self.assertEqual(r["code"], 0, r)
        return {"token": r["result"]["token"], "userId": r["result"]["userId"], "code": code, "redirect": redirect}

    def oss(self, policy, content, name, extra_headers=None):
        boundary = "----orcatest" + uuid.uuid4().hex
        parts = []
        for k in ("OSSAccessKeyId", "Signature", "Policy", "Key", "Callback"):
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{policy[k]}\r\n'.encode())
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="File"; filename="{name}"\r\n'
                     f'Content-Type: application/octet-stream\r\n\r\n'.encode() + content + b"\r\n")
        parts.append(f"--{boundary}--\r\n".encode())
        h = {"Content-Type": "multipart/form-data; boundary=" + boundary}
        h.update(extra_headers or {})
        req = urllib.request.Request(policy["Host"], data=b"".join(parts), headers=h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code

    def state(self):
        return json.loads(self.get("/mock/state")[2])

    @staticmethod
    def preset(name="My PLA"):
        return json.dumps({"name": name, "from": "User", "version": "2.3.2", "inherits": "CR-PLA @K2 Plus-all", "filament_type": ["PLA"],
                           "filament_vendor": ["Generic"], "compatible_printers": ["Creality K2 Plus 0.4 nozzle"],
                           "filament_settings_id": [name]}).encode()

    # ---- tests ----
    def test_login_and_code_is_single_use(self):
        s = self.login()
        r = self.api("/api/cxy/account/v2/oauthLogin", {"code": s["code"], "clientId": CLIENT_ID, "redirecturi": s["redirect"]})
        self.assertEqual(r["code"], 1000)
        self.assertEqual(self.api("/api/cxy/v2/slice/profile/user/list", {"version": ""}, s)["code"], 0)

    def test_create_then_update(self):
        s = self.login()
        content = self.preset()
        r = self.api("/api/cxy/v2/slice/profile/user/preUploadProfile", {"md5": hashlib.md5(content).hexdigest(), "type": "materia"}, s)
        self.assertEqual(r["code"], 0, r)
        pid = r["result"]["id"]
        self.assertEqual(self.oss(r["result"]["uploadPolicy"], content, "My PLA"), 200)
        r = self.api("/api/cxy/v2/slice/profile/user/editProfile", {"id": pid}, s)
        self.assertEqual(r["code"], 0)
        self.assertEqual(self.oss(r["result"]["uploadPolicy"], self.preset(), "My PLA"), 200)
        st = self.state()
        self.assertEqual(st["violations"], [])
        self.assertEqual(len(st["profiles"]), 1)
        self.assertEqual(st["profiles"][0]["uploads"], 2)
        self.assertEqual(st["profiles"][0]["printer"], "Creality K2 Plus 0.4 nozzle")
        lst = self.api("/api/cxy/v2/slice/profile/user/list", {"version": ""}, s)
        self.assertEqual(lst["result"]["list"][0]["id"], pid)

    def test_edit_unknown_is_517(self):
        s = self.login()
        self.assertEqual(self.api("/api/cxy/v2/slice/profile/user/editProfile", {"id": "nope"}, s)["code"], 517)

    def test_expired_token_is_code_4(self):
        s = self.login()
        req = urllib.request.Request(self.base + "/mock/set", data=b'{"expire_tokens": true}', method="POST")
        urllib.request.urlopen(req, timeout=5).read()
        self.assertEqual(self.api("/api/cxy/v2/slice/profile/user/list", {"version": ""}, s)["code"], 4)

    def test_violations(self):
        s = self.login()
        content = self.preset()
        r = self.api("/api/cxy/v2/slice/profile/user/preUploadProfile", {"md5": "0" * 32, "type": "materia"}, s)
        self.oss(r["result"]["uploadPolicy"], content, "x", {"__CXY_TOKEN_": s["token"]})
        v = self.state()["violations"]
        self.assertTrue(any("md5" in x for x in v), v)
        self.assertTrue(any("storage host" in x for x in v), v)
        r = self.api("/api/cxy/v2/slice/profile/user/preUploadProfile", {"md5": "0" * 32, "type": "printer"}, s)
        self.assertNotEqual(r["code"], 0)


    def mock_set(self, body):
        req = urllib.request.Request(self.base + "/mock/set", data=json.dumps(body).encode(), method="POST")
        urllib.request.urlopen(req, timeout=5).read()

    def test_rate_limit_1088_and_429(self):
        s = self.login()
        md5 = hashlib.md5(self.preset()).hexdigest()
        self.mock_set({"rate_limit": {"mode": "1088", "min_interval_s": 5}})
        self.assertEqual(self.api("/api/cxy/v2/slice/profile/user/preUploadProfile", {"md5": md5, "type": "materia"}, s)["code"], 0)
        self.assertEqual(self.api("/api/cxy/v2/slice/profile/user/preUploadProfile", {"md5": md5, "type": "materia"}, s)["code"], 1088)
        self.mock_set({"rate_limit": None, "refuse_next": 1, "refuse_mode": "429"})
        req = urllib.request.Request(self.base + "/api/cxy/v2/slice/profile/user/editProfile", data=b'{"id": "x"}', headers=headers(s), method="POST")
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=5)
        self.assertEqual(cm.exception.code, 429)
        self.mock_set({"refuse_next": 1, "refuse_mode": "429", "rate_limit": None})
        self.mock_set({"rate_limit": {"mode": "429", "min_interval_s": 0, "retry_after": 7}, "refuse_next": 0})
        self.mock_set({"rate_limit": {"mode": "429", "min_interval_s": 60, "retry_after": 7}})
        req = urllib.request.Request(self.base + "/api/cxy/v2/slice/profile/user/editProfile", data=b'{"id": "x"}', headers=headers(s), method="POST")
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=5)
        self.assertEqual(cm.exception.headers.get("Retry-After"), "7")
        self.assertGreaterEqual(len(self.state()["refused"]), 3)

    def test_oss_429_keeps_profile_for_edit(self):
        s = self.login()
        content = self.preset()
        r = self.api("/api/cxy/v2/slice/profile/user/preUploadProfile", {"md5": hashlib.md5(content).hexdigest(), "type": "materia"}, s)
        self.mock_set({"oss_429_next": 1})
        self.assertEqual(self.oss(r["result"]["uploadPolicy"], content, "x"), 429)
        r2 = self.api("/api/cxy/v2/slice/profile/user/editProfile", {"id": r["result"]["id"]}, s)
        self.assertEqual(self.oss(r2["result"]["uploadPolicy"], content, "x"), 200)
        self.assertEqual(len(self.state()["profiles"]), 1)


if __name__ == "__main__":
    unittest.main()
