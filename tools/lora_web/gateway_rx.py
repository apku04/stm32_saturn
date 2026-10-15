"""
SX1301 concentrator input for the Saturn LoRa dashboard.

A RAK2243 (or any SX1301/SX1302 gateway) running the Semtech packet
forwarder sends every LoRa packet it hears as JSON over UDP ("PUSH_DATA",
Semtech UDP protocol v2). This module answers that protocol and decodes the
raw mesh packets into the same dicts the serial parsers in server.py produce,
so beacons from the gateway land in beacons.db exactly like serial ones.

Gateway radio config must match the mesh: 868.0 MHz, BW125, private sync
word (lorawan_public=false). SF5/SF6 are not receivable by the SX1301.

Air layout (see firmware/src/driver/sx1262.c receive_packet):
    0 dst  1 src  2-3 seq (LE)  4 control_mac  5 protocol_Ver  6 TTL
    7 mesh_dest  8 mesh_tbl_entries  9 mesh_src  10 control_app  11 length
    12.. data: mesh_tbl_entries * (dest, next_hop, hops), then app payload
"""

from __future__ import annotations

import base64
import json
import re
import socket
import struct
import threading
import time
from collections import deque

PACKET_HEADER_SIZE = 12

# control_app values (firmware/include/globalInclude.h)
APP_TYPES = ["BEACON", "PAYLOAD", "ACK", "PING", "PONG", "CMD_CFG", "CMD_ACK"]
BEACON = 0

# Semtech UDP protocol identifiers
PROTOCOL_VERSION = 2
PUSH_DATA, PUSH_ACK, PULL_DATA, PULL_RESP, PULL_ACK, TX_ACK = 0, 1, 2, 3, 4, 5


def decode_header(raw: bytes):
    """Return the 12-byte mesh header as a dict, or None if too short."""
    if len(raw) < PACKET_HEADER_SIZE:
        return None
    entries = raw[8]
    if entries > 16:
        entries = 0     # same sanity clamp as the firmware
    return {
        "dst": raw[0], "src": raw[1],
        "seq": raw[2] | (raw[3] << 8),
        "control_mac": raw[4], "protocol_ver": raw[5], "ttl": raw[6],
        "mesh_dest": raw[7], "mesh_tbl_entries": entries,
        "mesh_src": raw[9], "type": raw[10], "len": raw[11],
    }


def decode_beacon(data: bytes):
    """Decode a beacon's app payload (after the routing table).

    Mirrors the length-based version ladder in main.c app_incoming, so the
    returned keys match server.parse_beacon. Returns {} if no telemetry.
    """
    n = len(data)
    if n >= 7:
        i_ma, bus, bat, chg = struct.unpack_from("<hHHB", data, 0)
        b = {"i_ma": i_ma, "bus_mv": bus, "bat_mv": bat, "chg": chg}
    elif n >= 5:
        bat, _sol, chg = struct.unpack_from("<HHB", data, 0)
        return {"bat_mv": bat, "chg": chg}
    elif n >= 2:
        return {"bat_mv": struct.unpack_from("<H", data, 0)[0]}
    else:
        return {}
    if n >= 9:
        b["tx_pwr"], b["sf"] = data[7], data[8]
    if n >= 18:
        b["lat_udeg"], b["lon_udeg"] = struct.unpack_from("<ii", data, 9)
        b["fix"] = data[17]
    if n >= 22:
        b["temp_cdeg"], b["hum_cpct"] = struct.unpack_from("<hH", data, 18)
    if n >= 26:
        b["rst"], b["boot"], b["last_stage"], b["up_min"] = data[22:26]
    return b


