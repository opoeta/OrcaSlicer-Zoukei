#!/usr/bin/env python3
"""Mock Snapmaker U1 (Klipper + Moonraker) for Orca Zoukei. Standard library only.

Emulates what the U1 integration reads and writes, as far as it was learnt from the open firmware
(github.com/Snapmaker/u1-klipper: klippy/extras/print_task_config.py, filament_feed.py, lava/fluidd.cfg):

  reads   GET/POST /printer/objects/query  print_task_config, filament_detect, filament_feed left|right,
          print_stats, virtual_sdcard, extruder, extruder1..3, heater_bed, toolhead, gcode_move, webhooks, ...
  writes  POST /printer/gcode/script       SET_PRINT_FILAMENT_CONFIG (official heads locked unless FORCE=1),
          SET_PRINT_EXTRUDER_MAP and SET_PRINT_USED_EXTRUDERS (refused while printing / paused),
          SET_PRINT_PREFERENCES, AUTO_FEEDING EXTRUDER=n LOAD=1|UNLOAD=1 (simulated feed states),
          INNER_FILAMENT_UNLOAD, M104 / M140 / SET_HEATER_TEMPERATURE, SDCARD_PRINT_FILE, PAUSE / RESUME / CANCEL_PRINT
  also    /server/info, /printer/info, /printer/objects/list, /server/files/*, /server/history/*, /websocket
          (JSON-RPC subscribe + notify_status_update), POST /printer/print/*

Every G-code script is recorded (memory, GET /mock/scripts, and the JSON-lines file given with --record).
Nothing here touches a real printer; it binds to 127.0.0.1 only.

  python tools/zoukei/mock_u1_moonraker.py                      # idle, 4 heads, head 0 an official (NFC) spool
  python tools/zoukei/mock_u1_moonraker.py --printing           # a print is running: all writes must be refused
  python tools/zoukei/mock_u1_moonraker.py --official 0,1 --empty 3 --port 7126

Control endpoints (for tests): GET /mock/state, GET /mock/scripts, POST /mock/reset, POST /mock/set
({"print_state": "printing"|"paused"|"standby"|"complete", "official": [0], "empty": [3]}).
"""
import argparse
import base64
import copy
import hashlib
import json
import re
import shlex
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, unquote, urlparse

PHYSICAL = 4
LOGICAL = 32
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# Feed states that mean "the channel is busy" (a subset of filament_feed.py FEED_STA_*).
LOAD_STEPS = ["load_prepare", "load_homing", "load_picking", "load_heating", "load_feeding", "load_extruding", "load_flushing", "load_finish"]
UNLOAD_STEPS = ["unload_prepare", "unload_homing", "unload_picking", "unload_heating", "unload_doing", "unload_finish"]

SAMPLE_HEADS = [  # vendor, type, subtype, rgba
    ("Snapmaker", "PLA", "SnapSpeed", "E03A3AFF"),
    ("Generic", "PETG", "NONE", "2D6CDFFF"),
    ("Polymaker", "PLA", "Silk", "F2C230FF"),
    ("NONE", "NONE", "NONE", "FFFFFFFF"),
]


