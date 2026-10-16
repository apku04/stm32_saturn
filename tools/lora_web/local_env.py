"""
Local environment sensor (SHT3x on the gateway Pi's I2C bus).

A background thread samples the sensor every `interval` seconds and stores
each reading in the `env_readings` table of the dashboard's SQLite DB, so the
"Enclosure" tab can show how hot the gateway case gets. The Pi's CPU
temperature and throttle flags are logged alongside for comparison.

Requires: smbus2 (`sudo apt install python3-smbus2`).
"""

from __future__ import annotations

import math
import os
import sqlite3
import subprocess
import threading
import time

SHT3X_ADDRS = (0x44, 0x45)
CMD_MEASURE = (0x24, 0x00)      # single shot, high repeatability, no clock stretching
CMD_SOFT_RESET = (0x30, 0xA2)
MEASURE_WAIT = 0.02             # datasheet max 15.5 ms for high repeatability

SCHEMA = """
CREATE TABLE IF NOT EXISTS env_readings (
    ts        INTEGER NOT NULL,            -- unix epoch seconds (UTC)
    temp_cdeg INTEGER NOT NULL,            -- temperature * 100 (°C)
    hum_cpct  INTEGER NOT NULL,            -- humidity * 100 (%RH)
    cpu_cdeg  INTEGER                      -- Pi SoC temperature * 100 (°C)
);
CREATE INDEX IF NOT EXISTS idx_env_ts ON env_readings (ts);
"""

# Above this many raw rows a series request is averaged into buckets so the
# browser never has to plot more than ~MAX_POINTS points.
MAX_POINTS = 1500


def _crc8(data) -> int:
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def cpu_temp() -> float | None:
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return int(f.read().strip()) / 1000
    except (OSError, ValueError):
        return None


def throttled() -> int | None:
    """vcgencmd get_throttled bitmask (bit0 under-voltage now, bit2 throttled
    now, bit3 soft temp limit now; bits 16..19 = same, has occurred)."""
    try:
        out = subprocess.run(["vcgencmd", "get_throttled"], capture_output=True,
                             text=True, timeout=2).stdout
        return int(out.strip().split("=")[1], 16)
    except (OSError, IndexError, ValueError, subprocess.SubprocessError):
        return None


def dew_point(t: float, rh: float) -> float | None:
    """Magnus formula, good to ~0.35 °C for -45..60 °C."""
    if rh <= 0:
        return None
    a, b = 17.62, 243.12
    g = math.log(rh / 100.0) + a * t / (b + t)
    return b * g / (a - g)


class SHT3x:
    def __init__(self, bus: int = 1, addr: int | None = None):
        from smbus2 import SMBus, i2c_msg
        self._msg = i2c_msg
        self._bus = SMBus(bus)
        self.addr = addr if addr is not None else self._probe()

    def _probe(self) -> int:
        # The first transaction after power-up is sometimes NACKed, so retry.
        for _ in range(3):
            for a in SHT3X_ADDRS:
                try:
                    self._bus.write_quick(a)
                    return a
                except OSError:
                    pass
            time.sleep(0.01)
        raise OSError("no SHT3x found at 0x44/0x45")

    def _write(self, cmd):
        self._bus.i2c_rdwr(self._msg.write(self.addr, list(cmd)))

    def reset(self):
        try:
            self._write(CMD_SOFT_RESET)
        except OSError:
            pass
        time.sleep(0.002)

    def read(self) -> tuple[float, float]:
        self._write(CMD_MEASURE)
        time.sleep(MEASURE_WAIT)
        m = self._msg.read(self.addr, 6)
        self._bus.i2c_rdwr(m)
        d = list(m)
        if _crc8(d[0:2]) != d[2] or _crc8(d[3:5]) != d[5]:
            raise IOError(f"CRC mismatch {bytes(d).hex()}")
        t = -45 + 175 * ((d[0] << 8) | d[1]) / 65535
        rh = 100 * ((d[3] << 8) | d[4]) / 65535
        return t, rh

    def close(self):
        self._bus.close()