def decode_packet(raw: bytes):
    """Decode a full mesh packet: header, routing table and beacon fields."""
    hdr = decode_header(raw)
    if hdr is None:
        return None
    end = min(len(raw), max(hdr["len"], PACKET_HEADER_SIZE))
    data = raw[PACKET_HEADER_SIZE:end]
    n_tbl = hdr["mesh_tbl_entries"]
    routes = [
        {"dest": data[i * 3], "next_hop": data[i * 3 + 1], "hops": data[i * 3 + 2]}
        for i in range(n_tbl) if i * 3 + 2 < len(data)
    ]
    pkt = dict(hdr, routes=routes, data=data)
    if hdr["type"] == BEACON:
        b = decode_beacon(data[n_tbl * 3:])
        b["entries"] = n_tbl
        pkt["beacon"] = b
    return pkt


def decode_rxpk(rxpk: dict):
    """Decode one Semtech `rxpk` object into a mesh packet + radio metadata.

    Returns None for packets that failed CRC or don't look like mesh packets.
    """
    if rxpk.get("stat") != 1 or rxpk.get("modu") != "LORA":
        return None
    try:
        raw = base64.b64decode(rxpk.get("data", ""))
    except (ValueError, TypeError):
        return None
    pkt = decode_packet(raw)
    if pkt is None:
        return None
    datr = rxpk.get("datr", "")
    sf = None
    if datr.startswith("SF") and "BW" in datr:
        try:
            sf = int(datr[2:datr.index("BW")])
        except ValueError:
            pass
    rssi = rxpk.get("rssis", rxpk.get("rssi"))
    snr = rxpk.get("lsnr")
    pkt["rx"] = {
        "src": pkt["src"], "dst": pkt["dst"],
        "rssi": int(rssi) if rssi is not None else None,
        "prssi": None,
        "snr": int(round(snr)) if snr is not None else None,
        "sf": sf,
        "freq": int(round(float(rxpk["freq"]) * 1e6)) if "freq" in rxpk else None,
        "type": pkt["type"], "seq": pkt["seq"], "len": pkt["len"],
    }
    return pkt


def parse_datagram(dgram: bytes):
    """Parse a forwarder datagram.

    Returns (ack_bytes_or_None, [rxpk, ...], stat_or_None). PUSH_DATA gets a
    PUSH_ACK plus its rxpk list and periodic `stat` object; PULL_DATA gets a
    PULL_ACK (keeps the forwarder happy).
    """
    if len(dgram) < 4 or dgram[0] != PROTOCOL_VERSION:
        return None, [], None
    token, ident = dgram[1:3], dgram[3]
    if ident == PUSH_DATA and len(dgram) >= 12:
        ack = bytes([PROTOCOL_VERSION]) + token + bytes([PUSH_ACK])
        try:
            body = json.loads(dgram[12:].decode("utf-8", errors="replace"))
        except ValueError:
            return ack, [], None
        return ack, list(body.get("rxpk") or []), body.get("stat")
    if ident == PULL_DATA:
        return bytes([PROTOCOL_VERSION]) + token + bytes([PULL_ACK]), [], None
    return None, [], None


# ---------------------------------------------------------------- Channel monitor
# SX1301 IF chains: 0-7 multi-SF LoRa, 8 = single-SF "LoRa std", 9 = FSK.
CHAN_LORA_STD, CHAN_FSK = 8, 9


def load_channel_plan(path: str):
    """Read the listening channels from a packet-forwarder global_conf.json.

    The Semtech/RAK files contain /* */ comments, so strip them before parsing.
    Returns {"public": bool, "channels": [ {chan, kind, freq, bw, sf, radio, if}, ...]}.
    """
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"^\s*//.*$", "", text, flags=re.M)
    s = json.loads(text)["SX1301_conf"]
    radios = {i: s.get(f"radio_{i}", {}) for i in (0, 1)}
    chans = []

    def add(chan, kind, c, bw, sf):
        if not c or not c.get("enable"):
            return
        r = radios.get(c.get("radio"), {})
        if not r.get("enable", True):
            return
        chans.append({"chan": chan, "kind": kind, "freq": r.get("freq", 0) + c.get("if", 0),
                      "bw": bw, "sf": sf, "radio": c.get("radio"), "if": c.get("if", 0)})

    for i in range(8):
        add(i, "LoRa multi-SF", s.get(f"chan_multiSF_{i}"), 125000, "SF7-12")
    std = s.get("chan_Lora_std") or {}
    add(CHAN_LORA_STD, "LoRa std", std, std.get("bandwidth", 250000),
        f"SF{std.get('spread_factor', '?')}")
    fsk = s.get("chan_FSK") or {}
    add(CHAN_FSK, "FSK", fsk, fsk.get("bandwidth", 125000),
        f"{fsk.get('datarate', 0) // 1000} kbps")
    return {"public": bool(s.get("lorawan_public")), "channels": chans}