class Printer:
    """The state of the fake U1; every access goes through the lock."""

    def __init__(self, official=(0,), empty=(3,), printing=False, record=None, fast=False):
        self.lock = threading.RLock()
        self.record_path = record
        self.scripts = []
        self.uploads = []
        self.fast = fast            # feed simulation without delays (tests)
        self.t0 = time.time()
        self.reset(official, empty, printing)
        self.running = True
        threading.Thread(target=self._ticker, daemon=True).start()

    # ---- state ------------------------------------------------------------------------------------

    def reset(self, official=(0,), empty=(3,), printing=False):
        with self.lock:
            self.scripts = []
            self.uploads = []
            ptc = {
                "filament_vendor": [], "filament_type": [], "filament_sub_type": [], "filament_color": [],
                "filament_color_rgba": [], "filament_color_multi": [], "filament_official": [], "filament_sku": [],
                "filament_edit": [], "filament_exist": [], "filament_soft": [],
                "extruder_map_table": list(range(PHYSICAL)) + [0] * (LOGICAL - PHYSICAL),
                "extruders_used": [False] * PHYSICAL, "extruders_replenished": list(range(PHYSICAL)),
                "time_lapse_camera": False, "auto_bed_leveling": False, "flow_calibrate": False,
                "flow_calib_extruders": [True] * PHYSICAL, "shaper_calibrate": False,
                "auto_replenish_filament": True, "replenish_ignore_color": False,
                "filament_entangle_detect": False, "filament_entangle_sen": "medium",
                "end_led_turn_off": False, "end_unload_filament": [False] * PHYSICAL,
                "reprint_info": {},
            }
            for i in range(PHYSICAL):
                vendor, ftype, sub, rgba = SAMPLE_HEADS[i]
                exist = i not in empty
                if not exist:
                    vendor, ftype, sub, rgba = "NONE", "NONE", "NONE", "FFFFFFFF"
                ptc["filament_vendor"].append(vendor)
                ptc["filament_type"].append(ftype)
                ptc["filament_sub_type"].append(sub)
                ptc["filament_color_rgba"].append(rgba)
                ptc["filament_color"].append((int(rgba[6:8], 16) << 24) | int(rgba[0:6], 16))
                ptc["filament_color_multi"].append({"nums": 1, "alpha": int(rgba[6:8], 16), "mode": 0, "colors": [rgba[0:6]]})
                ptc["filament_official"].append(exist and i in official)
                ptc["filament_sku"].append(1001 + i if exist and i in official else 0)
                ptc["filament_exist"].append(exist)
                ptc["filament_soft"].append(False)
                ptc["filament_edit"].append(False)
            self.ptc = ptc
            self._update_edit_flags()
            self.print_state = "printing" if printing else "standby"
            self.filename = "benchy_u1.gcode" if printing else ""
            self.progress = 0.37 if printing else 0.0
            self.active_extruder = "extruder"
            self.nozzle = [25.0] * PHYSICAL
            self.nozzle_target = [0.0] * PHYSICAL
            self.bed = 24.0
            self.bed_target = 0.0
            self.feed = {i: {"state": "load_finish" if self.ptc["filament_exist"][i] else "inited", "action": "none",
                             "error": "ok", "detected": self.ptc["filament_exist"][i], "auto": True} for i in range(PHYSICAL)}
            self.files = {"benchy_u1.gcode": 1843200, "cube.gcode": 52311}

    def _update_edit_flags(self):
        p = self.ptc
        p["filament_edit"] = [bool(p["filament_exist"][i] and not p["filament_official"][i]) for i in range(PHYSICAL)]

    def busy_print(self):
        return self.print_state in ("printing", "paused")

    def _ticker(self):
        while self.running:
            time.sleep(0.5)
            with self.lock:
                for i in range(PHYSICAL):
                    t = self.nozzle_target[i]
                    cur = self.nozzle[i]
                    self.nozzle[i] = cur + max(-8.0, min(8.0, (t if t > 0 else 25.0) - cur))
                self.bed += max(-3.0, min(3.0, (self.bed_target if self.bed_target > 0 else 24.0) - self.bed))
                if self.print_state == "printing":
                    self.progress = min(1.0, self.progress + 0.002)
                    if self.progress >= 1.0:
                        self.print_state = "complete"

    # ---- query -------------------------------------------------------------------------------------

    def object_names(self):
        return ["webhooks", "configfile", "gcode_move", "toolhead", "print_stats", "virtual_sdcard", "display_status", "fan",
                "heater_bed", "extruder", "extruder1", "extruder2", "extruder3", "print_task_config", "filament_detect",
                "filament_feed left", "filament_feed right", "gcode_macro AUTO_FEEDING", "gcode_macro _FILAMENT_FEED_VARIABLE"]

    def _feed_dict(self, i):
        f = self.feed[i]
        return {"module_exist": True, "filament_detected": bool(f["detected"]), "disable_auto": not f["auto"],
                "channel_state": f["state"], "channel_error": f["error"],
                "channel_error_state": f["state"] if f["error"] != "ok" else "none", "channel_action_state": f["action"]}

    def get_object(self, name):
        with self.lock:
            if name == "print_task_config":
                return copy.deepcopy(self.ptc)
            if name == "filament_detect":
                info = []
                for i in range(PHYSICAL):
                    official = self.ptc["filament_official"][i]
                    info.append({"VENDOR": self.ptc["filament_vendor"][i] if official else "NONE",
                                 "MAIN_TYPE": self.ptc["filament_type"][i], "SUB_TYPE": self.ptc["filament_sub_type"][i],
                                 "OFFICIAL": official, "SKU": self.ptc["filament_sku"][i],
                                 "ARGB_COLOR": self.ptc["filament_color"][i],
                                 "BED_TEMP": 60 if official else 0, "FIRST_LAYER_TEMP": 220 if official else 0})
                return {"info": info, "state": ["idle"] * PHYSICAL, "config": {}}
            if name == "filament_feed left":
                return {"extruder0": self._feed_dict(0), "extruder1": self._feed_dict(1)}
            if name == "filament_feed right":
                return {"extruder2": self._feed_dict(2), "extruder3": self._feed_dict(3)}
            if name == "print_stats":
                dur = 600.0 * self.progress if self.print_state in ("printing", "paused") else 0.0
                return {"filename": self.filename, "total_duration": dur, "print_duration": dur, "filament_used": 1200.0 * self.progress,
                        "state": self.print_state, "message": "", "exception": {},
                        "info": {"total_layer": 120, "current_layer": int(120 * self.progress)}}
            if name == "virtual_sdcard":
                return {"file_path": self.filename, "progress": self.progress, "is_active": self.print_state == "printing", "file_position": 0}
            if name == "toolhead":
                return {"extruder": self.active_extruder, "position": [120.0, 120.0, 10.0, 0.0], "homed_axes": "xyz", "print_time": 0.0}
            if name == "gcode_move":
                return {"speed_factor": 1.0, "extrude_factor": 1.0, "absolute_coordinates": True}
            if name == "webhooks":
                return {"state": "ready", "state_message": "Printer is ready"}
            if name == "display_status":
                return {"progress": self.progress, "message": None}
            if name == "fan":
                return {"speed": 0.0, "rpm": None}
            if name == "heater_bed":
                return {"temperature": round(self.bed, 2), "target": self.bed_target, "power": 0.0}
            m = re.match(r"^extruder(\d?)$", name)
            if m:
                i = int(m.group(1) or 0)
                return {"temperature": round(self.nozzle[i], 2), "target": self.nozzle_target[i], "power": 0.0, "extruder_index": i}
            if name == "configfile":
                return {"settings": {"extruder": {"max_temp": 300.0}, "heater_bed": {"max_temp": 120.0}, "printer": {"max_physical_extruder_num": 4}}}
            if name.startswith("gcode_macro"):
                return {}
        return None

    def query(self, wanted):
        status = {}
        for name, fields in wanted.items():
            obj = self.get_object(name)
            if obj is None:
                continue
            if isinstance(fields, list) and fields:
                obj = {k: v for k, v in obj.items() if k in fields}
            status[name] = obj
        return {"eventtime": time.time() - self.t0, "status": status}

    # ---- G-code ------------------------------------------------------------------------------------

    def record(self, script, ok, message):
        entry = {"t": round(time.time() - self.t0, 3), "script": script, "ok": ok, "message": message}
        with self.lock:
            self.scripts.append(entry)
            print("[mock-u1] %s -> %s%s" % (script.replace("\n", " | "), "ok" if ok else "ERROR", "" if ok else " (" + message + ")"), flush=True)
            if self.record_path:
                try:
                    with open(self.record_path, "a", encoding="utf-8") as f:
                        f.write(json.dumps(entry) + "\n")
                except OSError:
                    pass

    def run_script(self, script):
        """Runs every line; returns (ok, message). Stops at the first failing line, like Klipper."""
        for raw in script.replace("\r", "").split("\n"):
            line = raw.split(";", 1)[0].strip()
            if not line:
                continue
            try:
                self._run_line(line)
            except ValueError as e:
                self.record(script, False, str(e))
                return False, str(e)
        self.record(script, True, "")
        return True, ""

    @staticmethod
    def _params(rest):
        out = {}
        for part in shlex.split(rest):
            if "=" in part:
                k, v = part.split("=", 1)
                out[k.upper()] = v
        return out

    def _run_line(self, line):
        cmd, _, rest = line.partition(" ")
        cmd = cmd.upper()
        p = self._params(rest) if rest else {}
        with self.lock:
            if cmd == "SET_PRINT_FILAMENT_CONFIG":
                self._set_filament_config(p)
            elif cmd == "SET_PRINT_EXTRUDER_MAP":
                if self.busy_print():
                    raise ValueError("[print_task_config] not allowed to set extruder map during printing!")
                if "CONFIG_EXTRUDER" not in p or "MAP_EXTRUDER" not in p:
                    raise ValueError("[print_task_config] extruder map, incomplete parameters")
                c, m = int(p["CONFIG_EXTRUDER"]), int(p["MAP_EXTRUDER"])
                if not (0 <= c < LOGICAL and 0 <= m < PHYSICAL):
                    raise ValueError("[print_task_config] extruder map, invalid extruder index!!!")
                self.ptc["extruder_map_table"][c] = m
            elif cmd == "SET_PRINT_USED_EXTRUDERS":
                if self.busy_print():
                    raise ValueError("[print_task_config] not allow to set used_extruders during printing!")
                used = [False] * PHYSICAL
                for v in p.get("EXTRUDERS", "").split(","):
                    if v.strip():
                        used[int(v)] = True
                self.ptc["extruders_used"] = used
            elif cmd == "SET_PRINT_PREFERENCES":
                if self.busy_print():
                    raise ValueError("[print_task_config] not allow to set preferences during printing!")
                for key, val in p.items():
                    if key == "AUTO_REPLENISH_FILAMENT":
                        self.ptc["auto_replenish_filament"] = val == "1"
                    elif key == "BED_LEVEL":
                        self.ptc["auto_bed_leveling"] = val == "1"
                    elif key == "FLOW_CALIBRATE":
                        self.ptc["flow_calibrate"] = val == "1"
                    elif key == "SHAPER_CALIBRATE":
                        self.ptc["shaper_calibrate"] = val == "1"
                    elif key == "TIME_LAPSE_CAMERA":
                        self.ptc["time_lapse_camera"] = val == "1"
            elif cmd == "AUTO_FEEDING":
                self._auto_feeding(p)
            elif cmd == "INNER_FILAMENT_UNLOAD":
                pass  # the macro is part of the unload stage in the real firmware
            elif cmd in ("M104", "M109"):
                t = re.search(r"S([0-9.]+)", rest)
                tool = re.search(r"T(\d)", rest)
                idx = int(tool.group(1)) if tool and int(tool.group(1)) < PHYSICAL else 0
                if t:
                    self.nozzle_target[idx] = float(t.group(1))
            elif cmd in ("M140", "M190"):
                t = re.search(r"S([0-9.]+)", rest)
                if t:
                    self.bed_target = float(t.group(1))
            elif cmd == "SET_HEATER_TEMPERATURE":
                heater, target = p.get("HEATER", ""), float(p.get("TARGET", "0"))
                if heater == "heater_bed":
                    self.bed_target = target
                elif heater.startswith("extruder"):
                    self.nozzle_target[int(heater[8:] or 0)] = target
            elif cmd == "SDCARD_PRINT_FILE":
                if self.busy_print():
                    raise ValueError("Print already in progress")
                self.filename = p.get("FILENAME", "")
                self.print_state, self.progress = "printing", 0.0
            elif cmd == "PAUSE" and self.print_state == "printing":
                self.print_state = "paused"
            elif cmd == "RESUME" and self.print_state == "paused":
                self.print_state = "printing"
            elif cmd in ("CANCEL_PRINT", "CLEAR_PAUSE"):
                self.print_state = "standby"
            elif cmd == "M112" or cmd == "FIRMWARE_RESTART":
                pass
            # anything else (G28, M220, SET_FAN_SPEED ...) is accepted and only recorded

    def _set_filament_config(self, p):
        if "CONFIG_EXTRUDER" not in p:
            raise ValueError("Missing CONFIG_EXTRUDER")
        i = int(p["CONFIG_EXTRUDER"])
        if i < 0 or i >= PHYSICAL:
            raise ValueError("[print_task_config] extruder%d is out of range[0, %d]" % (i, PHYSICAL - 1))
        force = int(p.get("FORCE", "0")) != 0
        if self.ptc["filament_official"][i] and not force:
            raise ValueError("[print_task_config] filament_config, official filament, not configurable!")
        if "FILAMENT_TYPE" in p:
            if "VENDOR" not in p or "FILAMENT_SUBTYPE" not in p:
                raise ValueError("[print_task_config] filament_config, incomplete parameters")
            self.ptc["filament_vendor"][i] = p["VENDOR"]
            self.ptc["filament_type"][i] = p["FILAMENT_TYPE"]
            self.ptc["filament_sub_type"][i] = p["FILAMENT_SUBTYPE"]
            self.ptc["filament_soft"][i] = p.get("SOFT") == "1" or p["FILAMENT_TYPE"].upper().startswith("TPU")
        if "COLOR_NUMS" in p:
            n = int(p["COLOR_NUMS"])
            colors = p.get("COLORS", "").split(",")
            if len(colors) != n or any(not re.fullmatch(r"[0-9A-Fa-f]{6}", c) for c in colors):
                raise ValueError("[print_task_config] filament_config, colors error")
            alpha = int(p["ALPHA"]) if "ALPHA" in p else int(self.ptc["filament_color_rgba"][i][6:8], 16)
            self.ptc["filament_color_multi"][i] = {"nums": n, "alpha": alpha, "mode": int(p.get("MULTI_MODE", "0")), "colors": colors}
            rgba = colors[0].upper() + "%02X" % alpha
            self.ptc["filament_color_rgba"][i] = rgba
            self.ptc["filament_color"][i] = (alpha << 24) | int(colors[0], 16)
        elif "FILAMENT_COLOR_RGBA" in p:
            rgba = p["FILAMENT_COLOR_RGBA"]
            if len(rgba) == 6:
                rgba += "FF"
            if not re.fullmatch(r"[0-9A-Fa-f]{8}", rgba):
                raise ValueError("[print_task_config] filament_config, rgba error")
            rgba = rgba.upper()
            self.ptc["filament_color_multi"][i] = {"nums": 1, "alpha": int(rgba[6:8], 16), "mode": 0, "colors": [rgba[0:6]]}
            self.ptc["filament_color_rgba"][i] = rgba
            self.ptc["filament_color"][i] = (int(rgba[6:8], 16) << 24) | int(rgba[0:6], 16)
        self.ptc["filament_official"][i] = False
        self.ptc["filament_sku"][i] = 0
        self._update_edit_flags()

    def _auto_feeding(self, p):
        if "EXTRUDER" not in p:
            raise ValueError("extruder error: 999!")
        i = int(p["EXTRUDER"])
        if not (0 <= i < PHYSICAL):
            raise ValueError("extruder error: %d!" % i)
        if "AUTO" in p:
            self.feed[i]["auto"] = p["AUTO"] == "1"
            return
        load, unload = p.get("LOAD") == "1", p.get("UNLOAD") == "1"
        if not load and not unload:
            raise ValueError("[feed] nothing to do")
        if self.feed[i]["action"] != "none":
            raise ValueError("[feed] channel busy")
        if load and not self.feed[i]["detected"]:
            raise ValueError("[feed] extruder[%d]: state: wait_insert, error: no_filament!" % i)
        steps, action = (LOAD_STEPS, "load") if load else (UNLOAD_STEPS, "unload")
        self.feed[i]["action"] = action
        threading.Thread(target=self._feed_sim, args=(i, steps, load), daemon=True).start()

    def _feed_sim(self, i, steps, load):
        # the real call blocks until the move is over (minutes); the mock takes ~1 s per step, or none with --fast
        for step in steps:
            with self.lock:
                self.feed[i]["state"] = step
                if step in ("load_heating", "unload_heating"):
                    self.nozzle_target[i] = 230.0
            if not self.fast:
                time.sleep(1.0)
        with self.lock:
            self.feed[i]["action"] = "none"
            self.nozzle_target[i] = 0.0
            if load:
                self.ptc["filament_exist"][i] = True
            else:
                self.feed[i]["detected"] = False
                self.ptc["filament_exist"][i] = False
                self.ptc["filament_vendor"][i] = self.ptc["filament_type"][i] = self.ptc["filament_sub_type"][i] = "NONE"
            self._update_edit_flags()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockU1Moonraker/1.0"
    printer = None  # set by main()

    def log_message(self, fmt, *args):  # quiet; scripts are logged by Printer.record
        pass

    # ---- helpers ------------------------------------------------------------------------------------

    def _send(self, code, obj, raw=None, ctype="application/json"):
        body = raw if raw is not None else json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _error(self, code, message):
        self._send(code, {"error": {"code": code, "message": message}})

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    def _json_body(self):
        try:
            return json.loads(self._body().decode("utf-8") or "{}")
        except ValueError:
            return {}

    # ---- routes --------------------------------------------------------------------------------------

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        url = urlparse(self.path)
        path = unquote(url.path)
        pr = self.printer
        if path == "/websocket" and "upgrade" in (self.headers.get("Connection", "").lower()):
            return self._websocket()
        if path in ("/server/info",):
            return self._send(200, {"result": {"klippy_connected": True, "klippy_state": "ready", "moonraker_version": "v0.9.3-mock",
                                               "machine_name": "Snapmaker U1 (mock)", "hostname": "mock-u1", "components": ["file_manager"]}})
        if path == "/printer/info":
            return self._send(200, {"result": {"state": "ready", "state_message": "Printer is ready", "hostname": "mock-u1",
                                               "software_version": "u1-klipper-mock", "klipper_path": "/home/lava/klipper"}})
        if path == "/printer/objects/list":
            return self._send(200, {"result": {"objects": pr.object_names()}})
        if path == "/printer/objects/query":
            wanted = {}
            for k, v in parse_qsl(url.query, keep_blank_values=True):
                wanted.setdefault(k, [])
                if v:
                    wanted[k] += v.split(",")
            return self._send(200, {"result": pr.query(wanted)})
        if path == "/server/files/list":
            return self._send(200, {"result": [{"path": n, "modified": time.time(), "size": s, "permissions": "rw"} for n, s in pr.files.items()]})
        if path == "/server/files/directory":
            return self._send(200, {"result": {"files": [{"filename": n, "modified": time.time(), "size": s, "permissions": "rw",
                                                          "estimated_time": 5400, "filament_type": json.dumps(["PLA"])} for n, s in pr.files.items()],
                                               "dirs": []}})
        if path == "/server/files/metadata":
            return self._send(200, {"result": {"filename": dict(parse_qsl(url.query)).get("filename", ""), "size": 1843200, "estimated_time": 5400,
                                               "layer_count": 120, "thumbnails": []}})
        if path == "/server/history/list":
            return self._send(200, {"result": {"count": 0, "jobs": []}})
        if path == "/server/history/totals":
            return self._send(200, {"result": {"job_totals": {"total_jobs": 0, "total_print_time": 0, "total_filament_used": 0}}})
        if path == "/server/webcams/list":
            return self._send(200, {"result": {"webcams": []}})
        if path == "/server/database/item":
            return self._send(200, {"result": {"namespace": dict(parse_qsl(url.query)).get("namespace", ""), "key": None, "value": {}}})
        if path == "/mock/state":
            with pr.lock:
                return self._send(200, {"print_state": pr.print_state, "print_task_config": pr.ptc, "feed": pr.feed,
                                        "nozzle": pr.nozzle, "nozzle_target": pr.nozzle_target, "uploads": pr.uploads})
        if path == "/mock/scripts":
            with pr.lock:
                return self._send(200, {"scripts": list(pr.scripts)})
        self._error(404, "Not Found: " + path)

    def do_POST(self):
        url = urlparse(self.path)
        path = unquote(url.path)
        pr = self.printer
        if path == "/printer/objects/query":
            objs = self._json_body().get("objects") or {}
            return self._send(200, {"result": pr.query({k: (v if isinstance(v, list) else []) for k, v in objs.items()})})
        if path == "/printer/gcode/script":
            script = str(self._json_body().get("script", ""))
            ok, msg = pr.run_script(script)
            return self._send(200, {"result": "ok"}) if ok else self._error(400, msg)
        if path in ("/printer/print/start", "/printer/print/pause", "/printer/print/resume", "/printer/print/cancel"):
            q = dict(parse_qsl(url.query))
            line = {"start": "SDCARD_PRINT_FILE FILENAME=%s" % q.get("filename", ""), "pause": "PAUSE", "resume": "RESUME", "cancel": "CANCEL_PRINT"}[path.rsplit("/", 1)[1]]
            ok, msg = pr.run_script(line)
            return self._send(200, {"result": "ok"}) if ok else self._error(400, msg)
        if path == "/server/files/upload":
            data = self._body()
            m = re.search(rb'name="file";\s*filename="([^"]+)"', data[:2048])
            name = m.group(1).decode("utf-8", "replace") if m else "upload.gcode"
            with pr.lock:
                pr.uploads.append({"name": name, "bytes": len(data)})
                pr.files[name] = len(data)
            print("[mock-u1] upload %s (%d bytes)" % (name, len(data)), flush=True)
            return self._send(201, {"result": {"item": {"path": name, "root": "gcodes"}, "print_started": False}})
        if path == "/mock/reset":
            pr.reset()
            return self._send(200, {"result": "ok"})
        if path == "/mock/set":
            body = self._json_body()
            with pr.lock:
                if "print_state" in body:
                    pr.print_state = str(body["print_state"])
                    if pr.print_state in ("printing", "paused") and not pr.filename:
                        pr.filename = "benchy_u1.gcode"
                if "official" in body:
                    pr.ptc["filament_official"] = [i in body["official"] and pr.ptc["filament_exist"][i] for i in range(PHYSICAL)]
                if "empty" in body:
                    for i in range(PHYSICAL):
                        pr.ptc["filament_exist"][i] = i not in body["empty"]
                        pr.feed[i]["detected"] = i not in body["empty"]
                pr._update_edit_flags()
            return self._send(200, {"result": "ok"})
        self._error(404, "Not Found: " + path)

    def do_DELETE(self):
        self._send(200, {"result": {"deleted": True}})

    # ---- websocket (just enough for the agent's status stream and the page's polling) ---------------

    def _websocket(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()
        conn = self.connection
        conn.settimeout(1.5)
        subscribed = {}
        send_lock = threading.Lock()

        def send(obj):
            data = json.dumps(obj).encode("utf-8")
            head = bytes([0x81])
            n = len(data)
            head += bytes([n]) if n < 126 else (bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n))
            with send_lock:
                conn.sendall(head + data)

        def read_frame():
            hdr = self._recv(conn, 2)
            if hdr is None:
                return None
            opcode, ln = hdr[0] & 0x0F, hdr[1] & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._recv(conn, 2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._recv(conn, 8))[0]
            mask = self._recv(conn, 4) if hdr[1] & 0x80 else b""
            payload = self._recv(conn, ln) if ln else b""
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            return opcode, payload

        try:
            while True:
                try:
                    frame = read_frame()
                except socket.timeout:
                    frame = "timeout"
                if frame is None or (frame != "timeout" and frame[0] == 8):
                    break
                if frame == "timeout":
                    if subscribed:
                        send({"jsonrpc": "2.0", "method": "notify_status_update",
                              "params": [self.printer.query({k: [] for k in subscribed})["status"], time.time() - self.printer.t0]})
                    continue
                opcode, payload = frame
                if opcode == 9:  # ping
                    with send_lock:
                        conn.sendall(bytes([0x8A, len(payload)]) + payload)
                    continue
                if opcode != 1:
                    continue
                try:
                    msg = json.loads(payload.decode("utf-8"))
                except ValueError:
                    continue
                method, mid = msg.get("method"), msg.get("id")
                if method == "printer.objects.subscribe":
                    subscribed = {k: v for k, v in (msg.get("params", {}).get("objects") or {}).items()}
                    send({"jsonrpc": "2.0", "result": {"eventtime": time.time() - self.printer.t0,
                                                       "status": self.printer.query({k: [] for k in subscribed})["status"]}, "id": mid})
                elif mid is not None:
                    send({"jsonrpc": "2.0", "result": {} if method != "server.connection.identify" else {"connection_id": 1}, "id": mid})
        except (OSError, struct.error):
            pass
        self.close_connection = True

    @staticmethod
    def _recv(conn, n):
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf


def main():
    ap = argparse.ArgumentParser(description="Mock Snapmaker U1 Moonraker (127.0.0.1 only)")
    ap.add_argument("--port", type=int, default=7125)
    ap.add_argument("--official", default="0", help="comma list of heads that hold an official (NFC, locked) spool; '' for none")
    ap.add_argument("--empty", default="3", help="comma list of empty heads")
    ap.add_argument("--printing", action="store_true", help="start with a print running (writes must be refused)")
    ap.add_argument("--record", default="mock_u1_scripts.jsonl", help="JSON-lines file with every G-code script ('' to disable)")
    ap.add_argument("--fast", action="store_true", help="no delays in the simulated load / unload")
    args = ap.parse_args()
    to_set = lambda s: {int(x) for x in s.split(",") if x.strip()}
    Handler.printer = Printer(official=to_set(args.official), empty=to_set(args.empty), printing=args.printing, record=args.record or None, fast=args.fast)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    print("Mock U1 Moonraker on http://127.0.0.1:%d  (official heads: %s, empty heads: %s, printing: %s)" % (args.port, args.official or "-", args.empty or "-", args.printing), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        Handler.printer.running = False


if __name__ == "__main__":
    main()
