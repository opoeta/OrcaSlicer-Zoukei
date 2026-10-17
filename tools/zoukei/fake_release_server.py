#!/usr/bin/env python3
"""Local fake of the GitHub Releases API + release assets, to test the Orca Zoukei updater without GitHub.

    python tools/zoukei/fake_release_server.py --port 8099 dist/release-2026.10.18 [dist/release-2026.10.19 ...]
    set ORCA_ZOUKEI_UPDATE_URL=http://127.0.0.1:8099/releases.json      (then start Orca with a temporary --datadir!)

Each directory is an output directory of make_release.py. Supports HTTP Range like GitHub's asset CDN.
  --corrupt NAME   flips one byte of that asset when served (the client must reject it)
  --prerelease V   marks version V as a pre-release
Binds to 127.0.0.1 only.
"""
import argparse
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def load_releases(dirs, prereleases, port):
    releases = []
    for d in dirs:
        mpath = [f for f in os.listdir(d) if f.startswith("manifest-") and f.endswith(".json")]
        if not mpath:
            raise SystemExit("no manifest-*.json in %s" % d)
        with open(os.path.join(d, mpath[0]), encoding="utf-8") as f:
            version = json.load(f)["version"]
        notes_path = os.path.join(d, "notes.md")
        notes = open(notes_path, encoding="utf-8").read() if os.path.exists(notes_path) else "Fake release %s\n\n- test notes" % version
        assets = []
        for name in sorted(os.listdir(d)):
            p = os.path.join(d, name)
            if os.path.isfile(p) and name != "notes.md":
                assets.append({"name": name, "size": os.path.getsize(p),
                               "browser_download_url": "http://127.0.0.1:%d/assets/%s/%s" % (port, version, name)})
        releases.append({"tag_name": "v" + version, "draft": False, "prerelease": version in prereleases,
                         "body": notes, "html_url": "http://127.0.0.1:%d/release/%s" % (port, version),
                         "assets": assets, "_dir": d, "_version": version})
    return releases


def make_handler(releases, corrupt):
    by_path = {}
    for r in releases:
        for a in r["assets"]:
            by_path["/assets/%s/%s" % (r["_version"], a["name"])] = os.path.join(r["_dir"], a["name"])

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            sys.stderr.write("fake-release: " + fmt % args + "\n")

        def do_GET(self):
            if self.path.startswith("/releases.json"):
                body = json.dumps([{k: v for k, v in r.items() if not k.startswith("_")} for r in releases]).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            path = by_path.get(self.path)
            if not path:
                self.send_error(404)
                return
            data = open(path, "rb").read()
            if os.path.basename(path) in corrupt and len(data) > 200:
                data = data[:100] + bytes([data[100] ^ 0xFF]) + data[101:]
            rng = self.headers.get("Range")
            start, end, status = 0, len(data) - 1, 200
            if rng:
                m = re.match(r"bytes=(\d+)-(\d*)$", rng)
                if m:
                    start = int(m.group(1))
                    end = min(int(m.group(2)), len(data) - 1) if m.group(2) else len(data) - 1
                    status = 206
            chunk = data[start:end + 1]
            self.send_response(status)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(chunk)))
            self.send_header("Accept-Ranges", "bytes")
            if status == 206:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(data)))
            self.end_headers()
            self.wfile.write(chunk)

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--corrupt", action="append", default=[])
    ap.add_argument("--prerelease", action="append", default=[])
    a = ap.parse_args()
    releases = load_releases(a.dirs, set(a.prerelease), a.port)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(releases, set(a.corrupt)))
    print("serving %d release(s) at http://127.0.0.1:%d/releases.json" % (len(releases), a.port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
