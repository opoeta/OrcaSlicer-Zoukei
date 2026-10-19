#!/usr/bin/env python3
"""Mock Creality Cloud for testing the Orca Zoukei Creality Cloud integration without an account.

    python tools/zoukei/mock_creality_cloud.py [--port 8790]

Standard library only; binds to 127.0.0.1 and never talks to anything else. Start OrcaSlicer with

    ORCA_ZOUKEI_CREALITY_CLOUD_MOCK=http://127.0.0.1:8790

and every Creality Cloud address (login page, API, file storage) points here. Never type a real password into the
mock login page: it has no password field and only simulates the account page.

What it emulates (protocol of Creality Print, AGPL-3.0, github.com/CrealityOfficial/CrealityPrint):
  GET  /id?lang&client_id&app_id&redirect_uri&platform   account page (one "Log in (simulated)" button)
  GET  /id/authorize?...                                 -> 302 redirect_uri&code=<one-time code>
  GET  /oauth?back_url=...&code=...                      -> 302 back_url?code=...&result_url=/result (website step)
  POST /api/cxy/account/v2/oauthLogin {code, clientId, redirecturi}
  POST /api/cxy/v2/slice/profile/user/list {version}
  POST /api/cxy/v2/slice/profile/user/preUploadProfile {md5, type}     type must be "materia"
  POST /api/cxy/v2/slice/profile/user/editProfile {id}                  517 when the id is unknown
  POST /oss                                                             multipart form of the upload policy, field File
Answers {"code": 0, "msg": "ok", "result": ...}; code 4 = token invalid.

Test hooks:
  GET  /mock/state          accounts, profiles (parsed file summary), violations, request log (tokens redacted)
  POST /mock/set {"expire_tokens": true} | {"fail_oss": true} | {"fail_oss": false}
                 {"rate_limit": {"mode": "1088" | "429", "min_interval_s": 2.0, "retry_after": 1}}  refuse profile calls
                     (preUploadProfile / editProfile) closer than min_interval_s to the previous accepted one, like the
                     real "Please do not repeat the same operation many times" (code 1088) or HTTP 429 (+ Retry-After)
                 {"rate_limit": null}             off
                 {"refuse_next": 2, "refuse_mode": "1088" | "429"}   refuse the next N profile calls whatever the pace
                 {"oss_429_next": 1}              the next N storage uploads answer HTTP 429 (resume test)
  POST /mock/reset
Violations recorded: missing __CXY_* headers, the token sent to the storage host, a wrong client id / redirect uri,
a reused or unknown code, an md5 that does not match the file, a file that is not a filament preset.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import threading
import time
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlencode, urlparse

CLIENT_IDS = {"f9c302ecc29c59a0a6e921ff39a073ca", "8ea5010984fa52a298f12110af8b05d0"}
CALLBACK = "http://localhost:13666/login"
MOCK_USER_ID = "100001"
MOCK_NICK = "Mock User"


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        self.codes = {}          # code -> used flag
        self.tokens = {}         # token -> user id
        self.profiles = {}       # id -> dict
        self.pending = {}        # oss key -> {"id", "md5"}
        self.violations = []
        self.log = []
        self.fail_oss = False
        self.next_id = 1
        self.rate_limit = None      # {"mode", "min_interval_s", "retry_after"}
        self.last_profile_call = 0.0
        self.refuse_next = 0
        self.refuse_mode = "1088"
        self.oss_429_next = 0
        self.refused = []           # (time, path, mode) of every refused call

    def violation(self, text):
        self.violations.append(text)


STATE = State()


def _redact(headers):
    out = {}
    for k, v in headers.items():
        out[k] = "<redacted>" if k.lower() == "__cxy_token_" else v
    return out


class Handler(BaseHTTPRequestHandler):
    server_version = "MockCrealityCloud/1.0"
    base = "http://127.0.0.1:8790"

    def log_message(self, fmt, *args):  # quiet; the request log is in /mock/state
        pass

    # ---- helpers ----
    def _send(self, status, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else (json.dumps(body) if ctype == "application/json" else body).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith("text/") else ""))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _ok(self, result):
        self._send(200, {"code": 0, "msg": "ok", "result": result})

    def _err(self, code, msg):
        self._send(200, {"code": code, "msg": msg, "result": None})

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def _json(self):
        try:
            return json.loads(self._body() or b"{}")
        except ValueError:
            return None

    def _record(self):
        with STATE.lock:
            STATE.log.append({"t": time.time(), "method": self.command, "path": urlparse(self.path).path, "headers": _redact(dict(self.headers))})
            del STATE.log[:-200]

    def _check_api_headers(self):
        h = self.headers
        for name, want in (("__CXY_APP_ID_", "creality_model"), ("__CXY_PLATFORM_", "11")):
            if h.get(name) != want:
                STATE.violation(f"{self.path}: header {name} = {h.get(name)!r}, expected {want!r}")
        for name in ("__CXY_DUID_", "__CXY_REQUESTID_", "__CXY_OS_LANG_", "__CXY_APP_VER_"):
            if not h.get(name):
                STATE.violation(f"{self.path}: header {name} missing")

    def _auth(self):
        token = self.headers.get("__CXY_TOKEN_", "")
        uid = self.headers.get("__CXY_UID_", "")
        with STATE.lock:
            if STATE.tokens.get(token) != uid or not uid:
                return None
        return uid

    def _rate_limited(self):
        """True when this profile call is refused (the answer is already sent)."""
        now = time.time()
        with STATE.lock:
            mode = None
            retry_after = None
            if STATE.refuse_next > 0:
                STATE.refuse_next -= 1
                mode = STATE.refuse_mode
            elif STATE.rate_limit:
                rl = STATE.rate_limit
                if now - STATE.last_profile_call < float(rl.get("min_interval_s", 0)):
                    mode = str(rl.get("mode", "1088"))
                    retry_after = rl.get("retry_after")
            if mode is None:
                STATE.last_profile_call = now
                return False
            STATE.refused.append((now, urlparse(self.path).path, mode))
        if mode == "429":
            data = b'{"error":"Too Many Requests"}'
            self.send_response(429)
            self.send_header("Content-Type", "application/json")
            if retry_after is not None:
                self.send_header("Retry-After", str(retry_after))
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self._err(1088, "Please do not repeat the same operation many times. Take a rest and try it later.")
        return True

    def _policy(self, pid, uid):
        key = f"profiles/{uid}/{pid}/{secrets.token_hex(4)}.json"
        return key, {"OSSAccessKeyId": "MOCKACCESSKEY", "Signature": "mock-signature", "Policy": "mock-policy", "Key": key,
                     "Callback": "mock-callback", "Host": self.base + "/oss"}

    # ---- GET ----
    def do_GET(self):
        self._record()
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/id":
            if q.get("client_id") not in CLIENT_IDS:
                STATE.violation(f"login page: unknown client_id {q.get('client_id')!r}")
            if q.get("app_id") != "creality_model":
                STATE.violation(f"login page: app_id {q.get('app_id')!r}")
            redirect = q.get("redirect_uri", "")
            if redirect != self.base + "/oauth?back_url=" + CALLBACK:
                STATE.violation(f"login page: redirect_uri {redirect!r}")
            href = "/id/authorize?" + urlencode({"redirect_uri": redirect})
            page = f"""<!doctype html><html><head><meta charset="utf-8"><title>Mock Creality ID</title></head>