class EnvLogger:
    def __init__(self, db_path: str, bus: int = 1, interval: float = 60.0):
        self.bus = bus
        self.interval = interval
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(db_path, check_same_thread=False, isolation_level=None)
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.executescript(SCHEMA)
        cols = {r[1] for r in self._conn.execute("PRAGMA table_info(env_readings)")}
        if "cpu_cdeg" not in cols:
            self._conn.execute("ALTER TABLE env_readings ADD COLUMN cpu_cdeg INTEGER")
        self._status = {"ok": False, "addr": None, "error": "starting",
                        "last_ok_ts": None, "errors": 0}

    # ------------------------------------------------------------ sampling
    def _sample(self, dev: SHT3x, tries: int = 3):
        err = None
        for _ in range(tries):
            try:
                return dev.read()
            except OSError as e:
                err = e
                dev.reset()
                time.sleep(0.05)
        raise err

    def run(self, stop: threading.Event):
        # The Pi has no RTC: right after boot the wall clock is a stale
        # fake-hwclock value until NTP syncs, so hold off (bounded) to avoid
        # storing readings with wrong timestamps.
        flag = "/run/systemd/timesync/synchronized"
        if os.path.isdir(os.path.dirname(flag)):
            deadline = time.monotonic() + 120
            while (not os.path.exists(flag) and time.monotonic() < deadline
                   and not stop.is_set()):
                stop.wait(2)
        dev = None
        while not stop.is_set():
            try:
                if dev is None:
                    dev = SHT3x(self.bus)
                    self._set_status(addr=hex(dev.addr))
                    print(f"[env] SHT3x at {hex(dev.addr)} on i2c-{self.bus}, "
                          f"every {self.interval:g}s", flush=True)
                t, rh = self._sample(dev)
                ts = int(time.time())
                cpu = cpu_temp()
                with self._lock:
                    self._conn.execute(
                        "INSERT INTO env_readings (ts, temp_cdeg, hum_cpct, cpu_cdeg) "
                        "VALUES (?,?,?,?)",
                        (ts, round(t * 100), round(rh * 100),
                         None if cpu is None else round(cpu * 100)))
                self._set_status(ok=True, error=None, last_ok_ts=ts)
            except Exception as e:
                msg = f"{type(e).__name__}: {e}"
                if self._status["error"] != msg:
                    print(f"[env] sensor read failed: {msg}", flush=True)
                with self._lock:
                    self._status["errors"] += 1
                self._set_status(ok=False, error=msg)
                if dev is not None and "no SHT3x" in msg:
                    dev.close()
                    dev = None
            stop.wait(self.interval)
        if dev is not None:
            dev.close()

    def _set_status(self, **kw):
        with self._lock:
            self._status.update(kw)

    # ------------------------------------------------------------ queries
    def status(self) -> dict:
        with self._lock:
            st = dict(self._status, interval=self.interval)
        st["cpu_now_c"] = cpu_temp()
        st["throttled"] = throttled()
        return st

    def latest(self):
        with self._lock:
            row = self._conn.execute(
                "SELECT ts, temp_cdeg, hum_cpct, cpu_cdeg FROM env_readings "
                "ORDER BY ts DESC LIMIT 1"
            ).fetchone()
        if not row:
            return None
        t, rh = row[1] / 100, row[2] / 100
        dp = dew_point(t, rh)
        return {"ts": row[0], "temp_c": t, "hum_pct": rh,
                "dew_c": round(dp, 2) if dp is not None else None,
                "cpu_c": None if row[3] is None else row[3] / 100}

    def series(self, since: int, until: int) -> dict:
        with self._lock:
            n, first = self._conn.execute(
                "SELECT COUNT(*), MIN(ts) FROM env_readings WHERE ts BETWEEN ? AND ?",
                (since, until)).fetchone()
            if n > MAX_POINTS:
                span = until - max(since, first or since)
                bucket = max(1, -(-span // MAX_POINTS))
                rows = self._conn.execute(
                    "SELECT CAST(AVG(ts) AS INTEGER), AVG(temp_cdeg), AVG(hum_cpct), "
                    "AVG(cpu_cdeg) "
                    "FROM env_readings WHERE ts BETWEEN ? AND ? "
                    "GROUP BY ts / ? ORDER BY 1", (since, until, bucket)).fetchall()
            else:
                bucket = None
                rows = self._conn.execute(
                    "SELECT ts, temp_cdeg, hum_cpct, cpu_cdeg FROM env_readings "
                    "WHERE ts BETWEEN ? AND ? ORDER BY ts", (since, until)).fetchall()
            agg = self._conn.execute(
                "SELECT MIN(temp_cdeg), MAX(temp_cdeg), AVG(temp_cdeg), "
                "MIN(hum_cpct), MAX(hum_cpct), AVG(hum_cpct), MAX(cpu_cdeg) "
                "FROM env_readings WHERE ts BETWEEN ? AND ?", (since, until)).fetchone()
        c = lambda v: None if v is None else round(v / 100, 2)
        temps, hums = [c(r[1]) for r in rows], [c(r[2]) for r in rows]
        dews = [None if t is None or h is None or dew_point(t, h) is None
                else round(dew_point(t, h), 2) for t, h in zip(temps, hums)]
        return {
            "count": n, "bucket_s": bucket,
            "ts": [r[0] for r in rows],
            "temp_c": temps,
            "hum_pct": hums,
            "dew_c": dews,
            "cpu_c": [c(r[3]) for r in rows],
            "stats": {"temp_min": c(agg[0]), "temp_max": c(agg[1]), "temp_avg": c(agg[2]),
                      "hum_min": c(agg[3]), "hum_max": c(agg[4]), "hum_avg": c(agg[5]),
                      "cpu_max": c(agg[6])},
        }

    def csv(self, since: int, until: int) -> str:
        with self._lock:
            rows = self._conn.execute(
                "SELECT ts, temp_cdeg, hum_cpct, cpu_cdeg FROM env_readings "
                "WHERE ts BETWEEN ? AND ? ORDER BY ts", (since, until)).fetchall()
        out = ["timestamp_utc,unix_ts,case_temp_c,case_humidity_pct,dew_point_c,cpu_temp_c"]
        for ts, t, h, cpu in rows:
            iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ts))
            dp = dew_point(t / 100, h / 100)
            out.append(f"{iso},{ts},{t / 100:.2f},{h / 100:.2f},"
                       f"{'' if dp is None else f'{dp:.2f}'},"
                       f"{'' if cpu is None else f'{cpu / 100:.2f}'}")
        return "\n".join(out) + "\n"
