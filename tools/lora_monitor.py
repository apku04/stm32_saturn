#!/usr/bin/env python3
"""
Saturn LoRa Monitor — Serial packet monitor GUI
Works with both STM32 (VID 0483) and PIC24 (VID 04D8) LoRa boards.
Cross-platform: Linux, macOS, Windows.

Requirements: pip install pyserial
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
import threading
import re
import time
from datetime import datetime

import serial
import serial.tools.list_ports

# Device USB identifiers
KNOWN_DEVICES = {
    0x0483: {"name": "STM32", "pid": 0x5740},
    0x04D8: {"name": "PIC24", "pid": None},
}
BAUD_RATE = 115200

CHARGE_STATUS = {
    "0": "Off",
    "1": "Charging",
    "2": "Done",
    "3": "Fault",
}

PKT_TYPES = {
    "0": "BEACON",
    "1": "PAYLOAD",
    "2": "ACK",
    "3": "PING",
    "4": "PONG",
}

# Regex patterns for parsing firmware output
RX_PATTERN = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+prssi=(-?\d+)\s+"
    r"type=(\d+)\s+seq=(\d+)\s+len=(\d+)"
)
# Also match older firmware without prssi/seq
RX_PATTERN_LEGACY = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+type=(\d+)\s+len=(\d+)"
)
BEACON_PATTERN_V3 = re.compile(
    r"\[BEACON\]\s+i_ma=(-?\d+)\s+bus=(\d+)\s+bat=(\d+)\s+chg=(\d+)\s+entries=(\d+)"
)
BEACON_PATTERN_V2 = re.compile(
    r"\[BEACON\]\s+shunt=(-?\d+)\s+bus=(\d+)\s+bat=(\d+)\s+chg=(\d+)\s+entries=(\d+)"
)
BEACON_PATTERN = re.compile(
    r"\[BEACON\]\s+bat=(\d+)\s+sol=(\d+)\s+chg=(\d+)\s+entries=(\d+)"
)
BEACON_PATTERN_LEGACY = re.compile(
    r"\[BEACON\]\s+bat=(\d+)\s+entries=(\d+)"
)
BEACON_PATTERN_MINIMAL = re.compile(
    r"\[BEACON\]\s+entries=(\d+)"
)


class LoRaMonitor:
    def __init__(self, root):
        self.root = root
        self.root.title("Saturn LoRa Monitor")
        self.root.geometry("1050x720")
        self.root.minsize(800, 500)

        self.serial_port = None
        self.reader_thread = None
        self.running = False
        self.last_rx = {}  # src -> last RX info for beacon panel

        self._build_ui()
        self._refresh_ports()

    # ------------------------------------------------------------------ UI
    def _build_ui(self):
        # Top bar: port selection + connect
        top = ttk.Frame(self.root, padding=4)
        top.pack(fill=tk.X)

        ttk.Label(top, text="Port:").pack(side=tk.LEFT)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=30, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=(4, 8))

        self.refresh_btn = ttk.Button(top, text="Refresh", command=self._refresh_ports)
        self.refresh_btn.pack(side=tk.LEFT, padx=2)

        self.connect_btn = ttk.Button(top, text="Connect", command=self._toggle_connect)
        self.connect_btn.pack(side=tk.LEFT, padx=2)

        self.status_lbl = ttk.Label(top, text="Disconnected", foreground="gray")
        self.status_lbl.pack(side=tk.LEFT, padx=12)

        # Notebook with tabs
        nb = ttk.Notebook(self.root)
        nb.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        # --- Tab 1: Packet Table ---
        pkt_frame = ttk.Frame(nb)
        nb.add(pkt_frame, text="Packets")

        cols = ("time", "src", "dst", "type", "seq", "rssi", "prssi", "len", "info")
        self.pkt_tree = ttk.Treeview(pkt_frame, columns=cols, show="headings", height=14)
        for c, w in zip(cols, (70, 40, 40, 70, 50, 50, 50, 40, 380)):
            self.pkt_tree.heading(c, text=c.upper())
            self.pkt_tree.column(c, width=w, minwidth=30)
        self.pkt_tree.pack(fill=tk.BOTH, expand=True, side=tk.LEFT)
        sb = ttk.Scrollbar(pkt_frame, orient=tk.VERTICAL, command=self.pkt_tree.yview)
        sb.pack(fill=tk.Y, side=tk.RIGHT)
        self.pkt_tree.configure(yscrollcommand=sb.set)

        # --- Tab 2: Beacon Dashboard ---
        bcn_frame = ttk.Frame(nb, padding=8)
        nb.add(bcn_frame, text="Beacons")

        bcn_cols = ("time", "src", "rssi", "prssi", "bus_v", "i_ma", "p_mw", "charge", "entries")
        self.bcn_tree = ttk.Treeview(bcn_frame, columns=bcn_cols, show="headings", height=14)
        for c, w in zip(bcn_cols, (70, 50, 50, 50, 80, 80, 80, 80, 60)):
            self.bcn_tree.heading(c, text=c.upper().replace("_", " "))
            self.bcn_tree.column(c, width=w, minwidth=30)
        self.bcn_tree.pack(fill=tk.BOTH, expand=True, side=tk.LEFT)
        sb2 = ttk.Scrollbar(bcn_frame, orient=tk.VERTICAL, command=self.bcn_tree.yview)
        sb2.pack(fill=tk.Y, side=tk.RIGHT)
        self.bcn_tree.configure(yscrollcommand=sb2.set)

        # --- Tab 3: Raw Log ---
        log_frame = ttk.Frame(nb)
        nb.add(log_frame, text="Log")

        self.log_text = scrolledtext.ScrolledText(log_frame, wrap=tk.WORD, font=("Consolas", 10))
        self.log_text.pack(fill=tk.BOTH, expand=True)

        # Bottom: command entry
        bot = ttk.Frame(self.root, padding=4)
        bot.pack(fill=tk.X)

        ttk.Label(bot, text="Cmd:").pack(side=tk.LEFT)
        self.cmd_var = tk.StringVar()
        self.cmd_entry = ttk.Entry(bot, textvariable=self.cmd_var, width=50)
        self.cmd_entry.pack(side=tk.LEFT, padx=4)
        self.cmd_entry.bind("<Return>", lambda e: self._send_command())
        self.send_btn = ttk.Button(bot, text="Send", command=self._send_command)
        self.send_btn.pack(side=tk.LEFT, padx=2)

        # Quick buttons
        for label, cmd in [("Get Battery", "get battery"), ("Get Solar", "get solar"),
                           ("Get Charge", "get charge"), ("Get Routing", "get routing"),
                           ("Ping", "ping")]:
            ttk.Button(bot, text=label, command=lambda c=cmd: self._send_raw(c)).pack(side=tk.LEFT, padx=2)

    # --------------------------------------------------------- Port logic
    def _refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        items = []
        for p in sorted(ports, key=lambda x: x.device):
            tag = ""
            if p.vid:
                for vid, info in KNOWN_DEVICES.items():
                    if p.vid == vid:
                        tag = f" [{info['name']}]"
                        break
            items.append(f"{p.device}{tag}")
        self.port_combo["values"] = items
        if items:
            # Prefer known devices
            for i, item in enumerate(items):
                if "[STM32]" in item or "[PIC24]" in item:
                    self.port_combo.current(i)
                    break
            else:
                self.port_combo.current(0)

    def _get_selected_port(self):
        val = self.port_var.get()
        return val.split(" ")[0] if val else None

    def _toggle_connect(self):
        if self.running:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self._get_selected_port()
        if not port:
            return
        try:
            self.serial_port = serial.Serial(port, BAUD_RATE, timeout=0.1)
        except serial.SerialException as e:
            self._log(f"Error: {e}\n")
            return
        self.running = True
        self.connect_btn.config(text="Disconnect")
        self.status_lbl.config(text=f"Connected: {port}", foreground="green")
        self.port_combo.config(state="disabled")
        self.refresh_btn.config(state="disabled")
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

    def _disconnect(self):
        self.running = False
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        self.serial_port = None
        self.connect_btn.config(text="Connect")
        self.status_lbl.config(text="Disconnected", foreground="gray")
        self.port_combo.config(state="readonly")
        self.refresh_btn.config(state="normal")

    # -------------------------------------------------------- Serial read
    def _reader_loop(self):
        buf = ""
        pending_rx = None  # last parsed [RX] waiting for potential [BEACON] line

        while self.running:
            try:
                if not self.serial_port or not self.serial_port.is_open:
                    break
                raw = self.serial_port.read(512)
                if not raw:
                    # If we have a pending RX without beacon follow-up, flush it
                    if pending_rx:
                        self.root.after(0, self._add_rx_packet, pending_rx, None)
                        pending_rx = None
                    continue
                text = raw.decode("ascii", errors="replace")
                buf += text

                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue

                    self.root.after(0, self._log, line + "\n")

                    # Check for [BEACON] line (follows a [RX] with type=0)
                    bcn_v3 = BEACON_PATTERN_V3.match(line)
                    bcn_v2 = BEACON_PATTERN_V2.match(line) if not bcn_v3 else None
                    bcn = BEACON_PATTERN.match(line) if not bcn_v3 and not bcn_v2 else None
                    bcn_leg = BEACON_PATTERN_LEGACY.match(line) if not bcn_v3 and not bcn_v2 and not bcn else None
                    bcn_min = BEACON_PATTERN_MINIMAL.match(line) if not bcn_v3 and not bcn_v2 and not bcn and not bcn_leg else None
                    if bcn_v3 or bcn_v2 or bcn or bcn_leg or bcn_min:
                        beacon_data = self._parse_beacon(bcn_v3 or bcn_v2 or bcn or bcn_leg or bcn_min, is_v3=bool(bcn_v3))
                        if pending_rx:
                            self.root.after(0, self._add_rx_packet, pending_rx, beacon_data)
                            pending_rx = None
                        continue

                    # Flush any pending RX that wasn't followed by [BEACON]
                    if pending_rx:
                        self.root.after(0, self._add_rx_packet, pending_rx, None)
                        pending_rx = None

                    # Check for [RX] line
                    m = RX_PATTERN.match(line)
                    if m:
                        pending_rx = {
                            "src": m.group(1), "dst": m.group(2),
                            "rssi": m.group(3), "prssi": m.group(4),
                            "type": m.group(5), "seq": m.group(6),
                            "len": m.group(7),
                        }
                        continue

                    m = RX_PATTERN_LEGACY.match(line)
                    if m:
                        pending_rx = {
                            "src": m.group(1), "dst": m.group(2),
                            "rssi": m.group(3), "prssi": "—",
                            "type": m.group(4), "seq": "—",
                            "len": m.group(5),
                        }
                        continue

            except (serial.SerialException, OSError):
                self.root.after(0, self._disconnect)
                break
            except Exception:
                pass

    def _parse_beacon(self, m, is_v3=False):
        groups = m.groups()
        if is_v3 and len(groups) == 5:
            # V3: i_ma, bus, bat, chg, entries (current already in mA on the wire)
            return {"i_ma": groups[0], "bus": groups[1], "bat": groups[2],
                    "chg": groups[3], "entries": groups[4]}
        if len(groups) == 5:
            # V2: shunt(mV), bus, bat, chg, entries  — derive i_ma assuming 50mΩ shunt
            return {"i_ma": str(int(groups[0]) * 20), "bus": groups[1], "bat": groups[2],
                    "chg": groups[3], "entries": groups[4]}
        elif len(groups) == 4:
            return {"i_ma": "—", "bus": groups[1], "bat": groups[0],
                    "chg": groups[2], "entries": groups[3]}
        elif len(groups) == 2:
            return {"i_ma": "—", "bus": "—", "bat": groups[0],
                    "chg": "—", "entries": groups[1]}
        else:
            return {"i_ma": "—", "bus": "—", "bat": "—",
                    "chg": "—", "entries": groups[0]}

    # ---------------------------------------------------- Packet display
    def _add_rx_packet(self, rx, beacon):
        ts = datetime.now().strftime("%H:%M:%S")
        ptype = PKT_TYPES.get(rx["type"], rx["type"])

        info = ""
        if beacon:
            bus_v = f"{int(beacon['bus'])/1000:.2f}V" if beacon["bus"] != "—" else "—"
            chg_s = CHARGE_STATUS.get(beacon["chg"], beacon["chg"])
            if beacon["i_ma"] != "—" and beacon["bus"] != "—":
                i_ma_val = int(beacon["i_ma"])
                p_mw_val = (int(beacon["bus"]) * i_ma_val) // 1000
                i_ma = f"{i_ma_val} mA"
                p_mw = f"{p_mw_val} mW"
            else:
                i_ma = "—"
                p_mw = "—"
            info = f"Bus={bus_v}  I={i_ma}  P={p_mw}  Charge={chg_s}  Routes={beacon['entries']}"

            # Add to beacon tree
            self.bcn_tree.insert("", 0, values=(
                ts, rx["src"], rx["rssi"], rx["prssi"],
                bus_v, i_ma, p_mw, chg_s, beacon["entries"]
            ))
            # Trim beacon tree
            children = self.bcn_tree.get_children()
            if len(children) > 200:
                for c in children[200:]:
                    self.bcn_tree.delete(c)

        self.pkt_tree.insert("", 0, values=(
            ts, rx["src"], rx["dst"], ptype, rx["seq"],
            rx["rssi"], rx["prssi"], rx["len"], info
        ))
        # Trim packet tree
        children = self.pkt_tree.get_children()
        if len(children) > 500:
            for c in children[500:]:
                self.pkt_tree.delete(c)

    # ---------------------------------------------------------- Commands
    def _send_command(self):
        cmd = self.cmd_var.get().strip()
        if cmd:
            self._send_raw(cmd)
            self.cmd_var.set("")

    def _send_raw(self, cmd):
        if self.serial_port and self.serial_port.is_open:
            try:
                self.serial_port.write((cmd + "\r\n").encode())
                self._log(f"> {cmd}\n")
            except serial.SerialException as e:
                self._log(f"Send error: {e}\n")

    # --------------------------------------------------------------- Log
    def _log(self, text):
        self.log_text.insert(tk.END, text)
        self.log_text.see(tk.END)
        # Trim log
        line_count = int(self.log_text.index("end-1c").split(".")[0])
        if line_count > 2000:
            self.log_text.delete("1.0", f"{line_count - 2000}.0")


def main():
    root = tk.Tk()
    LoRaMonitor(root)
    root.mainloop()


if __name__ == "__main__":
    main()