<body style="font-family:sans-serif;margin:40px">
<h2>Mock Creality account page</h2>
<p>Test server only (tools/zoukei/mock_creality_cloud.py). There is no password field: never type a real password here.</p>
<p>lang={q.get('lang', '')} platform={q.get('platform', '')}</p>
<p><a id="login" href="{href}" style="display:inline-block;padding:10px 18px;background:#1fb26b;color:#fff;text-decoration:none;border-radius:4px">
Log in as {MOCK_NICK} (simulated)</a></p></body></html>"""
            return self._send(200, page, "text/html")
        if u.path == "/id/authorize":
            code = "mockcode-" + secrets.token_hex(8)
            with STATE.lock:
                STATE.codes[code] = False
            redirect = q.get("redirect_uri", "")
            sep = "&" if "?" in redirect else "?"
            return self._redirect(redirect + sep + "code=" + quote(code))
        if u.path == "/oauth":
            back = q.get("back_url", "")
            code = q.get("code", "")
            return self._redirect(back + "?" + urlencode({"code": code, "result_url": self.base + "/result"}))
        if u.path == "/result":
            return self._send(200, "<html><body>Mock login finished.</body></html>", "text/html")
        if u.path == "/mock/state":
            with STATE.lock:
                return self._send(200, {
                    "accounts": len(STATE.tokens),
                    "profiles": list(STATE.profiles.values()),
                    "violations": list(STATE.violations),
                    "log": list(STATE.log[-60:]),
                    "fail_oss": STATE.fail_oss,
                    "refused": list(STATE.refused),
                })
        return self._send(404, {"error": "not found"})

    # ---- POST ----
    def do_POST(self):
        self._record()
        u = urlparse(self.path)
        if u.path == "/mock/reset":
            with STATE.lock:
                STATE.reset()
            return self._send(200, {"ok": True})
        if u.path == "/mock/set":
            body = self._json() or {}
            with STATE.lock:
                if body.get("expire_tokens"):
                    STATE.tokens.clear()
                if "fail_oss" in body:
                    STATE.fail_oss = bool(body["fail_oss"])
                if "rate_limit" in body:
                    STATE.rate_limit = body["rate_limit"]
                if "refuse_next" in body:
                    STATE.refuse_next = int(body["refuse_next"])
                    STATE.refuse_mode = str(body.get("refuse_mode", "1088"))
                if "oss_429_next" in body:
                    STATE.oss_429_next = int(body["oss_429_next"])
            return self._send(200, {"ok": True})
        if u.path == "/oss":
            return self._oss()
        if not u.path.startswith("/api/"):
            return self._send(404, {"error": "not found"})

        self._check_api_headers()
        body = self._json()
        if body is None:
            return self._err(1, "invalid json")

        if u.path == "/api/cxy/account/v2/oauthLogin":
            code = body.get("code", "")
            if body.get("clientId") not in CLIENT_IDS:
                STATE.violation(f"oauthLogin: clientId {body.get('clientId')!r}")
                return self._err(2, "invalid client")
            if body.get("redirecturi") != self.base + "/oauth?back_url=" + CALLBACK:
                STATE.violation(f"oauthLogin: redirecturi {body.get('redirecturi')!r}")
                return self._err(2, "invalid redirect uri")
            if self.headers.get("__CXY_TOKEN_"):
                STATE.violation("oauthLogin: a token header was sent before login")
            with STATE.lock:
                if code not in STATE.codes or STATE.codes[code]:
                    STATE.violation(f"oauthLogin: unknown or reused code")
                    return self._err(1000, "invalid code")
                STATE.codes[code] = True
                token = "mocktoken-" + secrets.token_hex(16)
                STATE.tokens[token] = MOCK_USER_ID
            return self._ok({"token": token, "userId": MOCK_USER_ID, "user_info": {"nickName": MOCK_NICK, "avatar": ""}})

        uid = self._auth()
        if uid is None:
            return self._err(4, "token invalid")

        if u.path == "/api/cxy/v2/slice/profile/user/list":
            with STATE.lock:
                items = [{"id": p["id"], "type": "materia", "lastModifyTime": p["time"], "printerIntName": p.get("printer", ""),
                          "nozzleDiameter": [], "version": "", "zipUrl": ""} for p in STATE.profiles.values() if p["uid"] == uid]
            return self._ok({"list": items, "count": len(items)})

        if u.path in ("/api/cxy/v2/slice/profile/user/preUploadProfile", "/api/cxy/v2/slice/profile/user/editProfile") and self._rate_limited():
            return

        if u.path == "/api/cxy/v2/slice/profile/user/preUploadProfile":
            md5 = str(body.get("md5", ""))
            if body.get("type") != "materia":
                STATE.violation(f"preUploadProfile: type {body.get('type')!r}")
                return self._err(3, "bad type")
            if len(md5) != 32 or any(c not in "0123456789abcdef" for c in md5):
                STATE.violation(f"preUploadProfile: md5 {md5!r}")
                return self._err(3, "bad md5")
            with STATE.lock:
                pid = f"mockprofile{STATE.next_id}"
                STATE.next_id += 1
                STATE.profiles[pid] = {"id": pid, "uid": uid, "time": int(time.time() * 1000), "uploaded": False, "uploads": 0}
                key, policy = self._policy(pid, uid)
                STATE.pending[key] = {"id": pid, "md5": md5}
            return self._ok({"id": pid, "lastModifyTime": int(time.time() * 1000), "uploadPolicy": policy})

        if u.path == "/api/cxy/v2/slice/profile/user/editProfile":
            pid = str(body.get("id", ""))
            with STATE.lock:
                p = STATE.profiles.get(pid)
                if p is None or p["uid"] != uid:
                    return self._err(517, "data not exist")
                key, policy = self._policy(pid, uid)
                STATE.pending[key] = {"id": pid, "md5": None}
            return self._ok({"id": pid, "lastModifyTime": int(time.time() * 1000), "uploadPolicy": policy})

        return self._err(404, "unknown api")

    def _oss(self):
        if self.headers.get("__CXY_TOKEN_") or self.headers.get("__CXY_UID_"):
            STATE.violation("oss: Creality token / user id headers were sent to the storage host")
        ctype = self.headers.get("Content-Type", "")
        raw = self._body()
        if STATE.fail_oss:
            return self._send(403, "<Error><Code>AccessDenied</Code></Error>", "application/xml")
        with STATE.lock:
            oss_429 = STATE.oss_429_next > 0
            if oss_429:
                STATE.oss_429_next -= 1
                STATE.refused.append((time.time(), "/oss", "429"))
        if oss_429:
            # the key stays pending: the client must come back through editProfile (a new key), not create again
            return self._send(429, "<Error><Code>SlowDown</Code></Error>", "application/xml")
        if not ctype.startswith("multipart/form-data"):
            STATE.violation(f"oss: content type {ctype!r}")
            return self._send(400, "<Error>not multipart</Error>", "application/xml")
        msg = BytesParser(policy=HTTP).parsebytes(b"Content-Type: " + ctype.encode() + b"\r\n\r\n" + raw)
        fields, file_name, file_data = {}, None, None
        for part in msg.iter_parts():
            name = part.get_param("name", header="content-disposition")
            fname = part.get_param("filename", header="content-disposition")
            data = part.get_payload(decode=True) or b""
            if name == "File":
                file_name, file_data = fname, data
            else:
                fields[name] = data.decode("utf-8", "replace")
        for f in ("OSSAccessKeyId", "Signature", "Policy", "Key", "Callback"):
            if not fields.get(f):
                STATE.violation(f"oss: form field {f} missing")
        with STATE.lock:
            pend = STATE.pending.pop(fields.get("Key", ""), None)
        if pend is None:
            STATE.violation("oss: unknown Key")
            return self._send(403, "<Error>unknown key</Error>", "application/xml")
        if file_data is None:
            STATE.violation("oss: no File part")
            return self._send(400, "<Error>no file</Error>", "application/xml")
        md5 = hashlib.md5(file_data).hexdigest()
        if pend["md5"] is not None and pend["md5"] != md5:
            STATE.violation(f"oss: md5 of the file {md5} != preUploadProfile md5 {pend['md5']}")
        summary = {}
        try:
            preset = json.loads(file_data.decode("utf-8"))
            summary = {
                "name": preset.get("name"),
                "from": preset.get("from"),
                "inherits": preset.get("inherits"),
                "filament_type": preset.get("filament_type"),
                "filament_vendor": preset.get("filament_vendor"),
                "compatible_printers": preset.get("compatible_printers"),
                "filament_settings_id": preset.get("filament_settings_id"),
                "keys": len(preset),
            }
            if preset.get("from") != "User":
                STATE.violation(f"oss: preset 'from' = {preset.get('from')!r}")
            if not preset.get("filament_type"):
                STATE.violation("oss: the file is not a filament preset (no filament_type)")
            if not preset.get("compatible_printers"):
                STATE.violation("oss: the preset is not linked to a printer (compatible_printers empty)")
        except ValueError:
            STATE.violation("oss: the file is not JSON")
        with STATE.lock:
            p = STATE.profiles.get(pend["id"])
            if p is not None:
                p.update({"uploaded": True, "uploads": p["uploads"] + 1, "file_name": file_name, "md5": md5, "bytes": len(file_data),
                          "time": int(time.time() * 1000), "preset": summary,
                          "printer": (summary.get("compatible_printers") or [""])[0]})
        return self._send(200, {"Status": "OK", "id": pend["id"]})


def make_server(port: int) -> ThreadingHTTPServer:
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    Handler.base = "http://127.0.0.1:%d" % srv.server_address[1]
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8790)
    args = ap.parse_args()
    srv = make_server(args.port)
    print(f"Mock Creality Cloud on {Handler.base}  (set ORCA_ZOUKEI_CREALITY_CLOUD_MOCK={Handler.base})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