class ChannelMonitor:
    """Per-channel RX statistics for every packet the concentrator reports.

    Unlike the beacon store this keeps CRC failures and non-mesh packets too,
    so it shows what is actually on air on each listening channel.
    """

    def __init__(self, plan=None, history: int = 300, minutes: int = 60):
        self._lock = threading.Lock()
        self.plan = plan or {"public": None, "channels": []}
        self.minutes = minutes
        self.started = int(time.time())
        self._chans = {}
        self._recent = deque(maxlen=history)
        self._stat = None
        for c in self.plan["channels"]:
            self._chan(c["chan"], c["freq"])

    def _chan(self, chan, freq):
        c = self._chans.get(chan)
        if c is None:
            c = self._chans[chan] = {
                "chan": chan, "freq": freq, "total": 0, "crc_ok": 0, "crc_bad": 0,
                "no_crc": 0, "sf": {}, "rssi_last": None, "snr_last": None,
                "rssi_min": None, "rssi_max": None, "rssi_sum": 0, "snr_sum": 0.0,
                "n_sig": 0, "last_ts": None, "buckets": {},
            }
        return c

    def add_rxpk(self, rxpk: dict, pkt: dict | None = None, ts: int | None = None):
        ts = int(ts or time.time())
        chan = rxpk.get("chan")
        freq = int(round(float(rxpk.get("freq", 0)) * 1e6))
        if chan is None:
            chan = freq     # unknown IF chain: key by frequency instead
        stat = rxpk.get("stat")
        rssi = rxpk.get("rssis", rxpk.get("rssi"))
        snr = rxpk.get("lsnr")
        datr = rxpk.get("datr")
        sf = datr if isinstance(datr, str) else f"{datr} bps"
        if isinstance(datr, str) and datr.startswith("SF") and "BW" in datr:
            sf = datr[:datr.index("BW")]
        with self._lock:
            c = self._chan(chan, freq)
            c["total"] += 1
            if stat == 1:
                c["crc_ok"] += 1
            elif stat == -1:
                c["crc_bad"] += 1
            else:
                c["no_crc"] += 1
            c["sf"][sf] = c["sf"].get(sf, 0) + 1
            if rssi is not None:
                c["rssi_last"] = rssi
                c["rssi_min"] = rssi if c["rssi_min"] is None else min(c["rssi_min"], rssi)
                c["rssi_max"] = rssi if c["rssi_max"] is None else max(c["rssi_max"], rssi)
                c["rssi_sum"] += rssi
                c["n_sig"] += 1
                if snr is not None:
                    c["snr_last"] = snr
                    c["snr_sum"] += snr
            c["last_ts"] = ts
            minute = ts // 60
            c["buckets"][minute] = c["buckets"].get(minute, 0) + 1
            for m in [m for m in c["buckets"] if m <= minute - self.minutes]:
                del c["buckets"][m]
            entry = {"ts": ts, "chan": chan, "freq": freq, "sf": sf, "rssi": rssi,
                     "snr": snr, "size": rxpk.get("size"), "stat": stat}
            if pkt is not None:
                t = pkt["type"]
                entry.update(src=pkt["src"], dst=pkt["dst"], seq=pkt["seq"],
                             type=APP_TYPES[t] if t < len(APP_TYPES) else str(t))
            else:
                try:
                    entry["hex"] = base64.b64decode(rxpk.get("data", ""))[:16].hex()
                except (ValueError, TypeError):
                    pass
            self._recent.append(entry)

    def add_stat(self, stat: dict, ts: int | None = None):
        with self._lock:
            self._stat = dict(stat, rx_ts=int(ts or time.time()))

    def snapshot(self, now: int | None = None):
        now = int(now or time.time())
        cur = now // 60
        planned = {c["chan"]: c for c in self.plan["channels"]}
        out = []
        with self._lock:
            for key in sorted(self._chans, key=lambda k: (k not in planned, k)):
                c = self._chans[key]
                p = planned.get(key, {})
                n = c["n_sig"]
                out.append({
                    "chan": c["chan"], "freq": p.get("freq", c["freq"]),
                    "kind": p.get("kind", "unplanned"), "bw": p.get("bw"),
                    "sf_range": p.get("sf"), "radio": p.get("radio"),
                    "total": c["total"], "crc_ok": c["crc_ok"], "crc_bad": c["crc_bad"],
                    "no_crc": c["no_crc"], "sf": dict(c["sf"]),
                    "rssi_last": c["rssi_last"], "snr_last": c["snr_last"],
                    "rssi_min": c["rssi_min"], "rssi_max": c["rssi_max"],
                    "rssi_avg": round(c["rssi_sum"] / n, 1) if n else None,
                    "snr_avg": round(c["snr_sum"] / n, 1) if n else None,
                    "last_ts": c["last_ts"],
                    "per_min": [c["buckets"].get(m, 0)
                                for m in range(cur - self.minutes + 1, cur + 1)],
                })
            recent = list(self._recent)[-100:][::-1]
            stat = dict(self._stat) if self._stat else None
        return {"public": self.plan.get("public"), "channels": out, "recent": recent,
                "stat": stat, "started": self.started, "minutes": self.minutes,
                "now": now}


