#!/usr/bin/env python3
"""
Saturn Dev Dashboard — local development tool for pi5-jenkins.

A browser-based control panel that:
  * Lists USB-connected STM32 devices (CDC + DFU mode)
  * Shows per-device info (USB path, VID/PID, serial, MAC, UID, BOR, firmware)
  * Streams the CDC console live and lets you send commands
  * Flashes a chosen firmware via USB DFU or SWD (sudo openocd)
  * Reserves a section for future OTA updates

Stdlib only + pyserial.

Usage:
  python3 server.py                    # listens on 0.0.0.0:8089
  python3 server.py --port 9000

Open  http://<host>:8089/  from the LAN.
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
import zlib
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import urlparse, parse_qs

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pyserial required: pip install pyserial", file=sys.stderr)
    sys.exit(1)


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIRMWARE_DIR = os.path.join(REPO_ROOT, "firmware")
TEST_DIR = os.path.join(FIRMWARE_DIR, "app", "lora", "test")
STATIC_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_HTML = os.path.join(STATIC_DIR, "index.html")
TESTS_HTML = os.path.join(STATIC_DIR, "tests.html")
JUNIT_DIR = "/tmp/dev_web_junit"
os.makedirs(JUNIT_DIR, exist_ok=True)

# ---------------------------------------------------------------- USB enum
def lsusb_dfu() -> list[dict]:
    """Return list of STM32 DFU devices (0483:df11) with bus/port path."""
    out = []
    try:
        r = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=3)
    except Exception:
        return out
    for line in r.stdout.splitlines():
        m = re.match(r"Bus (\d+) Device (\d+): ID (\w+):(\w+)\s*(.*)", line)
        if not m:
            continue
        vid, pid = m.group(3).lower(), m.group(4).lower()
        if (vid, pid) == ("0483", "df11"):
            out.append({
                "kind": "dfu",
                "bus": m.group(1),
                "device": m.group(2),
                "vid": vid, "pid": pid,
                "desc": m.group(5).strip(),
            })
    return out


def list_cdc_devices() -> list[dict]:
    """Return list of USB CDC ports with metadata (skip onboard UARTs)."""
    out = []
    for p in serial.tools.list_ports.comports():
        # Skip non-USB ports (RPi onboard UART /dev/ttyAMA*, ttyS*, etc.)
        if p.vid is None and not (p.device.startswith("/dev/ttyACM")
                                  or p.device.startswith("/dev/ttyUSB")):
            continue
        vid = f"{p.vid:04x}" if p.vid is not None else None
        pid = f"{p.pid:04x}" if p.pid is not None else None
        # Only STM32 CDC (0483:5740) by default — but show others too as info
        is_stm32 = (vid, pid) == ("0483", "5740")
        out.append({
            "kind": "cdc",
            "device": p.device,
            "vid": vid, "pid": pid,
            "serial_number": p.serial_number,
            "manufacturer": p.manufacturer,
            "product": p.product,
            "location": p.location,           # bus-port path
            "hwid": p.hwid,
            "is_stm32": is_stm32,
        })
    return out


# ---------------------------------------------------------------- Console
class ConsoleSession:
    """A single open serial port with broadcast to N SSE subscribers.

    The port is opened lazily on first subscriber and closed when the last
    subscriber leaves *and* `stop()` was called externally (e.g. before
    flashing). For normal operation we keep it open.
    """
    BUFLEN = 5000   # ring of recent lines for replay

    def __init__(self, device: str, baud: int = 115200):
        self.device = device
        self.baud = baud
        self._ser: Optional[serial.Serial] = None
        self._subs: list[queue.Queue] = []
        self._sub_lock = threading.Lock()
        self._reader: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._closed = threading.Event()
        self._buf = deque(maxlen=self.BUFLEN)   # recent lines for replay
        self._partial = ""

    # --- lifecycle -----------------------------------------------------
    def open(self):
        if self._ser is not None:
            return
        self._ser = serial.Serial(self.device, self.baud, timeout=0.2)
        try:
            self._ser.setDTR(True)
            self._ser.setRTS(True)
        except Exception:
            pass
        self._stop.clear()
        self._closed.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def close(self):
        self._stop.set()
        if self._reader:
            self._reader.join(timeout=2)
        if self._ser:
            try:
                self._ser.close()
            except Exception:
                pass
        self._ser = None
        self._reader = None
        self._closed.set()
        self._broadcast({"type": "closed"})

    # --- I/O -----------------------------------------------------------
    def send(self, text: str):
        if self._ser is None:
            raise RuntimeError("console not open")
        if not text.endswith("\n"):
            text += "\r\n"
        self._ser.write(text.encode("utf-8", errors="replace"))
        # echo to subscribers so the user sees what was sent
        self._broadcast({"type": "tx", "text": text.rstrip("\r\n")})

    def _read_loop(self):
        while not self._stop.is_set():
            try:
                raw = self._ser.read(512) if self._ser else b""
            except Exception as e:
                self._broadcast({"type": "error", "text": f"read error: {e}"})
                break
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace")
            # split into lines but keep partial for next read
            self._partial += text
            # Inject newline before any [RX]/[BEACON]/[BTBL]/[OTA_ACK] marker
            # (firmware can interleave prints — same fix as in lora_web/server.py).
            self._partial = (self._partial
                             .replace("[RX]", "\n[RX]")
                             .replace("[BEACON]", "\n[BEACON]")
                             .replace("[BTBL]", "\n[BTBL]")
                             .replace("[OTA_ACK]", "\n[OTA_ACK]"))
            while "\n" in self._partial:
                line, self._partial = self._partial.split("\n", 1)
                line = line.rstrip("\r")
                if not line:
                    continue
                self._buf.append(line)
                self._broadcast({"type": "rx", "text": line})

    # --- subscribers ---------------------------------------------------
    def subscribe(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=1000)
        # replay recent buffer to new subscribers
        for line in list(self._buf):
            try:
                q.put_nowait({"type": "rx", "text": line, "replay": True})
            except queue.Full:
                break
        with self._sub_lock:
            self._subs.append(q)
        return q

    def unsubscribe(self, q: queue.Queue):
        with self._sub_lock:
            try:
                self._subs.remove(q)
            except ValueError:
                pass

    def _broadcast(self, msg: dict):
        with self._sub_lock:
            subs = list(self._subs)
        for q in subs:
            try:
                q.put_nowait(msg)
            except queue.Full:
                pass


class ConsoleManager:
    def __init__(self):
        self._sessions: dict[str, ConsoleSession] = {}
        self._lock = threading.Lock()

    def get(self, device: str) -> ConsoleSession:
        with self._lock:
            s = self._sessions.get(device)
            if s is None:
                s = ConsoleSession(device)
                self._sessions[device] = s
            return s

    def open(self, device: str) -> ConsoleSession:
        s = self.get(device)
        try:
            s.open()
        except Exception:
            # drop bad session so user can retry
            with self._lock:
                self._sessions.pop(device, None)
            raise
        return s

    def close(self, device: str):
        with self._lock:
            s = self._sessions.pop(device, None)
        if s is not None:
            s.close()

    def close_all(self):
        with self._lock:
            sessions = list(self._sessions.values())
            self._sessions.clear()
        for s in sessions:
            s.close()


# ---------------------------------------------------------------- Jobs
class Job:
    """Async subprocess with line-buffered output broadcast to subscribers."""
    def __init__(self, cmd: list[str], cwd: str, label: str):
        self.id = uuid.uuid4().hex[:12]
        self.cmd = cmd
        self.cwd = cwd
        self.label = label
        self.created = time.time()
        self.exit_code: Optional[int] = None
        self.lines: list[str] = []
        self._subs: list[queue.Queue] = []
        self._lock = threading.Lock()
        self._proc: Optional[subprocess.Popen] = None

    def start(self):
        def _run():
            self._broadcast({"type": "info",
                             "text": f"$ {' '.join(shlex.quote(c) for c in self.cmd)}"})
            try:
                self._proc = subprocess.Popen(
                    self.cmd, cwd=self.cwd,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1,
                )
            except Exception as e:
                self._broadcast({"type": "error", "text": f"spawn failed: {e}"})
                self.exit_code = -1
                self._broadcast({"type": "done", "code": -1})
                return
            assert self._proc.stdout is not None
            for line in self._proc.stdout:
                line = line.rstrip("\n")
                self.lines.append(line)
                self._broadcast({"type": "log", "text": line})
            self._proc.wait()
            self.exit_code = self._proc.returncode
            self._broadcast({"type": "done", "code": self.exit_code})
        threading.Thread(target=_run, daemon=True).start()

    def subscribe(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=2000)
        # replay
        for line in self.lines:
            q.put_nowait({"type": "log", "text": line, "replay": True})
        if self.exit_code is not None:
            q.put_nowait({"type": "done", "code": self.exit_code, "replay": True})
        with self._lock:
            self._subs.append(q)
        return q

    def unsubscribe(self, q: queue.Queue):
        with self._lock:
            try:
                self._subs.remove(q)
            except ValueError:
                pass

    def _broadcast(self, msg: dict):
        with self._lock:
            subs = list(self._subs)
        for q in subs:
            try:
                q.put_nowait(msg)
            except queue.Full:
                pass


class JobManager:
    def __init__(self):
        self._jobs: dict[str, Job] = {}
        self._lock = threading.Lock()

    def add(self, job: Job):
        with self._lock:
            self._jobs[job.id] = job

    def get(self, jid: str) -> Optional[Job]:
        with self._lock:
            return self._jobs.get(jid)


# ---------------------------------------------------------------- Firmware
def _extract_build_id(data: bytes) -> str:
    """Scan a firmware binary for the SATVER magic tag written by version.c
    (see firmware/src/system/version.c — the tag is the only copy of the
    build_id string, kept alive against --gc-sections)."""
    m = re.search(rb'\x00SATVER:([^\x00]{1,16})', data)
    return m.group(1).decode('ascii', errors='replace') if m else 'unknown'


def list_firmwares() -> list[dict]:
    """Find candidate .bin firmwares in the repo."""
    out = []
    app_dir = os.path.join(FIRMWARE_DIR, "app")
    if not os.path.isdir(app_dir):
        return out
    for app in sorted(os.listdir(app_dir)):
        appdir = os.path.join(app_dir, app)
        if not os.path.isdir(appdir):
            continue
        # candidates: <app>.bin in appdir or build/<app>.bin
        for cand in (os.path.join(appdir, f"{app}.bin"),
                     os.path.join(appdir, "build", f"{app}.bin")):
            if os.path.isfile(cand):
                st = os.stat(cand)
                rel = os.path.relpath(cand, REPO_ROOT)
                try:
                    with open(cand, "rb") as f:
                        build_id = _extract_build_id(f.read())
                except Exception:
                    build_id = "unknown"
                out.append({
                    "app": app,
                    "path": rel,
                    "size": st.st_size,
                    "mtime": int(st.st_mtime),
                    "build_id": build_id,
                })
    return out


# ---------------------------------------------------------------- OTA-over-LoRa
# Drives the wireless firmware-update protocol (PREPARE/DATA/END, stop-and-wait
# ARQ) through an already-open ConsoleSession, the same way a human would type
# `send ota ...` relay commands — just automated, with retries and progress.
# Mirrors tools/lora_web/server.py's ota_driver(); kept independent because
# lora_web owns one dedicated serial reader thread per process while dev_web
# multiplexes several devices through ConsoleSession/ConsoleManager instead.

OTA_CHUNK = 192   # 8-byte aligned; must match firmware Packet.data[]/relay buf
OTA_MAX_IMAGE = 62 * 1024
OTA_PREPARE_TIMEOUT = 35.0   # includes ~31 pages * ~100ms erase time
OTA_DATA_TIMEOUT = 8.0       # must exceed worst-case MAC CSMA back-off both ways
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

OTA_ACK_RE = re.compile(
    r"\[OTA_ACK\]\s+from=(\d+)\s+op=(\d+)\s+status=(\d+)\s+off=(\d+)", re.I)


def parse_ota_ack(line: str):
    m = OTA_ACK_RE.search(line)
    if not m:
        return None
    return {"from": int(m.group(1)), "op": int(m.group(2)),
            "status": int(m.group(3)), "off": int(m.group(4))}


class OtaState:
    """Single active OTA job (this is a one-engineer dev tool — one transfer
    at a time is the expected usage, same as the Flash panel's one job)."""
    def __init__(self):
        self._lock = threading.Lock()
        self._job = None

    def active(self) -> bool:
        with self._lock:
            return self._job is not None and self._job["state"] == "running"

    def start(self, device, target, firmware, total, chunks, crc):
        now = time.time()
        with self._lock:
            self._job = {
                "device": device, "target": int(target), "firmware": firmware,
                "state": "running", "total": int(total), "chunks": int(chunks),
                "sent_chunks": 0, "sent_bytes": 0, "retries": 0,
                "crc": "%08x" % crc, "started": now, "updated": now,
                "error": None, "log": [],
            }

    def log(self, msg):
        with self._lock:
            if self._job is not None:
                self._job["log"].append({"ts": time.time(), "msg": msg})
                self._job["log"] = self._job["log"][-200:]
                self._job["updated"] = time.time()

    def progress(self, sent_chunks, sent_bytes):
        with self._lock:
            if self._job is not None:
                self._job["sent_chunks"] = sent_chunks
                self._job["sent_bytes"] = sent_bytes
                self._job["updated"] = time.time()

    def bump_retry(self):
        with self._lock:
            if self._job is not None:
                self._job["retries"] += 1

    def finish(self, state, error=None):
        with self._lock:
            if self._job is not None:
                self._job["state"] = state
                self._job["error"] = error
                self._job["updated"] = time.time()

    def status(self):
        with self._lock:
            return json.loads(json.dumps(self._job)) if self._job else None


def ota_driver(srv: "DevServer", device: str, target: int, fw_path: str, fw_name: str):
    """Push a firmware image to `target` over LoRa, relayed through `device`.

    Protocol: PREPARE (handshake + version check + bank erase) -> DATA... -> END.
    Runs in its own thread; reads ACKs off the device's live ConsoleSession
    (the same stream the web console/SSE tails), so the console panel shows
    the whole exchange while it runs.
    """
    try:
        with open(fw_path, "rb") as f:
            data = f.read()
    except Exception as e:
        srv.ota.start(device, target, fw_name, 0, 0, 0)
        srv.ota.finish("error", f"cannot read firmware: {e}")
        return

    size = len(data)
    if size == 0 or size > OTA_MAX_IMAGE:
        srv.ota.start(device, target, fw_name, size, 0, 0)
        srv.ota.finish("error", f"image size {size} out of range (1..{OTA_MAX_IMAGE})")
        return

    crc = zlib.crc32(data) & 0xFFFFFFFF
    nchunks = (size + OTA_CHUNK - 1) // OTA_CHUNK
    build_id = _extract_build_id(data)
    srv.ota.start(device, target, fw_name, size, nchunks, crc)
    srv.ota.log(
        f"image {fw_name}: ver={build_id!r} size={size} crc={crc:08x} "
        f"chunks={nchunks} -> node {target}"
    )

    try:
        sess = srv.consoles.open(device)
    except Exception as e:
        srv.ota.finish("error", f"cannot open console on {device}: {e}")
        return

    q = sess.subscribe()
    try:
        def wait_ack(op, off, timeout):
            deadline = time.time() + timeout
            while True:
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                try:
                    msg = q.get(timeout=remaining)
                except queue.Empty:
                    return None
                if msg.get("type") != "rx":
                    continue
                ack = parse_ota_ack(msg.get("text", ""))
                if ack and ack["op"] == op and (off is None or ack["off"] == off):
                    return ack

        def step(cmd, op, off, timeout, label):
            """Send cmd and wait for matching ACK; retry up to OTA_MAX_RETRY times."""
            for attempt in range(OTA_MAX_RETRY):
                sess.send(cmd)
                ack = wait_ack(op, off, timeout)
                if ack is None:
                    srv.ota.bump_retry()
                    srv.ota.log(f"{label}: timeout (try {attempt + 1}/{OTA_MAX_RETRY})")
                    continue
                if ack["status"] != 0:
                    srv.ota.bump_retry()
                    srv.ota.log(f"{label}: nak status={ack['status']} (try {attempt + 1})")
                    continue
                return True
            return False

        # ---- Phase 1: PREPARE — handshake, version check, bank erase ----
        # Custom loop (not `step`) because status=OTA_PREPARE_SAME_VER is a
        # clean "already up to date" exit, not a retriable error.
        prepare_cmd = f"send ota prepare {target} {build_id} {size} {crc:08x}"
        prepare_ok = False
        for attempt in range(OTA_MAX_RETRY):
            sess.send(prepare_cmd)
            ack = wait_ack(OTA_OP_PREPARE, None, OTA_PREPARE_TIMEOUT)
            if ack is None:
                srv.ota.bump_retry()
                srv.ota.log(f"prepare: timeout (try {attempt + 1}/{OTA_MAX_RETRY})")
                continue
            if ack["status"] == OTA_PREPARE_SAME_VER:
                srv.ota.log(f"prepare: node {target} already running {build_id!r}")
                srv.ota.finish("up_to_date", f"node {target} already on {build_id!r}")
                return
            if ack["status"] != OTA_PREPARE_OK:
                reason = {
                    OTA_PREPARE_TOO_LARGE:  "image too large for OTA bank",
                    OTA_PREPARE_BUSY:       "node already in OTA mode",
                    OTA_PREPARE_ERASE_FAIL: "flash erase failed on node",
                }.get(ack["status"], f"unknown reason (status={ack['status']})")
                srv.ota.bump_retry()
                srv.ota.log(f"prepare: rejected — {reason} (try {attempt + 1})")
                continue
            prepare_ok = True
            break

        if not prepare_ok:
            srv.ota.finish("error", "prepare failed (node unreachable or rejected)")
            return
        srv.ota.log(f"prepare ok — node {target} erased bank, entering OTA RX mode")

        # ---- Phase 2: DATA — stop-and-wait ARQ ----
        off = 0
        idx = 0
        while off < size:
            chunk = data[off:off + OTA_CHUNK]
            pad = (-len(chunk)) % 8   # flash writes are 8-byte aligned
            payload = chunk + (b"\xff" * pad)
            cmd = f"send ota data {target} {off} {payload.hex()}"
            if not step(cmd, OTA_OP_DATA, off, OTA_DATA_TIMEOUT, f"data@{off}"):
                srv.ota.finish("error", f"data@{off} failed after retries")
                return
            off += len(chunk)
            idx += 1
            srv.ota.progress(idx, off)

        # ---- Phase 3: END — CRC validation on target ----
        if not step(f"send ota end {target} {size} {crc:08x}", OTA_OP_END, None,
                    OTA_END_TIMEOUT, "end"):
            srv.ota.finish("error", "end failed (crc mismatch on target or no ack)")
            return

        srv.ota.log("transfer complete — target validating CRC and rebooting to apply")
        srv.ota.finish("done")
    finally:
        sess.unsubscribe(q)


# ---------------------------------------------------------------- Tests
def list_tests() -> dict:
    """Run pytest --collect-only -q and parse into class -> [tests]."""
    try:
        r = subprocess.run(
            ["python3", "-m", "pytest", "--collect-only", "-q",
             "test_target_test.py"],
            cwd=TEST_DIR, capture_output=True, text=True, timeout=15,
        )
    except Exception as e:
        return {"error": f"pytest collect failed: {e}", "tests": []}
    tests = []
    classes: dict[str, list[str]] = {}
    for line in r.stdout.splitlines():
        line = line.strip()
        if "::" not in line:
            continue
        # e.g. "test_target_test.py::TestDeviceSunshineScenarios::test_help"
        parts = line.split("::")
        if len(parts) < 3:
            continue
        cls, fn = parts[1], parts[-1]
        tests.append({"id": line, "class": cls, "name": fn})
        classes.setdefault(cls, []).append(fn)
    return {"tests": tests,
            "classes": [{"name": c, "tests": classes[c]} for c in classes],
            "total": len(tests)}


def parse_junit(jid: str, job: "Job") -> dict:
    """Parse JUnit XML for a finished test job. Returns summary + per-test."""
    import xml.etree.ElementTree as ET
    xml_path = os.path.join(JUNIT_DIR, f"{jid}.xml")
    out: dict = {
        "job": jid,
        "exit_code": job.exit_code,
        "running": job.exit_code is None,
        "tests": [],
        "summary": {"total": 0, "passed": 0, "failed": 0, "skipped": 0,
                    "errors": 0, "duration": 0.0},
    }
    if not os.path.isfile(xml_path):
        return out
    try:
        tree = ET.parse(xml_path)
    except Exception as e:
        out["error"] = f"junit parse: {e}"
        return out
    root = tree.getroot()
    # Could be <testsuites> wrapping <testsuite>, or <testsuite> root
    suites = root.findall(".//testsuite") or [root]
    for suite in suites:
        for tc in suite.findall("testcase"):
            t = {
                "class": tc.get("classname", ""),
                "name": tc.get("name", ""),
                "time": float(tc.get("time", 0) or 0),
                "status": "passed",
                "message": None,
            }
            f = tc.find("failure")
            e = tc.find("error")
            s = tc.find("skipped")
            if f is not None:
                t["status"] = "failed"
                t["message"] = (f.get("message") or "") + "\n" + (f.text or "")
                out["summary"]["failed"] += 1
            elif e is not None:
                t["status"] = "error"
                t["message"] = (e.get("message") or "") + "\n" + (e.text or "")
                out["summary"]["errors"] += 1
            elif s is not None:
                t["status"] = "skipped"
                t["message"] = s.get("message") or s.text or ""
                out["summary"]["skipped"] += 1
            else:
                out["summary"]["passed"] += 1
            out["summary"]["total"] += 1
            out["summary"]["duration"] += t["time"]
            out["tests"].append(t)
    return out


# ---------------------------------------------------------------- HTTP
class DevServer:
    def __init__(self):
        self.consoles = ConsoleManager()
        self.jobs = JobManager()
        self.ota = OtaState()


def _sse_format(msg: dict) -> bytes:
    return f"data: {json.dumps(msg)}\n\n".encode("utf-8")


def make_handler(srv: DevServer):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            return

        # --- helpers ---
        def _json(self, obj, status=200):
            data = json.dumps(obj).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _read_body(self) -> dict:
            n = int(self.headers.get("Content-Length") or 0)
            if not n:
                return {}
            raw = self.rfile.read(n)
            try:
                return json.loads(raw.decode("utf-8"))
            except Exception:
                return {}

        def _sse_open(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.send_header("X-Accel-Buffering", "no")
            self.end_headers()

        # --- routes ---
        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            try:
                if u.path in ("/", "/index.html"):
                    self._serve_file(INDEX_HTML, "text/html; charset=utf-8")
                    return
                if u.path in ("/tests", "/tests.html"):
                    self._serve_file(TESTS_HTML, "text/html; charset=utf-8")
                    return
                if u.path == "/api/tests/list":
                    self._json(list_tests())
                    return
                if u.path == "/api/tests/results":
                    jid = q.get("id", [""])[0]
                    job = srv.jobs.get(jid)
                    if not job:
                        self._json({"error": "no such job"}, 404); return
                    self._json(parse_junit(jid, job))
                    return
                if u.path == "/api/devices":
                    self._json({
                        "cdc": list_cdc_devices(),
                        "dfu": lsusb_dfu(),
                        "now": int(time.time()),
                    })
                    return
                if u.path == "/api/firmwares":
                    self._json({"firmwares": list_firmwares()})
                    return
                if u.path == "/api/ota/status":
                    self._json({"ota": srv.ota.status(), "now": int(time.time())})
                    return
                if u.path == "/api/console/stream":
                    device = q.get("device", [""])[0]
                    if not device:
                        self._json({"error": "device required"}, 400)
                        return
                    self._stream_console(device)
                    return
                if u.path == "/api/job/stream":
                    jid = q.get("id", [""])[0]
                    job = srv.jobs.get(jid)
                    if not job:
                        self._json({"error": "no such job"}, 404)
                        return
                    self._stream_job(job)
                    return
                self.send_error(404)
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as e:
                try:
                    self._json({"error": str(e)}, 500)
                except Exception:
                    pass

        def do_POST(self):
            u = urlparse(self.path)
            try:
                body = self._read_body()
                if u.path == "/api/console/open":
                    device = body.get("device", "")
                    if not device:
                        self._json({"error": "device required"}, 400); return
                    try:
                        srv.consoles.open(device)
                        self._json({"ok": True})
                    except Exception as e:
                        self._json({"error": str(e)}, 500)
                    return
                if u.path == "/api/console/close":
                    device = body.get("device", "")
                    srv.consoles.close(device)
                    self._json({"ok": True})
                    return
                if u.path == "/api/console/send":
                    device = body.get("device", "")
                    text = body.get("text", "")
                    sess = srv.consoles.get(device)
                    try:
                        sess.send(text)
                        self._json({"ok": True})
                    except Exception as e:
                        self._json({"error": str(e)}, 500)
                    return
                if u.path == "/api/probe":
                    self._do_probe(body)
                    return
                if u.path == "/api/flash":
                    self._do_flash(body)
                    return
                if u.path == "/api/tests/run":
                    self._do_tests_run(body)
                    return
                if u.path == "/api/ota/start":
                    self._do_ota_start(body)
                    return
                self.send_error(404)
            except Exception as e:
                try:
                    self._json({"error": str(e)}, 500)
                except Exception:
                    pass

        # --- file ---
        def _serve_file(self, path, ctype):
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except FileNotFoundError:
                self.send_error(404); return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        # --- SSE: console ---
        def _stream_console(self, device: str):
            try:
                sess = srv.consoles.open(device)
            except Exception as e:
                self._json({"error": str(e)}, 500); return
            self._sse_open()
            q = sess.subscribe()
            try:
                # initial hello
                self.wfile.write(_sse_format(
                    {"type": "hello", "device": device}))
                self.wfile.flush()
                last_keepalive = time.time()
                while True:
                    try:
                        msg = q.get(timeout=10)
                    except queue.Empty:
                        # keep-alive comment
                        self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                        last_keepalive = time.time()
                        continue
                    self.wfile.write(_sse_format(msg))
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                sess.unsubscribe(q)

        # --- SSE: job ---
        def _stream_job(self, job: Job):
            self._sse_open()
            q = job.subscribe()
            try:
                self.wfile.write(_sse_format(
                    {"type": "hello", "job": job.id, "label": job.label}))
                self.wfile.flush()
                while True:
                    try:
                        msg = q.get(timeout=10)
                    except queue.Empty:
                        self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                        if job.exit_code is not None:
                            break
                        continue
                    self.wfile.write(_sse_format(msg))
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                job.unsubscribe(q)

        # --- probe (inject CDC commands) ---
        def _do_probe(self, body):
            device = body.get("device", "")
            if not device:
                self._json({"error": "device required"}, 400); return
            try:
                sess = srv.consoles.open(device)
                for cmd in ("version", "get mac_address", "get uid",
                            "get bor", "get flash"):
                    sess.send(cmd)
                    time.sleep(0.05)
                self._json({"ok": True})
            except Exception as e:
                self._json({"error": str(e)}, 500)

        # --- flash ---
        def _do_flash(self, body):
            method = body.get("method", "")        # "dfu" | "swd"
            firmware = body.get("firmware", "")    # repo-relative path
            device = body.get("device", "")        # optional CDC dev for dfu --enter

            if method not in ("dfu", "swd"):
                self._json({"error": "method must be 'dfu' or 'swd'"}, 400); return
            if not firmware:
                self._json({"error": "firmware required"}, 400); return

            # safety: firmware path must stay inside repo
            abs_fw = os.path.abspath(os.path.join(REPO_ROOT, firmware))
            if not abs_fw.startswith(REPO_ROOT + os.sep) or not os.path.isfile(abs_fw):
                self._json({"error": "invalid firmware path"}, 400); return

            # Close any open console for the device (DFU jump uses the port).
            if device:
                srv.consoles.close(device)

            # firmware/tools/dfu_flash.sh and flash.sh expect to be run
            # from firmware/ with a path like "app/lora/lora.bin".
            rel_in_fw = os.path.relpath(abs_fw, FIRMWARE_DIR)

            if method == "dfu":
                cmd = ["bash", "tools/dfu_flash.sh", rel_in_fw, "--enter"]
            else:  # swd
                cmd = ["sudo", "-n", "bash", "tools/flash.sh", rel_in_fw]

            job = Job(cmd=cmd, cwd=FIRMWARE_DIR, label=f"{method}:{rel_in_fw}")
            srv.jobs.add(job)
            job.start()
            self._json({"job": job.id})

        # --- OTA-over-LoRa ---
        def _do_ota_start(self, body):
            device = body.get("device", "")
            firmware = body.get("firmware", "")   # repo-relative path, from /api/firmwares
            try:
                target = int(body.get("target"))
            except (TypeError, ValueError):
                self._json({"error": "target required"}, 400); return
            if not device:
                self._json({"error": "device required"}, 400); return
            if target < 1 or target > 254:
                self._json({"error": "target must be 1-254"}, 400); return

            chosen = None
            for f in list_firmwares():
                if firmware in (f["path"], f["app"]):
                    chosen = f
                    break
            if chosen is None:
                self._json({"error": "unknown firmware"}, 400); return

            if srv.ota.active():
                self._json({"error": "an OTA is already running"}, 409); return

            abs_fw = os.path.join(REPO_ROOT, chosen["path"])
            threading.Thread(
                target=ota_driver,
                args=(srv, device, target, abs_fw, chosen["path"]),
                daemon=True,
            ).start()
            self._json({"ok": True, "device": device, "target": target,
                        "firmware": chosen["path"], "size": chosen["size"]})

        # --- target tests (pytest) ---
        def _do_tests_run(self, body):
            ports = body.get("ports") or []
            selection = (body.get("selection") or "").strip()  # pytest -k expr
            test_ids = body.get("test_ids") or []              # explicit nodeids
            if not ports or not isinstance(ports, list):
                self._json({"error": "ports (list) required"}, 400); return
            for p in ports:
                if not isinstance(p, str) or not p.startswith("/dev/"):
                    self._json({"error": f"bad port: {p}"}, 400); return

            # Free the ports — pytest will open them itself.
            for p in ports:
                srv.consoles.close(p)

            jid = uuid.uuid4().hex[:12]
            xml_path = os.path.join(JUNIT_DIR, f"{jid}.xml")
            cmd = ["python3", "-m", "pytest", "test_target_test.py",
                   "-v", "--color=no", "-rN", f"--junitxml={xml_path}"]
            if selection:
                cmd += ["-k", selection]
            if test_ids:
                # nodeids relative to test_target_test.py
                cmd += test_ids

            env = os.environ.copy()
            env["SERIAL_PORTS"] = ",".join(ports)
            env["PYTHONUNBUFFERED"] = "1"

            label = f"pytest [{','.join(ports)}]"
            if selection:
                label += f" -k {selection!r}"
            if test_ids:
                label += f" ({len(test_ids)} sel)"
            job = Job(cmd=cmd, cwd=TEST_DIR, label=label)
            job.id = jid   # use our id so junit xml path matches
            # pass env to subprocess via Popen — Job uses env=None currently;
            # extend it inline by wrapping the cmd through `env`.
            # Simpler: monkey-patch by overriding start with env baked in.
            self._spawn_with_env(job, env)
            srv.jobs.add(job)
            self._json({"job": job.id})

        def _spawn_with_env(self, job: "Job", env: dict):
            def _run():
                job._broadcast({"type": "info",
                                "text": "$ SERIAL_PORTS=" + env.get("SERIAL_PORTS", "")
                                        + " " + " ".join(shlex.quote(c) for c in job.cmd)})
                try:
                    job._proc = subprocess.Popen(
                        job.cmd, cwd=job.cwd, env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, bufsize=1,
                    )
                except Exception as e:
                    job._broadcast({"type": "error", "text": f"spawn failed: {e}"})
                    job.exit_code = -1
                    job._broadcast({"type": "done", "code": -1})
                    return
                assert job._proc.stdout is not None
                for line in job._proc.stdout:
                    line = line.rstrip("\n")
                    job.lines.append(line)
                    job._broadcast({"type": "log", "text": line})
                job._proc.wait()
                job.exit_code = job._proc.returncode
                job._broadcast({"type": "done", "code": job.exit_code})
            threading.Thread(target=_run, daemon=True).start()

    return Handler


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Saturn dev dashboard")
    ap.add_argument("--port", type=int, default=8089)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    srv = DevServer()
    httpd = ThreadingHTTPServer((args.bind, args.port), make_handler(srv))
    url = f"http://{args.bind if args.bind != '0.0.0.0' else 'localhost'}:{args.port}/"
    print(f"[dev_web] serving {url}", flush=True)

    def _sigterm(*_):
        srv.consoles.close_all()
        httpd.shutdown()
    signal.signal(signal.SIGTERM, _sigterm)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.consoles.close_all()
        httpd.server_close()


if __name__ == "__main__":
    main()
