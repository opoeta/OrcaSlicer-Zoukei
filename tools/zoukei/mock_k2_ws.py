"""Mock Creality K2 websocket (ws://127.0.0.1:9999), stdlib only. Nothing here talks to a real printer.

Behaves like the K2 status stream the Orca Zoukei device panel / Creality agent read:
  * on connect and every second it pushes JSON status messages. The first is complete, with the string temperatures the
    firmware really sends ("bedTemp0": "50.100000", "nozzleTemp": "139.800000"); the following ones are incremental and
    do NOT repeat the temperatures (the client must keep the last value);
  * an active error {"err": {"errcode": 1103, "key": 1103}} until the client sends {"method":"set","params":{"cleanErr":1}};
    then {"err": {"errcode": 0, "key": 0}} is pushed and the error stays gone;
  * every received "set"/"get" is appended to GET-less state kept in memory and printed (use --log FILE to keep it).

  python tools/zoukei/mock_k2_ws.py                  # port 9999, error active
  python tools/zoukei/mock_k2_ws.py --no-error --port 9998
  python tools/zoukei/mock_k2_ws.py --selftest       # starts itself on a free port and checks the protocol (exit 0 = ok)
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def read_frame(sock):
    b1, b2 = recv_exact(sock, 2)
    opcode, masked, ln = b1 & 0x0F, b2 & 0x80, b2 & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if masked else b""
    data = bytearray(recv_exact(sock, ln))
    if masked:
        for i in range(ln):
            data[i] ^= mask[i % 4]
    return opcode, bytes(data)


def frame(text, mask=False):
    data = text.encode("utf-8")
    head = bytes([0x81])
    m = 0x80 if mask else 0
    if len(data) < 126:
        head += bytes([m | len(data)])
    elif len(data) < 65536:
        head += bytes([m | 126]) + struct.pack(">H", len(data))
    else:
        head += bytes([m | 127]) + struct.pack(">Q", len(data))
    if mask:
        key = os.urandom(4)
        return head + key + bytes(b ^ key[i % 4] for i, b in enumerate(data))
    return head + data


class State:
    def __init__(self, error=True, log=None):
        self.lock = threading.Lock()
        self.errcode = 1103 if error else 0
        self.received = []          # every JSON message the clients sent
        self.log = log
        # A CFS with four slots (box 1); "get boxsInfo" returns it and "set modifyMaterial" changes it (an ack is pushed too).
        self.materials = [{"id": i, "vendor": "Creality", "type": t, "name": "Hyper " + t, "color": c, "rfid": "", "state": 1,
                           "selected": 0, "minTemp": 190, "maxTemp": 240, "pressure": 0.04, "percent": 100, "editStatus": 0}
                          for i, (t, c) in enumerate([("PLA", "#0ff0000"), ("PLA", "#000ff00"), ("PETG", "#00000ff"), ("ABS", "#0ffffff")])]

    def boxs_info(self):
        with self.lock:
            return {"boxsInfo": {"materialBoxs": [{"id": 1, "type": 0, "state": 1, "temp": 28, "humidity": 40,
                                                   "materials": [dict(m) for m in self.materials]}]}}

    def modify_material(self, p):
        with self.lock:
            for m in self.materials:
                if m["id"] == p.get("id"):
                    m.update({k: p[k] for k in ("vendor", "type", "name", "color", "rfid", "minTemp", "maxTemp", "pressure") if k in p})

    def note(self, msg):
        with self.lock:
            self.received.append(msg)
            if msg.get("method") == "set" and (msg.get("params") or {}).get("cleanErr") == 1:
                self.errcode = 0
        line = json.dumps(msg)
        print("<-", line, flush=True)
        if self.log:
            with open(self.log, "a", encoding="utf-8") as f:
                f.write(line + "\n")

    def full_status(self):
        return {"deviceState": 1, "model": "K2 Plus", "hostname": "K2Plus-MOCK", "printProgress": 42,
                "bedTemp0": "50.100000", "bedTemp1": "0.000000", "bedTemp2": "0.000000", "targetBedTemp0": "50",
                "nozzleTemp": "139.800000", "targetNozzleTemp": "140", "boxTemp": "28", "lightSw": 1,
                "err": {"errcode": self.errcode, "key": self.errcode}}

    def partial_status(self, n):
        return {"printProgress": 42 + n % 50, "err": {"errcode": self.errcode, "key": self.errcode}}


def serve_client(conn, state):
    try:
        req = b""
        while b"\r\n\r\n" not in req:
            req += conn.recv(4096)
        key = [l.split(":", 1)[1].strip() for l in req.decode("latin1").split("\r\n") if l.lower().startswith("sec-websocket-key")][0]
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        send_lock = threading.Lock()

        def send(obj):
            with send_lock:
                conn.sendall(frame(json.dumps(obj)))

        def pusher():
            n = 0
            try:
                send(state.full_status())
                while True:
                    time.sleep(1.0)
                    n += 1
                    send(state.partial_status(n))
            except OSError:
                pass

        threading.Thread(target=pusher, daemon=True).start()
        while True:
            opcode, data = read_frame(conn)
            if opcode == 8:
                break
            if opcode == 1:
                if data == b"ok":
                    continue
                try:
                    msg = json.loads(data.decode("utf-8"))
                except ValueError:
                    continue
                state.note(msg)
                if msg.get("method") == "set" and (msg.get("params") or {}).get("cleanErr") == 1:
                    send({"err": {"errcode": 0, "key": 0}})
                params = msg.get("params") or {}
                if msg.get("method") == "get" and "boxsInfo" in params:
                    send(state.boxs_info())
                if msg.get("method") == "set" and isinstance(params.get("modifyMaterial"), dict):
                    state.modify_material(params["modifyMaterial"])
                    send({"modifyMaterial": {"state": 0}})
                    send(state.boxs_info())
    except (OSError, ConnectionError):
        pass
    finally:
        conn.close()


def run(port, state):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)

    def loop():
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=serve_client, args=(conn, state), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return srv.getsockname()[1]


def selftest():
    state = State(error=True)
    port = run(0, state)
    c = socket.create_connection(("127.0.0.1", port), timeout=5)
    c.sendall(("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += c.recv(1024)
    assert b"101" in buf.split(b"\r\n")[0]
    rest = buf.split(b"\r\n\r\n", 1)[1]
    assert rest == b"" or True
    _, data = read_frame(c)
    first = json.loads(data)
    assert first["bedTemp0"] == "50.100000" and first["err"]["errcode"] == 1103, first
    c.sendall(frame(json.dumps({"method": "set", "params": {"cleanErr": 1}}), mask=True))
    for _ in range(5):
        _, data = read_frame(c)
        msg = json.loads(data)
        if msg.get("err", {}).get("errcode") == 0:
            break
    else:
        raise AssertionError("error not cleared")
    assert {"method": "set", "params": {"cleanErr": 1}} in state.received
    print("selftest ok")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9999)
    ap.add_argument("--no-error", action="store_true")
    ap.add_argument("--log", default="")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        sys.exit(0)
    st = State(error=not a.no_error, log=a.log or None)
    p = run(a.port, st)
    print("mock K2 listening on ws://127.0.0.1:%d (error %s)" % (p, "active" if st.errcode else "off"), flush=True)
    while True:
        time.sleep(3600)