def describe(pkt: dict) -> str:
    """One-line console text, in the same spirit as the firmware [RX] line."""
    rx = pkt["rx"]
    t = pkt["type"]
    name = APP_TYPES[t] if t < len(APP_TYPES) else str(t)
    return (f"[GW] src={pkt['src']} dst={pkt['dst']} type={name} seq={pkt['seq']} "
            f"len={pkt['len']} rssi={rx['rssi']} snr={rx['snr']} sf={rx['sf']} "
            f"freq={rx['freq']}")


def handle_packet(store, pkt: dict, ts: int | None = None):
    """Feed one decoded packet into the dashboard Store."""
    store.console_add(describe(pkt))
    if pkt["routes"] and pkt["type"] == BEACON:
        store.set_node_routes(pkt["src"], pkt["routes"])
    if "beacon" in pkt:
        store.insert(ts or int(time.time()), pkt["src"], pkt["rx"], pkt["beacon"])


def udp_reader(bind: str, port: int, store, stop_event: threading.Event,
               monitor: ChannelMonitor | None = None):
    """Thread body: serve the Semtech UDP protocol and feed the Store."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind, port))
    sock.settimeout(1.0)
    print(f"[gateway] listening for packet forwarder on udp {bind}:{port}", flush=True)
    store.console_add(f"[gateway] listening on udp {bind}:{port}")
    while not stop_event.is_set():
        try:
            dgram, addr = sock.recvfrom(65535)
        except socket.timeout:
            continue
        except OSError as e:
            print(f"[gateway] recv error: {e}", flush=True)
            stop_event.wait(1.0)
            continue
        ack, rxpks, stat = parse_datagram(dgram)
        if ack:
            try:
                sock.sendto(ack, addr)
            except OSError:
                pass
        if monitor is not None and stat:
            monitor.add_stat(stat)
        for rxpk in rxpks:
            pkt = decode_rxpk(rxpk)
            if monitor is not None:
                monitor.add_rxpk(rxpk, pkt)
            if pkt is None:
                continue
            try:
                handle_packet(store, pkt)
                print(describe(pkt), flush=True)
            except Exception as e:
                print(f"[gateway] store failed: {e}", flush=True)
    sock.close()
