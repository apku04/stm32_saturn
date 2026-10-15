"""Tests for gateway_rx, using packets captured from the RAK2243 on 2026-10-15."""

import base64
import json

import gateway_rx as g

# Real rxpk objects logged by lora_pkt_fwd while nodes 10 and 11 were running.
BEACON_RXPK = {"tmst": 8787763, "chan": 0, "rfch": 1, "freq": 868.0, "stat": 1,
               "modu": "LORA", "datr": "SF7BW125", "codr": "4/5", "lsnr": 9.2,
               "rssi": -97, "size": 42,
               "data": "/woRAAAKBwABCgAqCwsBAACgA6ADAQ4HAAAAAAAAAAAAAAAAABgBABHQ"}
PING_RXPK = dict(BEACON_RXPK, lsnr=10.0, rssi=-99, size=12, data="/woAAAAKAAAACgMM")
PONG_RXPK = dict(BEACON_RXPK, lsnr=6.5, rssi=-110, size=12, data="/wsAAAAKAAAACwQM")


class FakeStore:
    def __init__(self):
        self.rows, self.routes, self.console = [], {}, []

    def insert(self, ts, src, rx, beacon):
        self.rows.append((ts, src, rx, beacon))

    def set_node_routes(self, src, routes):
        self.routes[src] = routes

    def console_add(self, text):
        self.console.append(text)


def test_ping_header():
    pkt = g.decode_rxpk(PING_RXPK)
    assert (pkt["dst"], pkt["src"], pkt["type"], pkt["len"]) == (255, 10, g.APP_TYPES.index("PING"), 12)
    assert "beacon" not in pkt


def test_pong_from_node_11():
    pkt = g.decode_rxpk(PONG_RXPK)
    assert pkt["src"] == 11 and pkt["type"] == g.APP_TYPES.index("PONG")
    assert pkt["rx"]["rssi"] == -110 and pkt["rx"]["snr"] == 6


def test_beacon_v7_fields():
    pkt = g.decode_rxpk(BEACON_RXPK)
    assert pkt["src"] == 10 and pkt["seq"] == 17 and pkt["type"] == g.BEACON
    assert pkt["routes"] == [{"dest": 11, "next_hop": 11, "hops": 1}]
    b = pkt["beacon"]
    assert b["entries"] == 1
    assert (b["i_ma"], b["bus_mv"], b["bat_mv"], b["chg"]) == (0, 928, 928, 1)
    assert (b["tx_pwr"], b["sf"]) == (14, 7)
    assert (b["lat_udeg"], b["lon_udeg"], b["fix"]) == (0, 0, 0)
    assert "rst" in b and "up_min" in b
    assert pkt["rx"]["sf"] == 7 and pkt["rx"]["freq"] == 868000000


def test_crc_fail_dropped():
    assert g.decode_rxpk(dict(PING_RXPK, stat=-1)) is None


def test_short_packet_dropped():
    short = dict(PING_RXPK, data=base64.b64encode(b"\xff\x0a\x00").decode())
    assert g.decode_rxpk(short) is None


def test_beacon_payload_ladder():
    assert g.decode_beacon(b"") == {}
    assert g.decode_beacon(b"\x10\x0e") == {"bat_mv": 3600}
    v4 = g.decode_beacon(bytes([0xF6, 0xFF, 0x10, 0x0E, 0x10, 0x0E, 1, 14, 9]))
    assert v4 == {"i_ma": -10, "bus_mv": 3600, "bat_mv": 3600, "chg": 1, "tx_pwr": 14, "sf": 9}


def test_push_data_ack_and_rxpk():
    body = json.dumps({"rxpk": [PING_RXPK]}).encode()
    dgram = bytes([2, 0xAB, 0xCD, g.PUSH_DATA]) + b"\x00" * 8 + body
    ack, rxpks, stat = g.parse_datagram(dgram)
    assert ack == bytes([2, 0xAB, 0xCD, g.PUSH_ACK])
    assert rxpks == [PING_RXPK] and stat is None


def test_pull_data_ack():
    ack, rxpks, stat = g.parse_datagram(bytes([2, 1, 2, g.PULL_DATA]) + b"\x00" * 8)
    assert ack == bytes([2, 1, 2, g.PULL_ACK]) and rxpks == [] and stat is None


def test_stat_only_push_data():
    body = json.dumps({"stat": {"rxnb": 0}}).encode()
    ack, rxpks, stat = g.parse_datagram(bytes([2, 0, 1, g.PUSH_DATA]) + b"\x00" * 8 + body)
    assert ack is not None and rxpks == [] and stat == {"rxnb": 0}


