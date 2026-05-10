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
import sqlite3
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

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
        # Need check_same_thread=False so reader thread + HTTP threads share.
        self._conn = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
        with self._lock:
            self._conn.executescript(SCHEMA)

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


# ----------------------------------------------------------- Serial reader
def serial_reader(port: str, baud: int, store: Store, stop_event: threading.Event):
    if serial is None:
        print("pyserial not installed — cannot read serial. pip install pyserial", file=sys.stderr)
        return
    while not stop_event.is_set():
        try:
            print(f"[serial] opening {port} @ {baud}", flush=True)
            ser = serial.Serial(port, baud, timeout=0.5)
            try:
                ser.setDTR(True)
                ser.setRTS(True)
            except Exception:
                pass
        except Exception as e:
            print(f"[serial] open failed: {e}; retrying in 5s", flush=True)
            stop_event.wait(5.0)
            continue

        buf = ""
        last_rx = None
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
                buf = buf.replace("[RX]", "\n[RX]").replace("[BEACON]", "\n[BEACON]")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    rx = parse_rx(line)
                    if rx:
                        last_rx = rx
                        continue
                    bcn = parse_beacon(line)
                    if bcn and last_rx and last_rx.get("type") == 0:
                        ts = int(time.time())
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
            try:
                ser.close()
            except Exception:
                pass
        if not stop_event.is_set():
            print("[serial] disconnected, retrying in 2s", flush=True)
            stop_event.wait(2.0)


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
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            try:
                if u.path in ("/", "/index.html"):
                    self._send_file(INDEX_HTML_PATH, "text/html; charset=utf-8")
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
