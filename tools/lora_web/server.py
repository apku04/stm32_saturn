#!/usr/bin/env python3
"""
Saturn LoRa Web Dashboard

Reads beacon packets from a connected receiver over a serial port,
stores them in a SQLite database, and serves a browser dashboard with
zoomable time-series graphs (temperature, humidity, RSSI, SNR, battery,
INA219 current/power, etc.) per source MAC address.

Usage:
    python3 server.py                           # auto-detect port, http on :8080
    python3 server.py --port /dev/ttyACM0
    python3 server.py --http-port 8000 --db /tmp/beacons.db
    python3 server.py --no-serial               # read-only, just serve the UI

Requires: pyserial (`pip install pyserial`).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sqlite3
import sys
import threading
import time
import zlib
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


# Port where dev_web (Saturn Dev Dashboard) is listening on the same host.
# Native Dev/Flash tab in the UI proxies through /api/dev/* to this backend.
DEV_WEB_HOST = "127.0.0.1"
DEV_WEB_PORT = 8089

# Repo root (…/stm32) relative to this file (tools/lora_web/server.py), used to
# discover firmware .bin images for OTA.
REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# OTA-over-LoRa transfer parameters (must satisfy the firmware receive path:
# fragments are 8-byte aligned and <= 40 bytes — see ota_rx_dispatch in main.c).
OTA_CHUNK = 192   # 8-byte aligned; fits firmware Packet.data[DATA_LEN]/relay cmd buffer
OTA_MAX_IMAGE = 62 * 1024
OTA_PREPARE_TIMEOUT = 35.0   # includes ~31 pages * ~100ms erase time
OTA_DATA_TIMEOUT = 8.0   # must exceed worst-case MAC CSMA back-off (~2.2s each
                         # way) so a backed-off chunk/ACK is not seen as lost
OTA_END_TIMEOUT = 10.0
OTA_MAX_RETRY = 10

# OTA op codes in ACK packets (must match firmware OTA_OP_* defines)
OTA_OP_PREPARE = 0
OTA_OP_BEGIN   = 1
OTA_OP_DATA    = 2
OTA_OP_END     = 3

# PREPARE NACK reason codes (must match firmware OTA_PREPARE_* defines)
OTA_PREPARE_OK         = 0
OTA_PREPARE_SAME_VER   = 1
OTA_PREPARE_TOO_LARGE  = 2
OTA_PREPARE_BUSY       = 3
OTA_PREPARE_ERASE_FAIL = 4


def _extract_build_id(data: bytes) -> str:
    """Scan firmware binary for the SATVER magic tag written by version.c."""
    m = re.search(rb'\x00SATVER:([^\x00]{1,16})', data)
    return m.group(1).decode('ascii', errors='replace') if m else 'unknown'


def discover_firmwares():
    """Auto-discovered <app>.bin images under firmware/app/*/ and */build/."""
    out = []
    base = os.path.join(REPO_ROOT, "firmware", "app")
    if not os.path.isdir(base):
        return out
    for app in sorted(os.listdir(base)):
        appdir = os.path.join(base, app)
        if not os.path.isdir(appdir):
            continue
        for cand in (os.path.join(appdir, f"{app}.bin"),
                     os.path.join(appdir, "build", f"{app}.bin")):
            if os.path.isfile(cand):
                st = os.stat(cand)
                out.append({
                    "name": os.path.relpath(cand, base),
                    "path": cand,
                    "size": st.st_size,
                    "mtime": int(st.st_mtime),
                })
    return out

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None  # serial mode optional

# ---------------------------------------------------------------- Parsing
# Mirrors lora_monitor.py — keep in sync if firmware format changes.

RX_PATTERN_V2 = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+prssi=(-?\d+)\s+"
    r"snr=(-?\d+)\s+sf=(\d+)\s+freq=(\d+)\s+type=(\d+)\s+seq=(\d+)\s+len=(\d+)"
)
RX_PATTERN = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+prssi=(-?\d+)\s+"
    r"type=(\d+)\s+seq=(\d+)\s+len=(\d+)"
)
RX_PATTERN_LEGACY = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+type=(\d+)\s+len=(\d+)"
)

BEACON_PATTERN_V7 = re.compile(
    r"\[BEACON\]\s+i_ma=(-?\d+)\s+bus=(\d+)\s+bat=(\d+)\s+chg=(\d+)\s+"
    r"tx_pwr=(\d+)\s+sf=(\d+)\s+lat=(-?\d+)\s+lon=(-?\d+)\s+fix=(\d+)\s+"
    r"temp_cdeg=(-?\d+)\s+hum_cpct=(\d+)\s+"
    r"rst=0x([0-9A-Fa-f]+)\s+boot=(\d+)\s+last_stage=(\d+)\s+up_min=(\d+)\s+"
    r"entries=(\d+)"
)
BEACON_PATTERN_V6 = re.compile(
    r"\[BEACON\]\s+i_ma=(-?\d+)\s+bus=(\d+)\s+bat=(\d+)\s+chg=(\d+)\s+"
    r"tx_pwr=(\d+)\s+sf=(\d+)\s+lat=(-?\d+)\s+lon=(-?\d+)\s+fix=(\d+)\s+"
    r"temp_cdeg=(-?\d+)\s+hum_cpct=(\d+)\s+entries=(\d+)"
)
BEACON_PATTERN_V5 = re.compile(
    r"\[BEACON\]\s+i_ma=(-?\d+)\s+bus=(\d+)\s+bat=(\d+)\s+chg=(\d+)\s+"
    r"tx_pwr=(\d+)\s+sf=(\d+)\s+lat=(-?\d+)\s+lon=(-?\d+)\s+fix=(\d+)\s+entries=(\d+)"
)
BEACON_PATTERN_V4 = re.compile(
    r"\[BEACON\]\s+i_ma=(-?\d+)\s+bus=(\d+)\s+bat=(\d+)\s+chg=(\d+)\s+"
    r"tx_pwr=(\d+)\s+sf=(\d+)\s+entries=(\d+)"
)
BEACON_PATTERN_V3 = re.compile(
    r"\[BEACON\]\s+i_ma=(-?\d+)\s+bus=(\d+)\s+bat=(\d+)\s+chg=(\d+)\s+entries=(\d+)"
)

CHARGE_STATUS = {0: "Off", 1: "Charging", 2: "Done", 3: "Fault"}


def parse_beacon(line: str):
    """Return dict of normalized beacon fields (or None if no match)."""
    m = BEACON_PATTERN_V7.match(line)
    if m:
        g = m.groups()
        return {
            "i_ma": int(g[0]), "bus_mv": int(g[1]), "bat_mv": int(g[2]),
            "chg": int(g[3]), "tx_pwr": int(g[4]), "sf": int(g[5]),
            "lat_udeg": int(g[6]), "lon_udeg": int(g[7]), "fix": int(g[8]),
            "temp_cdeg": int(g[9]), "hum_cpct": int(g[10]),
            "rst": int(g[11], 16), "boot": int(g[12]),
            "last_stage": int(g[13]), "up_min": int(g[14]),
            "entries": int(g[15]),
        }
    m = BEACON_PATTERN_V6.match(line)
    if m:
        g = m.groups()
        return {
            "i_ma": int(g[0]), "bus_mv": int(g[1]), "bat_mv": int(g[2]),
            "chg": int(g[3]), "tx_pwr": int(g[4]), "sf": int(g[5]),
            "lat_udeg": int(g[6]), "lon_udeg": int(g[7]), "fix": int(g[8]),
            "temp_cdeg": int(g[9]), "hum_cpct": int(g[10]),
            "entries": int(g[11]),
        }
    m = BEACON_PATTERN_V5.match(line)
    if m:
        g = m.groups()
        return {
            "i_ma": int(g[0]), "bus_mv": int(g[1]), "bat_mv": int(g[2]),
            "chg": int(g[3]), "tx_pwr": int(g[4]), "sf": int(g[5]),
            "lat_udeg": int(g[6]), "lon_udeg": int(g[7]), "fix": int(g[8]),
            "entries": int(g[9]),
        }
    m = BEACON_PATTERN_V4.match(line)
    if m:
        g = m.groups()
        return {
            "i_ma": int(g[0]), "bus_mv": int(g[1]), "bat_mv": int(g[2]),
            "chg": int(g[3]), "tx_pwr": int(g[4]), "sf": int(g[5]),
            "entries": int(g[6]),
        }
    m = BEACON_PATTERN_V3.match(line)
    if m:
        g = m.groups()
        return {
            "i_ma": int(g[0]), "bus_mv": int(g[1]), "bat_mv": int(g[2]),
            "chg": int(g[3]), "entries": int(g[4]),
        }
    return None


def parse_rx(line: str):
    """Return dict of RX header fields (or None)."""
    m = RX_PATTERN_V2.match(line)
    if m:
        g = m.groups()
        return {
            "src": int(g[0]), "dst": int(g[1]),
            "rssi": int(g[2]), "prssi": int(g[3]),
            "snr": int(g[4]), "sf": int(g[5]), "freq": int(g[6]),
            "type": int(g[7]), "seq": int(g[8]), "len": int(g[9]),
        }
    m = RX_PATTERN.match(line)
    if m:
        g = m.groups()
        return {
            "src": int(g[0]), "dst": int(g[1]),
            "rssi": int(g[2]), "prssi": int(g[3]),
            "snr": None, "sf": None, "freq": None,
            "type": int(g[4]), "seq": int(g[5]), "len": int(g[6]),
        }
    m = RX_PATTERN_LEGACY.match(line)
    if m:
        g = m.groups()
        return {
            "src": int(g[0]), "dst": int(g[1]),
            "rssi": int(g[2]), "prssi": None,
            "snr": None, "sf": None, "freq": None,
            "type": int(g[3]), "seq": None, "len": int(g[4]),
        }
    return None


# Routing table dump (`get routing`) — emitted by the connected gateway node.
#   Header:  "Routing table (3 entries):"
#   Rows:    "  [0] dest=45 next=161 hops=2 age=1"
# Interleaving beacon prints can strip the leading "[i]" so we match anywhere.
ROUTING_HDR_RE = re.compile(r"Routing table \((\d+) entries\)")
ROUTING_ROW_RE = re.compile(
    r"dest=(\d+)\s+next=(\d+)\s+hops=(\d+)(?:\s+age=(\d+))?", re.I)


def parse_routing_row(line: str):
    """Return {dest,next_hop,hops,age} for one routing-table row (or None)."""
    m = ROUTING_ROW_RE.search(line)
    if not m:
        return None
    g = m.groups()
    return {
        "dest": int(g[0]),
        "next_hop": int(g[1]),
        "hops": int(g[2]),
        "age": int(g[3]) if g[3] is not None else None,
    }


# Piggybacked routing table carried inside a remote node's beacon and printed
# by the gateway firmware, e.g.
#   "[BTBL] src=45 n=3 3>3:1 44>44:1 161>161:1"
# Each entry is "dest>next_hop:hops" (no age — beacons don't carry it).
BTBL_HDR_RE = re.compile(r"\[BTBL\]\s+src=(\d+)\s+n=(\d+)")
BTBL_ENTRY_RE = re.compile(r"(\d+)>(\d+):(\d+)")
CMD_ACK_BEACON_RE = re.compile(
    r"\[CMD_ACK\]\s+from=(\d+)\s+op=16\s+status=(\d+)\s+target=(\d+)"
    r"\s+persist=(\d+)\s+beacon_interval=(\d+)", re.I)
# OTA_ACK emitted by the gateway when it hears a target's ack (see main.c).
OTA_ACK_RE = re.compile(
    r"\[OTA_ACK\]\s+from=(\d+)\s+op=(\d+)\s+status=(\d+)\s+off=(\d+)", re.I)


def parse_ota_ack(line: str):
    m = OTA_ACK_RE.search(line)
    if not m:
        return None
    return {"from": int(m.group(1)), "op": int(m.group(2)),
            "status": int(m.group(3)), "off": int(m.group(4)),
            "ts": int(time.time())}


def parse_btbl(line: str):
    """Return (src, [{dest,next_hop,hops}, ...]) for a [BTBL] line, or None."""
    h = BTBL_HDR_RE.search(line)
    if not h:
        return None
    src = int(h.group(1))
    routes = [
        {"dest": int(d), "next_hop": int(n), "hops": int(hp)}
        for d, n, hp in BTBL_ENTRY_RE.findall(line[h.end():])
    ]
    return src, routes


def parse_cmd_ack_beacon(line: str):
    m = CMD_ACK_BEACON_RE.search(line)
    if not m:
        return None
    return {
        "from": int(m.group(1)),
        "status": int(m.group(2)),
        "target": int(m.group(3)),
        "persist": int(m.group(4)),
        "interval": int(m.group(5)),
        "ts": int(time.time()),
    }


# ---------------------------------------------------------------- Storage
SCHEMA = """
CREATE TABLE IF NOT EXISTS beacons (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        INTEGER NOT NULL,            -- unix epoch seconds (UTC)
    src       INTEGER NOT NULL,            -- source MAC address (8-bit)
    rssi      INTEGER,
    prssi     INTEGER,
    snr       INTEGER,
    sf        INTEGER,
    tx_pwr    INTEGER,
    bus_mv    INTEGER,                     -- INA219 bus voltage (mV)
    i_ma      INTEGER,                     -- INA219 current (mA, signed)
    bat_mv    INTEGER,                     -- battery voltage (mV)
    chg       INTEGER,                     -- charge status 0..3
    temp_cdeg INTEGER,                     -- temperature * 100 (°C)
    hum_cpct  INTEGER,                     -- humidity * 100 (%RH)
    lat_udeg  INTEGER,
    lon_udeg  INTEGER,
    fix       INTEGER,
    entries   INTEGER
);
CREATE INDEX IF NOT EXISTS idx_beacons_src_ts ON beacons (src, ts);
CREATE INDEX IF NOT EXISTS idx_beacons_ts     ON beacons (ts);
"""


class Store:
    def __init__(self, path: str):
        self.path = path
        self._lock = threading.Lock()
        # Identity of the locally-connected board (the gateway/RX). It is
        # detected from beacon self-prints, which the firmware emits with
        # rssi=0 prssi=0 snr=0 because a node cannot measure its own TX.
        self._device_mac = None
        self._device_mac_ts = None
        self._device_port = None
        # Shared serial handle owned by the reader thread. The HTTP layer
        # writes commands through this SAME handle so only ONE process/owner
        # ever opens the port (no contention with a second serial instance).
        self._serial = None
        self._serial_lock = threading.Lock()
        # Console ring buffer: every raw line read from the device is appended
        # here with a monotonic sequence number so the web console can poll
        # incrementally via /api/console?since=N.
        self._console = []          # list of (seq, ts, text)
        self._console_seq = 0
        self._console_max = 2000
        self._console_lock = threading.Lock()
        # Result captured for the most recent `get mac_address` style probe.
        self._probe = {}
        # Routing table of the connected gateway node, learned by polling
        # `get routing`. Each entry: {dest, next_hop, hops, age}. This is the
        # ONLY source of multi-hop link info — beacons carry just the route
        # *count*, so next-hop relationships come from here.
        self._routes = []
        self._routes_ts = None
        self._routes_lock = threading.Lock()
        # Per-node routing tables learned from piggybacked beacon [BTBL] lines.
        # { src(int): {"routes": [{dest,next_hop,hops}], "ts": int} }
        self._node_routes = {}
        self._node_routes_lock = threading.Lock()
        # Browser-triggered remote config jobs and their CMD_ACK state.
        self._cfg_jobs = []
        self._cfg_lock = threading.Lock()
        self._cfg_seq = 0
        # OTA-over-LoRa transfer: a single active job plus the acks heard for it.
        self._ota_lock = threading.Lock()
        self._ota_cv = threading.Condition(self._ota_lock)
        self._ota_job = None
        self._ota_acks = []
        # Need check_same_thread=False so reader thread + HTTP threads share.
        self._conn = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
        with self._lock:
            self._conn.executescript(SCHEMA)

    def set_device(self, mac: int, ts: int, port: str | None = None):
        with self._lock:
            self._device_mac = mac
            self._device_mac_ts = ts
            if port is not None:
                self._device_port = port

    def set_device_port(self, port: str | None):
        with self._lock:
            self._device_port = port

    def set_routes(self, routes):
        """Store a fresh routing-table snapshot from the gateway node."""
        with self._routes_lock:
            self._routes = list(routes)
            self._routes_ts = int(time.time())

    def routes_info(self):
        with self._routes_lock:
            routes = list(self._routes)
            ts = self._routes_ts
        now = int(time.time())
        return {
            "routes": routes,
            "ts": ts,
            "age": (now - ts) if ts is not None else None,
        }

    def set_node_routes(self, src, routes):
        """Store the routing table a remote node advertised in its beacon."""
        with self._node_routes_lock:
            self._node_routes[int(src)] = {
                "routes": list(routes),
                "ts": int(time.time()),
            }

    def node_routes_info(self):
        """Return {src: {routes, age}} for every node that advertised a table."""
        now = int(time.time())
        with self._node_routes_lock:
            return {
                src: {
                    "routes": list(v["routes"]),
                    "age": now - v["ts"],
                }
                for src, v in self._node_routes.items()
            }

    def known_node_macs(self):
        """Known nodes from latest beacons, routes, BTBL rows, plus gateway."""
        gw_mac = self.device_info().get("mac")
        known = set()
        with self._lock:
            for (src,) in self._conn.execute("SELECT DISTINCT src FROM beacons"):
                known.add(int(src))
        for rt in self.routes_info()["routes"]:
            known.add(int(rt["dest"]))
            known.add(int(rt["next_hop"]))
        for src, info in self.node_routes_info().items():
            known.add(int(src))
            for rt in info["routes"]:
                known.add(int(rt["dest"]))
                known.add(int(rt["next_hop"]))
        if gw_mac is not None:
            known.add(int(gw_mac))
        known.discard(0)
        known.discard(255)
        return sorted(known)

    def start_cfg_job(self, target, expected, interval, persist):
        now = int(time.time())
        with self._cfg_lock:
            self._cfg_seq += 1
            job = {
                "id": self._cfg_seq,
                "ts": now,
                "target": target,
                "expected": list(expected),
                "interval": int(interval),
                "persist": bool(persist),
                "acks": {},
            }
            self._cfg_jobs.append(job)
            self._cfg_jobs = self._cfg_jobs[-20:]
            return job

    def add_cfg_ack(self, ack):
        src = int(ack["from"])
        with self._cfg_lock:
            for job in reversed(self._cfg_jobs):
                if src not in job["expected"]:
                    continue
                if int(job["interval"]) != int(ack["interval"]):
                    continue
                if bool(job["persist"]) != bool(ack["persist"]):
                    continue
                job["acks"][str(src)] = ack
                return
            self._cfg_jobs.append({
                "id": None,
                "ts": ack["ts"],
                "target": ack["target"],
                "expected": [src],
                "interval": ack["interval"],
                "persist": bool(ack["persist"]),
                "acks": {str(src): ack},
            })
            self._cfg_jobs = self._cfg_jobs[-20:]

    def cfg_status(self):
        now = int(time.time())
        with self._cfg_lock:
            jobs = json.loads(json.dumps(self._cfg_jobs))
        for job in jobs:
            job["age"] = now - job["ts"]
            job["acked"] = sorted(int(k) for k in job.get("acks", {}).keys())
            job["pending"] = [m for m in job.get("expected", [])
                              if str(m) not in job.get("acks", {})]
        return {"jobs": jobs}

    def cfg_pending_for_job(self, job_id):
        with self._cfg_lock:
            for job in self._cfg_jobs:
                if job.get("id") == job_id:
                    return [m for m in job.get("expected", [])
                            if str(m) not in job.get("acks", {})]
        return []

    # ----------------------------------------------------- OTA-over-LoRa driver
    def ota_active(self):
        with self._ota_lock:
            return self._ota_job is not None and self._ota_job["state"] == "running"

    def ota_start(self, target, firmware, total, chunks, crc):
        now = int(time.time())
        with self._ota_lock:
            self._ota_acks = []
            self._ota_job = {
                "state": "running", "target": int(target), "firmware": firmware,
                "total": int(total), "chunks": int(chunks),
                "sent_chunks": 0, "sent_bytes": 0, "retries": 0,
                "crc": "%08x" % crc, "started": now, "updated": now,
                "error": None, "log": [],
            }

    def ota_log(self, msg):
        with self._ota_lock:
            if self._ota_job is not None:
                self._ota_job["log"].append({"ts": int(time.time()), "msg": msg})
                self._ota_job["log"] = self._ota_job["log"][-200:]
                self._ota_job["updated"] = int(time.time())

    def ota_progress(self, sent_chunks, sent_bytes):
        with self._ota_lock:
            if self._ota_job is not None:
                self._ota_job["sent_chunks"] = sent_chunks
                self._ota_job["sent_bytes"] = sent_bytes
                self._ota_job["updated"] = int(time.time())

    def ota_bump_retry(self):
        with self._ota_lock:
            if self._ota_job is not None:
                self._ota_job["retries"] += 1

    def ota_finish(self, state, error=None):
        with self._ota_lock:
            if self._ota_job is not None:
                self._ota_job["state"] = state
                self._ota_job["error"] = error
                self._ota_job["updated"] = int(time.time())

    def ota_status(self):
        with self._ota_lock:
            return json.loads(json.dumps(self._ota_job)) if self._ota_job else None

    def add_ota_ack(self, ack):
        with self._ota_cv:
            self._ota_acks.append(ack)
            if len(self._ota_acks) > 8000:
                self._ota_acks = self._ota_acks[-8000:]
            self._ota_cv.notify_all()

    def ota_ack_mark(self):
        with self._ota_lock:
            return len(self._ota_acks)

    def ota_wait_ack(self, mark, op, off, timeout):
        """Block until an ack (after index `mark`) matches op (and off, if not
        None), or until timeout. Returns the ack dict or None."""
        deadline = time.time() + timeout
        with self._ota_cv:
            while True:
                for a in self._ota_acks[mark:]:
                    if a["op"] == op and (off is None or a["off"] == off):
                        return a
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                self._ota_cv.wait(remaining)

    # ----------------------------------------------------- serial / console
    def set_serial(self, ser):
        """Register (or clear) the live serial handle owned by the reader."""
        with self._serial_lock:
            self._serial = ser

    def serial_open(self) -> bool:
        with self._serial_lock:
            return self._serial is not None

    def send_command(self, text: str) -> bool:
        """Write a command line to the device through the shared handle.

        Returns True if written, False if no serial port is currently open.
        """
        with self._serial_lock:
            ser = self._serial
        if ser is None:
            return False
        if not text.endswith("\n"):
            text = text + "\n"
        try:
            ser.write(text.encode("ascii", errors="replace"))
            ser.flush()
        except Exception as e:
            self.console_add(f"[send error] {e}")
            return False
        self.console_add(">> " + text.rstrip("\n"))
        return True

    def console_add(self, text: str):
        with self._console_lock:
            self._console_seq += 1
            self._console.append((self._console_seq, int(time.time()), text))
            if len(self._console) > self._console_max:
                self._console = self._console[-self._console_max:]

    def console_since(self, since: int):
        with self._console_lock:
            lines = [
                {"seq": s, "ts": t, "text": x}
                for (s, t, x) in self._console if s > since
            ]
            last = self._console_seq
        return {"lines": lines, "last": last}

    def device_info(self):
        with self._lock:
            mac = self._device_mac
            ts = self._device_mac_ts
            port = self._device_port
        now = int(time.time())
        return {
            "mac": mac,
            "ts": ts,
            "age": (now - ts) if ts is not None else None,
            "port": port,
        }

    def seed_device_from_db(self):
        """Recover the most recent self-printing src (rssi=prssi=snr=0) from
        history so the connected-device MAC is known before the next beacon."""
        with self._lock:
            cur = self._conn.execute(
                "SELECT src, ts FROM beacons "
                "WHERE rssi=0 AND prssi=0 AND snr=0 "
                "ORDER BY ts DESC LIMIT 1"
            )
            r = cur.fetchone()
            if r:
                self._device_mac = r[0]
                self._device_mac_ts = r[1]

    def insert(self, ts: int, src: int, rx: dict | None, beacon: dict):
        rx = rx or {}
        row = (
            ts, src,
            rx.get("rssi"), rx.get("prssi"), rx.get("snr"),
            beacon.get("sf") or rx.get("sf"),
            beacon.get("tx_pwr"),
            beacon.get("bus_mv"), beacon.get("i_ma"),
            beacon.get("bat_mv"), beacon.get("chg"),
            beacon.get("temp_cdeg"), beacon.get("hum_cpct"),
            beacon.get("lat_udeg"), beacon.get("lon_udeg"), beacon.get("fix"),
            beacon.get("entries"),
        )
        with self._lock:
            self._conn.execute(
                "INSERT INTO beacons (ts, src, rssi, prssi, snr, sf, tx_pwr, "
                "bus_mv, i_ma, bat_mv, chg, temp_cdeg, hum_cpct, "
                "lat_udeg, lon_udeg, fix, entries) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                row,
            )

    def nodes(self):
        with self._lock:
            cur = self._conn.execute(
                "SELECT src, COUNT(*) AS n, MAX(ts) AS last_seen, MIN(ts) AS first_seen "
                "FROM beacons GROUP BY src ORDER BY src"
            )
            rows = cur.fetchall()
        return [
            {"src": r[0], "count": r[1], "last_seen": r[2], "first_seen": r[3]}
            for r in rows
        ]

    def series(self, src: int, since_ts: int, until_ts: int | None = None):
        until_ts = until_ts if until_ts is not None else int(time.time()) + 1
        with self._lock:
            cur = self._conn.execute(
                "SELECT ts, rssi, prssi, snr, sf, tx_pwr, bus_mv, i_ma, "
                "bat_mv, chg, temp_cdeg, hum_cpct, fix, entries "
                "FROM beacons WHERE src=? AND ts BETWEEN ? AND ? ORDER BY ts",
                (src, since_ts, until_ts),
            )
            rows = cur.fetchall()
        cols = ["ts", "rssi", "prssi", "snr", "sf", "tx_pwr", "bus_mv", "i_ma",
                "bat_mv", "chg", "temp_cdeg", "hum_cpct", "fix", "entries"]
        out = {c: [] for c in cols}
        # derived
        out["temp_c"] = []
        out["hum_pct"] = []
        out["bus_v"] = []
        out["bat_v"] = []
        out["p_mw"] = []
        for r in rows:
            for i, c in enumerate(cols):
                out[c].append(r[i])
            t = r[10]
            h = r[11]
            bus = r[6]
            bat = r[8]
            i_ma = r[7]
            out["temp_c"].append(t / 100.0 if t is not None else None)
            out["hum_pct"].append(h / 100.0 if h is not None else None)
            out["bus_v"].append(bus / 1000.0 if bus is not None else None)
            out["bat_v"].append(bat / 1000.0 if bat is not None else None)
            if bus is not None and i_ma is not None:
                out["p_mw"].append((bus * i_ma) // 1000)
            else:
                out["p_mw"].append(None)
        return out

    def latest(self, src: int):
        with self._lock:
            cur = self._conn.execute(
                "SELECT ts, rssi, prssi, snr, sf, tx_pwr, bus_mv, i_ma, "
                "bat_mv, chg, temp_cdeg, hum_cpct, lat_udeg, lon_udeg, fix, entries "
                "FROM beacons WHERE src=? ORDER BY ts DESC LIMIT 1",
                (src,),
            )
            r = cur.fetchone()
        if not r:
            return None
        return {
            "ts": r[0], "rssi": r[1], "prssi": r[2], "snr": r[3],
            "sf": r[4], "tx_pwr": r[5],
            "bus_mv": r[6], "i_ma": r[7], "bat_mv": r[8], "chg": r[9],
            "chg_str": CHARGE_STATUS.get(r[9], str(r[9])) if r[9] is not None else None,
            "temp_cdeg": r[10], "hum_cpct": r[11],
            "lat_udeg": r[12], "lon_udeg": r[13], "fix": r[14],
            "entries": r[15],
        }

    def map_data(self):
        """Return latest GPS position per node (only those with a valid fix)."""
        with self._lock:
            cur = self._conn.execute(
                "SELECT b.src, b.ts, b.rssi, b.snr, b.lat_udeg, b.lon_udeg, b.fix "
                "FROM beacons b "
                "INNER JOIN ("
                "  SELECT src, MAX(ts) AS mts FROM beacons "
                "  WHERE fix > 0 AND lat_udeg != 0 AND lon_udeg != 0 "
                "  GROUP BY src"
                ") latest ON b.src = latest.src AND b.ts = latest.mts"
            )
            rows = cur.fetchall()
        now = int(time.time())
        return [
            {
                "src": r[0], "ts": r[1], "age": now - r[1],
                "rssi": r[2], "snr": r[3],
                "lat": r[4] / 1e6, "lon": r[5] / 1e6, "fix": r[6],
            }
            for r in rows
        ]

    def topology(self):
        """Return the mesh topology as the gateway (this receiver) plus every
        node it has heard, with the latest link quality and route count.

        Beacons carry only the route *count* per node, but the gateway node's
        own routing table (polled via `get routing`, returned in `routes`)
        carries the `next_hop` for every destination — so multi-hop paths
        such as ``gateway -> 161 -> 45`` can be reconstructed and drawn.
        """
        with self._lock:
            cur = self._conn.execute(
                "SELECT b.src, b.ts, b.rssi, b.prssi, b.snr, b.sf, b.tx_pwr, "
                "       b.entries, cnt.n "
                "FROM beacons b "
                "INNER JOIN ("
                "  SELECT src, MAX(ts) AS mts, COUNT(*) AS n FROM beacons "
                "  GROUP BY src"
                ") cnt ON b.src = cnt.src AND b.ts = cnt.mts "
                "ORDER BY b.src"
            )
            rows = cur.fetchall()
        now = int(time.time())
        nodes = [
            {
                "src": r[0], "ts": r[1], "age": now - r[1],
                "rssi": r[2], "prssi": r[3], "snr": r[4],
                "sf": r[5], "tx_pwr": r[6],
                "entries": r[7], "count": r[8],
            }
            for r in rows
        ]
        gw_mac = self.device_info().get("mac")
        for nd in nodes:
            nd["is_gateway"] = (gw_mac is not None and nd["src"] == gw_mac)
        rinfo = self.routes_info()
        return {
            "gateway": {"id": "gw", "label": "Gateway (RX)", "mac": gw_mac},
            "nodes": nodes,
            "routes": rinfo["routes"],
            "routes_age": rinfo["age"],
            "node_routes": self.node_routes_info(),
        }



# ----------------------------------------------------------- Serial reader
def serial_reader(port: str, baud: int, store: Store, stop_event: threading.Event):
    if serial is None:
        print("pyserial not installed — cannot read serial. pip install pyserial", file=sys.stderr)
        return
    while not stop_event.is_set():
        try:
            # The CDC device can re-enumerate (ttyACM0 <-> ttyACM1) on reset.
            # If the configured port is gone, fall back to auto-detect so the
            # reader reconnects to wherever the board landed.
            open_port = port
            if not os.path.exists(open_port):
                detected = auto_detect_port()
                if detected:
                    if detected != port:
                        print(f"[serial] {port} missing; using {detected}", flush=True)
                    open_port = detected
            print(f"[serial] opening {open_port} @ {baud}", flush=True)
            ser = serial.Serial(open_port, baud, timeout=0.5)
            try:
                ser.setDTR(True)
                ser.setRTS(True)
            except Exception:
                pass
            store.set_device_port(open_port)
            store.set_serial(ser)
            store.console_add(f"[serial] connected {open_port} @ {baud}")
        except Exception as e:
            print(f"[serial] open failed: {e}; retrying in 5s", flush=True)
            stop_event.wait(5.0)
            continue

        buf = ""
        last_rx = None
        route_collect = []      # rows of the in-progress `get routing` dump
        route_expect = -1       # entry count from the table header (-1 = idle)
        try:
            while not stop_event.is_set():
                try:
                    raw = ser.read(512)
                except Exception as e:
                    print(f"[serial] read error: {e}", flush=True)
                    break
                if raw:
                    buf += raw.decode("ascii", errors="replace")
                # The MCU's [RX] and [BEACON] prints can interleave on the
                # CDC TX buffer when a packet arrives mid-print, producing
                # lines like "...freq=8680000[RX] src=44 ...". Inject a
                # newline before every [RX]/[BEACON] marker so each shows
                # up as its own line to the parser.
                buf = (buf.replace("[RX]", "\n[RX]").replace("[BEACON]", "\n[BEACON]")
                          .replace("[BTBL]", "\n[BTBL]").replace("[OTA_ACK]", "\n[OTA_ACK]"))
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    # Echo every line into the web console ring buffer.
                    store.console_add(line)
                    # ---- Routing table dump (`get routing`) -------------
                    # Header resets the collector; rows accumulate until the
                    # announced entry count is reached, then we publish a
                    # fresh snapshot. Beacon lines interleave harmlessly —
                    # they simply fall through to the parsers below.
                    rh = ROUTING_HDR_RE.search(line)
                    if rh:
                        route_expect = int(rh.group(1))
                        route_collect = []
                        if route_expect == 0:
                            store.set_routes([])
                            route_expect = -1
                        continue
                    if route_expect >= 0:
                        rr = parse_routing_row(line)
                        if rr is not None:
                            route_collect.append(rr)
                            if len(route_collect) >= route_expect:
                                store.set_routes(route_collect)
                                route_collect = []
                                route_expect = -1
                            continue
                    # ---- Per-node routing table piggybacked on a beacon ----
                    btbl = parse_btbl(line)
                    if btbl is not None:
                        store.set_node_routes(btbl[0], btbl[1])
                        continue
                    cfg_ack = parse_cmd_ack_beacon(line)
                    if cfg_ack is not None:
                        store.add_cfg_ack(cfg_ack)
                        continue
                    ota_ack = parse_ota_ack(line)
                    if ota_ack is not None:
                        store.add_ota_ack(ota_ack)
                        continue
                    # Learn the device MAC from an explicit `get mac_address`
                    # style reply, e.g. "mac_address=44" or "mac: 44".
                    mm = re.search(r"mac(?:_address)?\s*[:=]\s*(\d+)", line, re.I)
                    if mm:
                        store.set_device(int(mm.group(1)), int(time.time()))
                    rx = parse_rx(line)
                    if rx:
                        last_rx = rx
                        continue
                    bcn = parse_beacon(line)
                    if bcn and last_rx and last_rx.get("type") == 0:
                        ts = int(time.time())
                        # Self-print signature: the locally-connected board
                        # logs its own beacon with rssi=prssi=snr=0 (it can't
                        # measure its own TX). Use that to learn this device's
                        # MAC address.
                        if (last_rx.get("rssi") == 0 and last_rx.get("prssi") == 0
                                and last_rx.get("snr") == 0):
                            store.set_device(last_rx["src"], ts)
                        try:
                            store.insert(ts, last_rx["src"], last_rx, bcn)
                            diag = ""
                            if "rst" in bcn:
                                diag = (f" rst=0x{bcn['rst']:02X} boot={bcn['boot']}"
                                        f" last_stage={bcn['last_stage']} up_min={bcn['up_min']}")
                            print(f"[beacon] src={last_rx['src']} rssi={last_rx['rssi']} "
                                  f"snr={last_rx.get('snr')} bus_mv={bcn.get('bus_mv')} "
                                  f"i_ma={bcn.get('i_ma')} temp_cdeg={bcn.get('temp_cdeg')} "
                                  f"hum_cpct={bcn.get('hum_cpct')}{diag}", flush=True)
                        except Exception as e:
                            print(f"[db] insert failed: {e}", flush=True)
                        last_rx = None
        finally:
            store.set_serial(None)
            try:
                ser.close()
            except Exception:
                pass
        if not stop_event.is_set():
            print("[serial] disconnected, retrying in 2s", flush=True)
            store.console_add("[serial] disconnected, retrying in 2s")
            stop_event.wait(2.0)


def routing_poller(store: Store, stop_event: threading.Event, interval: float = 8.0):
    """Periodically ask the connected gateway node for its routing table so the
    mesh topology can show real next-hop / multi-hop links."""
    stop_event.wait(4.0)   # let the serial reader connect first
    while not stop_event.is_set():
        if store.serial_open():
            store.send_command("get routing")
        stop_event.wait(interval)


def ota_driver(store: Store, target: int, fw_path: str, fw_name: str):
    """Push a firmware image to a node over LoRa with handshake + stop-and-wait ARQ.

    Protocol: PREPARE (handshake + version check + bank erase) → DATA... → END.
    Runs in its own thread. Drives the gateway one step at a time and waits for
    each [OTA_ACK] the gateway relays back from the target.
    """
    try:
        with open(fw_path, "rb") as f:
            data = f.read()
    except Exception as e:
        store.ota_start(target, fw_name, 0, 0, 0)
        store.ota_finish("error", f"cannot read firmware: {e}")
        return

    size = len(data)
    if size == 0 or size > OTA_MAX_IMAGE:
        store.ota_start(target, fw_name, size, 0, 0)
        store.ota_finish("error", f"image size {size} out of range (1..{OTA_MAX_IMAGE})")
        return

    crc = zlib.crc32(data) & 0xFFFFFFFF
    nchunks = (size + OTA_CHUNK - 1) // OTA_CHUNK
    build_id = _extract_build_id(data)
    store.ota_start(target, fw_name, size, nchunks, crc)
    store.ota_log(
        f"image {fw_name}: ver={build_id!r} size={size} crc={crc:08x} "
        f"chunks={nchunks} -> node {target}"
    )

    def step(cmd, op, off, timeout, label):
        """Send cmd and wait for matching ACK; retry up to OTA_MAX_RETRY times."""
        for attempt in range(OTA_MAX_RETRY):
            mark = store.ota_ack_mark()          # capture BEFORE sending
            store.send_command(cmd)
            ack = store.ota_wait_ack(mark, op, off, timeout)
            if ack is None:
                store.ota_bump_retry()
                store.ota_log(f"{label}: timeout (try {attempt + 1}/{OTA_MAX_RETRY})")
                continue
            if ack["status"] != 0:
                store.ota_bump_retry()
                store.ota_log(f"{label}: nak status={ack['status']} (try {attempt + 1})")
                continue
            return True
        return False

    # ---- Phase 1: PREPARE — handshake, version check, bank erase ----
    # Custom loop (not `step`) because status=OTA_PREPARE_SAME_VER is a clean
    # "already up to date" exit, not a retriable error.
    prepare_cmd = f"send ota prepare {target} {build_id} {size} {crc:08x}"
    prepare_ok = False
    for attempt in range(OTA_MAX_RETRY):
        mark = store.ota_ack_mark()
        store.send_command(prepare_cmd)
        ack = store.ota_wait_ack(mark, OTA_OP_PREPARE, None, OTA_PREPARE_TIMEOUT)
        if ack is None:
            store.ota_bump_retry()
            store.ota_log(f"prepare: timeout (try {attempt + 1}/{OTA_MAX_RETRY})")
            continue
        if ack["status"] == OTA_PREPARE_SAME_VER:
            store.ota_log(f"prepare: node {target} already running {build_id!r}")
            store.ota_finish("up_to_date", f"node {target} already on {build_id!r}")
            return
        if ack["status"] != OTA_PREPARE_OK:
            reason = {
                OTA_PREPARE_TOO_LARGE:  "image too large for OTA bank",
                OTA_PREPARE_BUSY:       "node already in OTA mode",
                OTA_PREPARE_ERASE_FAIL: "flash erase failed on node",
            }.get(ack["status"], f"unknown reason (status={ack['status']})")
            store.ota_bump_retry()
            store.ota_log(f"prepare: rejected — {reason} (try {attempt + 1})")
            continue
        prepare_ok = True
        break

    if not prepare_ok:
        store.ota_finish("error", "prepare failed (node unreachable or rejected)")
        return
    store.ota_log(f"prepare ok — node {target} erased bank, entering OTA RX mode")

    # ---- Phase 2: DATA — stop-and-wait ARQ ----
    off = 0
    idx = 0
    while off < size:
        chunk = data[off:off + OTA_CHUNK]
        pad = (-len(chunk)) % 8                  # flash writes are 8-byte aligned
        payload = chunk + (b"\xff" * pad)
        cmd = f"send ota data {target} {off} {payload.hex()}"
        if not step(cmd, 2, off, OTA_DATA_TIMEOUT, f"data@{off}"):
            store.ota_finish("error", f"data@{off} failed after retries")
            return
        off += len(chunk)
        idx += 1
        store.ota_progress(idx, off)

    # ---- Phase 3: END — CRC validation on target ----
    if not step(f"send ota end {target} {size} {crc:08x}", 3, None, OTA_END_TIMEOUT, "end"):
        store.ota_finish("error", "end failed (crc mismatch on target or no ack)")
        return

    store.ota_log("transfer complete — target validating CRC and rebooting to apply")
    store.ota_finish("done")


def auto_detect_port():
    if serial is None:
        return None
    for p in serial.tools.list_ports.comports():
        if p.vid in (0x0483, 0x04D8):
            return p.device
    # fallback: first ttyACM/ttyUSB
    for p in serial.tools.list_ports.comports():
        if "ACM" in p.device or "USB" in p.device:
            return p.device
    return None


# ----------------------------------------------------------------- HTTP

INDEX_HTML_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "index.html")


def make_handler(store: Store):
    class Handler(BaseHTTPRequestHandler):
        # Quieter logs
        def log_message(self, fmt, *args):
            return

        def _send_json(self, obj, status=200):
            data = json.dumps(obj).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _send_file(self, path, ctype):
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except FileNotFoundError:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _proxy_dev(self, method: str):
            """Forward /api/dev/<rest> to dev_web on 8089 as /api/<rest>.

            Streams SSE responses byte-for-byte; passes through JSON otherwise.
            """
            u = urlparse(self.path)
            # /api/dev/...  ->  /api/...
            sub = u.path[len("/api/dev"):] or "/"
            target = "/api" + sub
            if u.query:
                target += "?" + u.query
            body = b""
            if method == "POST":
                length = int(self.headers.get("Content-Length") or 0)
                if length:
                    body = self.rfile.read(length)
            ctype_in = self.headers.get("Content-Type", "application/json")
            req = (
                f"{method} {target} HTTP/1.1\r\n"
                f"Host: {DEV_WEB_HOST}:{DEV_WEB_PORT}\r\n"
                f"Connection: close\r\n"
                f"Content-Type: {ctype_in}\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"\r\n"
            ).encode("utf-8") + body
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.settimeout(10)
                s.connect((DEV_WEB_HOST, DEV_WEB_PORT))
                s.sendall(req)
                # Read upstream headers (small).
                s.settimeout(None)
                head = b""
                while b"\r\n\r\n" not in head:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    head += chunk
                sep = head.find(b"\r\n\r\n")
                if sep < 0:
                    self._send_json({"error": "upstream gave no headers"}, status=502)
                    return
                head_bytes, leftover = head[:sep], head[sep + 4:]
                lines = head_bytes.decode("iso-8859-1").split("\r\n")
                status_code = int(lines[0].split(" ", 2)[1])
                ctype_out = "application/octet-stream"
                for hl in lines[1:]:
                    if ":" in hl and hl.lower().startswith("content-type:"):
                        ctype_out = hl.split(":", 1)[1].strip()
                        break
                is_sse = "text/event-stream" in ctype_out
                self.send_response(status_code)
                self.send_header("Content-Type", ctype_out)
                self.send_header("Cache-Control", "no-store")
                if is_sse:
                    self.send_header("Connection", "keep-alive")
                    self.send_header("X-Accel-Buffering", "no")
                    self.end_headers()
                    if leftover:
                        self.wfile.write(leftover)
                        self.wfile.flush()
                    while True:
                        try:
                            chunk = s.recv(4096)
                        except OSError:
                            break
                        if not chunk:
                            break
                        try:
                            self.wfile.write(chunk)
                            self.wfile.flush()
                        except (BrokenPipeError, ConnectionResetError):
                            break
                else:
                    body_bytes = bytearray(leftover)
                    while True:
                        try:
                            chunk = s.recv(8192)
                        except OSError:
                            break
                        if not chunk:
                            break
                        body_bytes += chunk
                    data = bytes(body_bytes)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
            except ConnectionRefusedError:
                self._send_json(
                    {"error": f"dev_web not reachable on {DEV_WEB_HOST}:{DEV_WEB_PORT}"},
                    status=502,
                )
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as e:
                try:
                    self._send_json({"error": f"proxy: {e}"}, status=502)
                except Exception:
                    pass
            finally:
                try:
                    s.close()
                except Exception:
                    pass

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            try:
                if u.path in ("/", "/index.html"):
                    self._send_file(INDEX_HTML_PATH, "text/html; charset=utf-8")
                    return
                if u.path.startswith("/api/dev/") or u.path == "/api/dev":
                    self._proxy_dev("GET")
                    return
                if u.path == "/api/nodes":
                    self._send_json({"nodes": store.nodes(), "now": int(time.time())})
                    return
                if u.path == "/api/series":
                    src = int(q.get("src", ["0"])[0])
                    hours = float(q.get("hours", ["24"])[0])
                    now = int(time.time())
                    since = now - int(hours * 3600)
                    self._send_json({"src": src, "since": since, "now": now,
                                     "data": store.series(src, since, now)})
                    return
                if u.path == "/api/latest":
                    src = int(q.get("src", ["0"])[0])
                    self._send_json({"src": src, "latest": store.latest(src),
                                     "now": int(time.time())})
                    return
                if u.path == "/api/map":
                    self._send_json({"nodes": store.map_data(), "now": int(time.time())})
                    return
                if u.path == "/api/topology":
                    topo = store.topology()
                    topo["now"] = int(time.time())
                    self._send_json(topo)
                    return
                if u.path == "/api/device":
                    self._send_json({"device": store.device_info(),
                                     "now": int(time.time())})
                    return
                if u.path == "/api/console":
                    since = int(q.get("since", ["0"])[0])
                    out = store.console_since(since)
                    out["serial_open"] = store.serial_open()
                    out["now"] = int(time.time())
                    self._send_json(out)
                    return
                if u.path == "/api/config/status":
                    out = store.cfg_status()
                    out["known_nodes"] = store.known_node_macs()
                    out["gateway"] = store.device_info()
                    self._send_json(out)
                    return
                if u.path == "/api/firmwares":
                    self._send_json({"firmwares": discover_firmwares()})
                    return
                if u.path == "/api/ota/status":
                    self._send_json({"ota": store.ota_status(),
                                     "serial_open": store.serial_open(),
                                     "now": int(time.time())})
                    return
                self.send_error(404)
            except Exception as e:
                self._send_json({"error": str(e)}, status=500)

        def do_POST(self):
            u = urlparse(self.path)
            try:
                if u.path.startswith("/api/dev/") or u.path == "/api/dev":
                    self._proxy_dev("POST")
                    return
                length = int(self.headers.get("Content-Length", "0") or "0")
                raw = self.rfile.read(length) if length else b""
                try:
                    body = json.loads(raw.decode("utf-8")) if raw else {}
                except Exception:
                    body = {}
                if u.path == "/api/send":
                    cmd = str(body.get("cmd", "")).strip()
                    if not cmd:
                        self._send_json({"ok": False, "error": "empty command"},
                                        status=400)
                        return
                    ok = store.send_command(cmd)
                    self._send_json({"ok": ok,
                                     "error": None if ok else "serial not open",
                                     "serial_open": store.serial_open()})
                    return
                if u.path == "/api/ota/start":
                    try:
                        target = int(body.get("target"))
                    except (TypeError, ValueError):
                        self._send_json({"ok": False, "error": "target required"}, status=400)
                        return
                    if target < 1 or target > 254:
                        self._send_json({"ok": False, "error": "target must be 1-254"}, status=400)
                        return
                    fw = str(body.get("firmware", ""))
                    chosen = None
                    for f in discover_firmwares():
                        if fw in (f["name"], f["path"]):
                            chosen = f
                            break
                    if chosen is None:
                        self._send_json({"ok": False, "error": "unknown firmware"}, status=400)
                        return
                    if not store.serial_open():
                        self._send_json({"ok": False, "error": "serial not open"}, status=400)
                        return
                    if store.ota_active():
                        self._send_json({"ok": False, "error": "an OTA is already running"}, status=409)
                        return
                    threading.Thread(
                        target=ota_driver,
                        args=(store, target, chosen["path"], chosen["name"]),
                        daemon=True,
                    ).start()
                    self._send_json({"ok": True, "target": target,
                                     "firmware": chosen["name"], "size": chosen["size"]})
                    return
                if u.path == "/api/config/beacon":
                    try:
                        interval = int(body.get("interval", 0))
                        persist = bool(body.get("persist", False))
                        target_raw = body.get("target", "all")
                    except Exception:
                        self._send_json({"ok": False, "error": "bad request"}, status=400)
                        return
                    if interval < 1 or interval > 255:
                        self._send_json({"ok": False, "error": "interval must be 1-255 seconds"}, status=400)
                        return
                    known = store.known_node_macs()
                    if target_raw in ("all", 255, "255"):
                        target = "all"
                        expected = known
                        send_targets = known
                    else:
                        target = int(target_raw)
                        if target < 1 or target > 254:
                            self._send_json({"ok": False, "error": "target must be all or 1-254"}, status=400)
                            return
                        expected = [target]
                        send_targets = [target]
                    if not expected:
                        self._send_json({"ok": False, "error": "no known target nodes"}, status=400)
                        return
                    job = store.start_cfg_job(target, expected, interval, persist)
                    gateway_mac = store.device_info().get("mac")
                    commands = [
                        (f"set beacon_interval {interval}{' persist' if persist else ''}"
                         if gateway_mac is not None and int(node) == int(gateway_mac)
                         else f"send cfg beacon {interval} {1 if persist else 0} {node}")
                        for node in send_targets
                    ]
                    ok = False
                    repeats = 2
                    sent_commands = []

                    def mark_local_ack(node):
                        store.add_cfg_ack({
                            "from": int(node),
                            "status": 0,
                            "target": int(node),
                            "persist": 1 if persist else 0,
                            "interval": int(interval),
                            "ts": int(time.time()),
                        })

                    def send_cfg_command(node, cmd):
                        nonlocal ok
                        if gateway_mac is not None and int(node) == int(gateway_mac):
                            local_ok = store.send_command(cmd)
                            ok = local_ok or ok
                            sent_commands.append(cmd)
                            if local_ok:
                                mark_local_ack(node)
                            return
                        for i in range(repeats):
                            ok = store.send_command(cmd) or ok
                            sent_commands.append(cmd)
                            if i + 1 < repeats:
                                time.sleep(2.0)

                    for ci, (node, cmd) in enumerate(zip(send_targets, commands)):
                        send_cfg_command(node, cmd)
                        if ci + 1 < len(commands):
                            time.sleep(1.5)

                    time.sleep(3.0)
                    pending = store.cfg_pending_for_job(job["id"])
                    for pi, node in enumerate(pending):
                        cmd = (f"set beacon_interval {interval}{' persist' if persist else ''}"
                               if gateway_mac is not None and int(node) == int(gateway_mac)
                               else f"send cfg beacon {interval} {1 if persist else 0} {node}")
                        send_cfg_command(node, cmd)
                        if pi + 1 < len(pending):
                            time.sleep(2.0)
                    self._send_json({
                        "ok": ok,
                        "job": job,
                        "commands": commands,
                        "sent_commands": sent_commands,
                        "repeats": repeats,
                        "error": None if ok else "serial not open",
                        "serial_open": store.serial_open(),
                    })
                    return
                self.send_error(404)
            except Exception as e:
                self._send_json({"error": str(e)}, status=500)

    return Handler


# ----------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Saturn LoRa web dashboard")
    ap.add_argument("--port", default=None, help="serial port (auto-detect if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--db", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "beacons.db"))
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--no-serial", action="store_true",
                    help="don't read serial; just serve the UI from existing DB")
    args = ap.parse_args()

    print(f"[db] {args.db}", flush=True)
    store = Store(args.db)
    store.seed_device_from_db()
    stop = threading.Event()
    reader = None

    if not args.no_serial:
        port = args.port or auto_detect_port()
        if port is None:
            print("[serial] no port found and --port not given. "
                  "Pass --no-serial to just browse the DB.", file=sys.stderr)
        else:
            reader = threading.Thread(
                target=serial_reader,
                args=(port, args.baud, store, stop),
                daemon=True,
            )
            reader.start()
            # Poll the gateway node's routing table for mesh topology.
            threading.Thread(
                target=routing_poller,
                args=(store, stop),
                daemon=True,
            ).start()

    httpd = ThreadingHTTPServer((args.bind, args.http_port), make_handler(store))
    url = f"http://{args.bind if args.bind != '0.0.0.0' else 'localhost'}:{args.http_port}/"
    print(f"[http] serving {url}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[http] shutting down", flush=True)
    finally:
        stop.set()
        httpd.server_close()


if __name__ == "__main__":
    main()