# Trimmed copy of the RAK global_conf.json, including its /* */ comments.
GLOBAL_CONF = """{
    "SX1301_conf": {
        "lorawan_public": false,
        "radio_0": {"enable": true, "type": "SX1257", "freq": 867500000},
        "radio_1": {"enable": true, "type": "SX1257", "freq": 868400000},
        "chan_multiSF_0": {"enable": true, "radio": 1, "if": -400000},
        "chan_multiSF_1": {"enable": true, "radio": 1, "if": -200000},
        "chan_multiSF_2": {"enable": false, "radio": 1, "if": 0},
        "chan_multiSF_3": {"enable": true, "radio": 0, "if": -400000},
        "chan_Lora_std": {"enable": true, "radio": 1, "if": -200000,
                          "bandwidth": 250000, "spread_factor": 7},
        "chan_FSK": {"enable": true, "radio": 1, "if": 300000,
                     "bandwidth": 125000, "datarate": 50000}
    },
    "gateway_conf": {
        "gateway_ID": "0000000000000000",
        /* change with default server address/ports */
        "server_address": "127.0.0.1"
    }
}"""


def test_load_channel_plan(tmp_path):
    p = tmp_path / "global_conf.json"
    p.write_text(GLOBAL_CONF)
    plan = g.load_channel_plan(str(p))
    assert plan["public"] is False
    got = [(c["chan"], c["freq"], c["kind"]) for c in plan["channels"]]
    assert got == [(0, 868000000, "LoRa multi-SF"), (1, 868200000, "LoRa multi-SF"),
                   (3, 867100000, "LoRa multi-SF"), (8, 868200000, "LoRa std"),
                   (9, 868700000, "FSK")]
    assert plan["channels"][3]["bw"] == 250000 and plan["channels"][3]["sf"] == "SF7"


def test_channel_monitor_counts_and_history():
    plan = {"public": False, "channels": [
        {"chan": 0, "kind": "LoRa multi-SF", "freq": 868000000, "bw": 125000,
         "sf": "SF7-12", "radio": 1, "if": -400000},
        {"chan": 1, "kind": "LoRa multi-SF", "freq": 868200000, "bw": 125000,
         "sf": "SF7-12", "radio": 1, "if": -200000}]}
    m = g.ChannelMonitor(plan, minutes=5)
    now = 600_000
    m.add_rxpk(PING_RXPK, g.decode_rxpk(PING_RXPK), ts=now - 130)
    m.add_rxpk(PONG_RXPK, g.decode_rxpk(PONG_RXPK), ts=now)
    m.add_rxpk(dict(PING_RXPK, stat=-1, data="AAEC"), None, ts=now)
    m.add_stat({"rxnb": 3, "rxok": 2}, ts=now)
    snap = m.snapshot(now=now)
    ch0, ch1 = snap["channels"]
    assert ch1["total"] == 0 and ch1["per_min"] == [0] * 5   # planned but quiet
    assert (ch0["total"], ch0["crc_ok"], ch0["crc_bad"]) == (3, 2, 1)
    assert ch0["sf"] == {"SF7": 3}
    assert (ch0["rssi_min"], ch0["rssi_max"], ch0["rssi_last"]) == (-110, -99, -99)
    assert ch0["per_min"] == [0, 1, 0, 0, 2]     # now-130 s falls 3 minutes back
    newest, pong, ping = snap["recent"]
    assert newest["stat"] == -1 and newest["hex"] == "000102"
    assert pong["src"] == 11 and pong["type"] == "PONG"
    assert snap["stat"]["rxnb"] == 3


def test_channel_monitor_learns_unplanned_channel():
    m = g.ChannelMonitor()
    m.add_rxpk(dict(PING_RXPK, chan=5, freq=867.5), None, ts=1000)
    (ch,) = m.snapshot(now=1000)["channels"]
    assert (ch["chan"], ch["freq"], ch["kind"]) == (5, 867500000, "unplanned")


def test_handle_packet_stores_beacon_and_routes():
    store = FakeStore()
    g.handle_packet(store, g.decode_rxpk(BEACON_RXPK), ts=1000)
    g.handle_packet(store, g.decode_rxpk(PING_RXPK), ts=1001)
    assert len(store.rows) == 1
    ts, src, rx, beacon = store.rows[0]
    assert (ts, src, rx["rssi"], beacon["bus_mv"]) == (1000, 10, -97, 928)
    assert store.routes == {10: [{"dest": 11, "next_hop": 11, "hops": 1}]}
    assert len(store.console) == 2 and store.console[1].startswith("[GW] src=10")
