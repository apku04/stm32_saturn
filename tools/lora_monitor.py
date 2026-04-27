#!/usr/bin/env python3
"""
Saturn LoRa Monitor — Serial packet monitor GUI
Works with both STM32 (VID 0483) and PIC24 (VID 04D8) LoRa boards.
Cross-platform: Linux, macOS, Windows.

Requirements: pip install pyserial
Optional map tab: pip install tkintermapview  (Windows: py -m pip install tkintermapview)
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import threading
import re
import time
import webbrowser
from datetime import datetime

import serial
import serial.tools.list_ports

try:
    import tkintermapview
except ImportError:
    tkintermapview = None

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
    "5": "CMD_CFG",
    "6": "CMD_ACK",
}

# Regex patterns for parsing firmware output
# v2 format adds snr/sf/freq (range-test build)
RX_PATTERN_V2 = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+prssi=(-?\d+)\s+"
    r"snr=(-?\d+)\s+sf=(\d+)\s+freq=(\d+)\s+type=(\d+)\s+seq=(\d+)\s+len=(\d+)"
)
RX_PATTERN = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+prssi=(-?\d+)\s+"
    r"type=(\d+)\s+seq=(\d+)\s+len=(\d+)"
)
# Also match older firmware without prssi/seq
RX_PATTERN_LEGACY = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+type=(\d+)\s+len=(\d+)"
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
CMD_ACK_PATTERN = re.compile(
    r"\[CMD_ACK\]\s+from=(\d+)\s+op=(\d+)\s+status=(\d+)\s+tx_pwr=(\d+)\s+sf=(\d+)"
)

CFG_OPS = {
    "1": "TX power",
    "2": "Spreading factor",
    "3": "TX power + SF",
}


class LoRaMonitor:
    def __init__(self, root):
        self.root = root
        self.root.title("Saturn LoRa Monitor")
        self.root.geometry("1050x720")
        self.root.minsize(800, 500)

        self.serial_port = None
        self.reader_thread = None
        self.running = False
        self.packet_count = 0
        self.beacon_count = 0
        self.last_rx = {}  # src -> last RX info for beacon panel
        self.gps_nodes = {}  # src -> latest GPS fix dict
        self.last_fix_time = {}  # src -> timestamp string of last valid fix
        self.map_widget = None
        self.map_markers = {}
        self.latest_map_url = None
        self.last_remote_command = ""
        self.remote_ack_count = 0

        self._setup_styles()
        self._build_ui()
        self._refresh_ports()

    # ------------------------------------------------------------------ UI
    def _setup_styles(self):
        self.root.geometry("1180x760")
        self.root.minsize(980, 620)

        self.colors = {
            "bg": "#f5f7fb",
            "panel": "#ffffff",
            "line": "#d8dee9",
            "text": "#1f2937",
            "muted": "#667085",
            "accent": "#2563eb",
            "good": "#047857",
            "bad": "#b42318",
            "warn": "#b54708",
            "row_alt": "#f8fafc",
            "row_selected": "#dbeafe",
        }
        self.root.configure(bg=self.colors["bg"])

        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

        default_font = ("Segoe UI", 10)
        mono_font = ("Consolas", 10)
        self.root.option_add("*Font", default_font)

        style.configure("TFrame", background=self.colors["bg"])
        style.configure("Panel.TFrame", background=self.colors["panel"], relief="solid", borderwidth=1)
        style.configure("TLabel", background=self.colors["bg"], foreground=self.colors["text"])
        style.configure("Panel.TLabel", background=self.colors["panel"], foreground=self.colors["text"])
        style.configure("Title.TLabel", font=("Segoe UI", 17, "bold"), foreground=self.colors["text"])
        style.configure("Muted.TLabel", foreground=self.colors["muted"])
        style.configure("Metric.TLabel", font=("Segoe UI", 10, "bold"), foreground=self.colors["text"])
        style.configure("Good.TLabel", foreground=self.colors["good"], font=("Segoe UI", 10, "bold"))
        style.configure("Bad.TLabel", foreground=self.colors["bad"], font=("Segoe UI", 10, "bold"))
        style.configure("TButton", padding=(10, 5))
        style.configure("Accent.TButton", foreground="#ffffff", background=self.colors["accent"], padding=(12, 6))
        style.map("Accent.TButton", background=[("active", "#1d4ed8"), ("disabled", "#93c5fd")])
        style.configure("TNotebook", background=self.colors["bg"], borderwidth=0)
        style.configure("TNotebook.Tab", padding=(14, 7), font=("Segoe UI", 10, "bold"))
        style.map("TNotebook.Tab", background=[("selected", self.colors["panel"]), ("!selected", "#e5e7eb")])
        style.configure("Treeview", font=mono_font, rowheight=26, fieldbackground=self.colors["panel"], background=self.colors["panel"], foreground=self.colors["text"], borderwidth=0)
        style.configure("Treeview.Heading", font=("Segoe UI", 9, "bold"), background="#e5e7eb", foreground="#374151", padding=(4, 6))
        style.map("Treeview", background=[("selected", self.colors["row_selected"])], foreground=[("selected", self.colors["text"])])
        style.configure("TLabelframe", background=self.colors["bg"], bordercolor=self.colors["line"], padding=8)
        style.configure("TLabelframe.Label", background=self.colors["bg"], foreground=self.colors["muted"], font=("Segoe UI", 9, "bold"))

    def _build_ui(self):
        main = ttk.Frame(self.root, padding=(12, 10, 12, 10))
        main.pack(fill=tk.BOTH, expand=True)

        # Header: product identity + live connection status
        header = ttk.Frame(main)
        header.pack(fill=tk.X, pady=(0, 10))
        title_box = ttk.Frame(header)
        title_box.pack(side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Label(title_box, text="Saturn LoRa Monitor", style="Title.TLabel").pack(anchor=tk.W)
        ttk.Label(title_box, text="Serial packet monitor for STM32 and PIC24 LoRa boards", style="Muted.TLabel").pack(anchor=tk.W)

        self.status_lbl = ttk.Label(header, text="Disconnected", style="Bad.TLabel")
        self.status_lbl.pack(side=tk.RIGHT, padx=(12, 0))

        # Connection controls
        conn = ttk.Frame(main, style="Panel.TFrame", padding=(10, 8))
        conn.pack(fill=tk.X, pady=(0, 10))

        ttk.Label(conn, text="Port", style="Panel.TLabel").pack(side=tk.LEFT)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn, textvariable=self.port_var, width=34, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=(8, 8))

        self.refresh_btn = ttk.Button(conn, text="Refresh", command=self._refresh_ports)
        self.refresh_btn.pack(side=tk.LEFT, padx=2)

        self.connect_btn = ttk.Button(conn, text="Connect", style="Accent.TButton", command=self._toggle_connect)
        self.connect_btn.pack(side=tk.LEFT, padx=2)

        ttk.Label(conn, text=f"Baud {BAUD_RATE}", style="Panel.TLabel").pack(side=tk.RIGHT, padx=(10, 0))

        # Notebook with tabs
        nb = ttk.Notebook(main)
        nb.pack(fill=tk.BOTH, expand=True)

        # --- Tab 1: Packet Table ---
        pkt_frame = ttk.Frame(nb, padding=8)
        nb.add(pkt_frame, text="Packets")

        pkt_summary = ttk.Frame(pkt_frame)
        pkt_summary.pack(fill=tk.X, pady=(0, 8))
        self.packet_metric = ttk.Label(pkt_summary, text="0 packets", style="Metric.TLabel")
        self.packet_metric.pack(side=tk.LEFT)
        self.beacon_metric = ttk.Label(pkt_summary, text="0 beacons", style="Metric.TLabel")
        self.beacon_metric.pack(side=tk.LEFT, padx=(18, 0))
        self.last_packet_metric = ttk.Label(pkt_summary, text="Last RX: --", style="Muted.TLabel")
        self.last_packet_metric.pack(side=tk.LEFT, padx=(18, 0))
        ttk.Button(pkt_summary, text="Copy info", command=self._copy_selected_packet_info).pack(side=tk.RIGHT)
        ttk.Button(pkt_summary, text="Clear", command=self._clear_packets).pack(side=tk.RIGHT, padx=(0, 6))

        # Top: tree + vertical scrollbar
        pkt_top = ttk.Frame(pkt_frame)
        pkt_top.pack(fill=tk.BOTH, expand=True)

        cols = ("time", "src", "dst", "type", "seq", "rssi", "prssi", "snr", "sf", "len", "info")
        self.pkt_tree = ttk.Treeview(pkt_top, columns=cols, show="headings", height=14)
        for c, w in zip(cols, (70, 40, 40, 70, 50, 50, 50, 40, 30, 40, 600)):
            self.pkt_tree.heading(c, text=c.upper())
            self.pkt_tree.column(c, width=w, minwidth=30, stretch=(c == "info"))
        self.pkt_tree.pack(fill=tk.BOTH, expand=True, side=tk.LEFT)
        self.pkt_tree.tag_configure("odd", background=self.colors["row_alt"])
        self.pkt_tree.tag_configure("BEACON", foreground=self.colors["good"])
        self.pkt_tree.tag_configure("ACK", foreground=self.colors["accent"])
        self.pkt_tree.tag_configure("CMD_CFG", foreground=self.colors["warn"])
        self.pkt_tree.tag_configure("CMD_ACK", foreground=self.colors["warn"])
        sb = ttk.Scrollbar(pkt_top, orient=tk.VERTICAL, command=self.pkt_tree.yview)
        sb.pack(fill=tk.Y, side=tk.RIGHT)
        hsb = ttk.Scrollbar(pkt_frame, orient=tk.HORIZONTAL, command=self.pkt_tree.xview)
        hsb.pack(fill=tk.X)
        self.pkt_tree.configure(yscrollcommand=sb.set, xscrollcommand=hsb.set)

        # Details pane: shows full INFO of the currently selected packet
        det_frame = ttk.LabelFrame(pkt_frame, text="Selected packet info", padding=4)
        det_frame.pack(fill=tk.X, padx=2, pady=(4, 2))
        self.pkt_detail = tk.Text(det_frame, height=4, wrap=tk.WORD, font=("Consolas", 10))
        self.pkt_detail.pack(fill=tk.X, expand=True)
        self.pkt_detail.configure(state=tk.DISABLED)
        self.pkt_tree.bind("<<TreeviewSelect>>", self._on_pkt_select)
        self.pkt_tree.bind("<Double-1>", self._on_pkt_double)

        # --- Tab 2: Beacon Dashboard ---
        bcn_frame = ttk.Frame(nb, padding=8)
        nb.add(bcn_frame, text="Beacons")

        bcn_cols = ("time", "src", "rssi", "prssi", "snr", "tx_pwr", "sf", "bus_v", "i_ma", "p_mw", "charge", "entries")
        self.bcn_tree = ttk.Treeview(bcn_frame, columns=bcn_cols, show="headings", height=14)
        for c, w in zip(bcn_cols, (70, 50, 50, 50, 40, 50, 30, 70, 70, 70, 70, 50)):
            self.bcn_tree.heading(c, text=c.upper().replace("_", " "))
            self.bcn_tree.column(c, width=w, minwidth=30)
        self.bcn_tree.pack(fill=tk.BOTH, expand=True, side=tk.LEFT)
        self.bcn_tree.tag_configure("odd", background=self.colors["row_alt"])
        sb2 = ttk.Scrollbar(bcn_frame, orient=tk.VERTICAL, command=self.bcn_tree.yview)
        sb2.pack(fill=tk.Y, side=tk.RIGHT)
        self.bcn_tree.configure(yscrollcommand=sb2.set)

        # --- Tab 3: Remote Configuration ---
        cfg_frame = ttk.Frame(nb, padding=8)
        nb.add(cfg_frame, text="Remote Config")

        cfg_top = ttk.Frame(cfg_frame, style="Panel.TFrame", padding=(12, 10))
        cfg_top.pack(fill=tk.X, pady=(0, 10))

        ttk.Label(cfg_top, text="Remote node settings", style="Metric.TLabel").grid(row=0, column=0, columnspan=6, sticky=tk.W, pady=(0, 8))
        ttk.Label(cfg_top, text="TX power", style="Panel.TLabel").grid(row=1, column=0, sticky=tk.W)
        self.remote_pwr_var = tk.IntVar(value=14)
        self.remote_pwr_spin = ttk.Spinbox(cfg_top, from_=1, to=22, textvariable=self.remote_pwr_var, width=6)
        self.remote_pwr_spin.grid(row=1, column=1, sticky=tk.W, padx=(8, 18))

        ttk.Label(cfg_top, text="SF", style="Panel.TLabel").grid(row=1, column=2, sticky=tk.W)
        self.remote_sf_var = tk.IntVar(value=7)
        self.remote_sf_combo = ttk.Combobox(
            cfg_top,
            textvariable=self.remote_sf_var,
            values=[5, 6, 7, 8, 9, 10, 11, 12],
            width=6,
            state="readonly",
        )
        self.remote_sf_combo.grid(row=1, column=3, sticky=tk.W, padx=(8, 18))

        self.sync_base_sf_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(cfg_top, text="Sync base SF after ACK wait", variable=self.sync_base_sf_var).grid(row=1, column=4, sticky=tk.W)

        btn_row = ttk.Frame(cfg_top, style="Panel.TFrame")
        btn_row.grid(row=2, column=0, columnspan=6, sticky=tk.W, pady=(12, 0))
        ttk.Button(btn_row, text="Set remote power", command=self._send_remote_power).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(btn_row, text="Set remote SF", command=self._send_remote_sf).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(btn_row, text="Set both", style="Accent.TButton", command=self._send_remote_both).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(btn_row, text="Sync base SF now", command=self._sync_base_sf_now).pack(side=tk.LEFT)

        self.remote_status = ttk.Label(cfg_frame, text="No remote config sent", style="Muted.TLabel")
        self.remote_status.pack(anchor=tk.W, pady=(0, 8))

        ack_cols = ("time", "from", "operation", "status", "tx_pwr", "sf", "command")
        self.ack_tree = ttk.Treeview(cfg_frame, columns=ack_cols, show="headings", height=8)
        for c, w in zip(ack_cols, (80, 60, 150, 90, 70, 50, 320)):
            self.ack_tree.heading(c, text=c.upper().replace("_", " "))
            self.ack_tree.column(c, width=w, minwidth=40, stretch=(c == "command"))
        self.ack_tree.pack(fill=tk.BOTH, expand=True, side=tk.LEFT)
        self.ack_tree.tag_configure("ok", foreground=self.colors["good"])
        self.ack_tree.tag_configure("fail", foreground=self.colors["bad"])
        self.ack_tree.tag_configure("odd", background=self.colors["row_alt"])
        ack_sb = ttk.Scrollbar(cfg_frame, orient=tk.VERTICAL, command=self.ack_tree.yview)
        ack_sb.pack(fill=tk.Y, side=tk.RIGHT)
        self.ack_tree.configure(yscrollcommand=ack_sb.set)

        # --- Tab 4: GPS Map (node positions) ---
        gps_frame = ttk.Frame(nb, padding=8)
        nb.add(gps_frame, text="GPS")

        gps_cols = ("time", "src", "fix", "lat", "lon", "last_fix", "maps_link")
        self.gps_tree = ttk.Treeview(gps_frame, columns=gps_cols, show="headings", height=14)
        for c, w in zip(gps_cols, (70, 50, 70, 120, 120, 90, 350)):
            self.gps_tree.heading(c, text=c.upper().replace("_", " "))
            self.gps_tree.column(c, width=w, minwidth=30)
        self.gps_tree.pack(fill=tk.BOTH, expand=True, side=tk.LEFT)
        self.gps_tree.tag_configure("odd", background=self.colors["row_alt"])
        sb3 = ttk.Scrollbar(gps_frame, orient=tk.VERTICAL, command=self.gps_tree.yview)
        sb3.pack(fill=tk.Y, side=tk.RIGHT)
        self.gps_tree.configure(yscrollcommand=sb3.set)
        self.gps_tree.tag_configure("fix", foreground="green")
        self.gps_tree.tag_configure("nofix", foreground="gray")

        # --- Tab 5: Map View ---
        map_frame = ttk.Frame(nb, padding=8)
        nb.add(map_frame, text="Map")

        map_toolbar = ttk.Frame(map_frame)
        map_toolbar.pack(fill=tk.X, pady=(0, 8))
        self.map_status = ttk.Label(map_toolbar, text="Waiting for GPS fix", style="Muted.TLabel")
        self.map_status.pack(side=tk.LEFT)
        ttk.Button(map_toolbar, text="Center latest", command=self._center_latest_map_node).pack(side=tk.RIGHT)
        ttk.Button(map_toolbar, text="Open in Google Maps", command=self._open_latest_map_url).pack(side=tk.RIGHT, padx=(0, 6))

        if tkintermapview:
            self.map_widget = tkintermapview.TkinterMapView(map_frame, corner_radius=0)
            self.map_widget.pack(fill=tk.BOTH, expand=True)
            try:
                self.map_widget.set_tile_server(
                    "https://mt0.google.com/vt/lyrs=m&hl=en&x={x}&y={y}&z={z}&s=Ga",
                    max_zoom=22,
                )
            except Exception:
                pass
            self.map_widget.set_position(0, 0)
            self.map_widget.set_zoom(2)
        else:
            fallback = ttk.Frame(map_frame, style="Panel.TFrame", padding=18)
            fallback.pack(fill=tk.BOTH, expand=True)
            ttk.Label(fallback, text="Map widget not installed", style="Metric.TLabel").pack(anchor=tk.W)
            ttk.Label(
                fallback,
                text="Install it on Windows with: py -m pip install tkintermapview",
                style="Panel.TLabel",
            ).pack(anchor=tk.W, pady=(8, 0))
            ttk.Label(
                fallback,
                text="Linux/macOS: python3 -m pip install tkintermapview",
                style="Panel.TLabel",
            ).pack(anchor=tk.W, pady=(4, 0))
            ttk.Label(
                fallback,
                text="The GPS table and Google Maps button will still work when nodes report a fix.",
                style="Panel.TLabel",
            ).pack(anchor=tk.W, pady=(4, 0))

        # --- Tab 6: Raw Log ---
        log_frame = ttk.Frame(nb)
        nb.add(log_frame, text="Log")

        log_toolbar = ttk.Frame(log_frame, padding=(8, 8, 8, 0))
        log_toolbar.pack(fill=tk.X)
        ttk.Label(log_toolbar, text="Raw serial log", style="Metric.TLabel").pack(side=tk.LEFT)
        ttk.Button(log_toolbar, text="Clear log", command=self._clear_log).pack(side=tk.RIGHT)

        self.log_text = scrolledtext.ScrolledText(log_frame, wrap=tk.WORD, font=("Consolas", 10), relief=tk.FLAT, borderwidth=8)
        self.log_text.pack(fill=tk.BOTH, expand=True)

        # Bottom: command entry
        bot = ttk.Frame(main, style="Panel.TFrame", padding=(10, 8))
        bot.pack(fill=tk.X, pady=(10, 0))

        ttk.Label(bot, text="Command", style="Panel.TLabel").pack(side=tk.LEFT)
        self.cmd_var = tk.StringVar()
        self.cmd_entry = ttk.Entry(bot, textvariable=self.cmd_var, width=50)
        self.cmd_entry.pack(side=tk.LEFT, padx=(8, 4), fill=tk.X, expand=True)
        self.cmd_entry.bind("<Return>", lambda e: self._send_command())
        self.send_btn = ttk.Button(bot, text="Send", command=self._send_command)
        self.send_btn.pack(side=tk.LEFT, padx=2)

        # Quick buttons
        quick = ttk.Frame(bot, style="Panel.TFrame")
        quick.pack(side=tk.LEFT, padx=(10, 0))
        for label, cmd in [("Get Battery", "get battery"), ("Get Solar", "get solar"),
                           ("Get Charge", "get charge"), ("Get Routing", "get routing"),
                           ("Ping", "ping")]:
            ttk.Button(quick, text=label, command=lambda c=cmd: self._send_raw(c)).pack(side=tk.LEFT, padx=2)

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
        elif not self.running:
            self.port_var.set("No serial ports found")

    def _get_selected_port(self):
        val = self.port_var.get()
        if val.startswith("No serial ports"):
            return None
        return val.split(" ")[0] if val else None

    def _toggle_connect(self):
        if self.running:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self._get_selected_port()
        if not port:
            messagebox.showwarning(
                "No port selected",
                "No serial port is selected.\n\n"
                "Plug in the LoRa board, click Refresh, then pick the port "
                "from the drop-down before clicking Connect.",
            )
            return
        try:
            self.serial_port = serial.Serial(port, BAUD_RATE, timeout=0.1)
            self.serial_port.setDTR(True)
            self.serial_port.setRTS(True)
        except serial.SerialException as e:
            self._log(f"Error opening {port}: {e}\n")
            hint = ""
            msg = str(e).lower()
            if "permission" in msg or "access" in msg or "denied" in msg:
                hint = (
                    "\n\nPermission denied. On Linux add your user to the "
                    "'dialout' group:\n"
                    "    sudo usermod -aG dialout $USER\n"
                    "then log out and back in."
                )
            elif "busy" in msg or "in use" in msg:
                hint = "\n\nThe port is already open in another program (close minicom/screen/etc.)."
            elif "no such" in msg or "could not open" in msg:
                hint = "\n\nThe port no longer exists. Click Refresh and try again."
            messagebox.showerror("Connect failed", f"Could not open {port}:\n{e}{hint}")
            return
        except Exception as e:  # noqa: BLE001
            self._log(f"Error opening {port}: {e}\n")
            messagebox.showerror("Connect failed", f"Could not open {port}:\n{e}")
            return
        self.running = True
        self.connect_btn.config(text="Disconnect")
        self.status_lbl.config(text=f"Connected: {port}", style="Good.TLabel")
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
        self.status_lbl.config(text="Disconnected", style="Bad.TLabel")
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

                    ack = CMD_ACK_PATTERN.match(line)
                    if ack:
                        if pending_rx:
                            self.root.after(0, self._add_rx_packet, pending_rx, None)
                            pending_rx = None
                        self.root.after(0, self._add_cfg_ack, ack.groups())
                        continue

                    # Check for [BEACON] line (follows a [RX] with type=0)
                    bcn_v5 = BEACON_PATTERN_V5.match(line)
                    bcn_v4 = BEACON_PATTERN_V4.match(line) if not bcn_v5 else None
                    bcn_v3 = BEACON_PATTERN_V3.match(line) if not (bcn_v5 or bcn_v4) else None
                    bcn_v2 = BEACON_PATTERN_V2.match(line) if not (bcn_v5 or bcn_v4 or bcn_v3) else None
                    bcn = BEACON_PATTERN.match(line) if not (bcn_v5 or bcn_v4 or bcn_v3 or bcn_v2) else None
                    bcn_leg = BEACON_PATTERN_LEGACY.match(line) if not (bcn_v5 or bcn_v4 or bcn_v3 or bcn_v2 or bcn) else None
                    bcn_min = BEACON_PATTERN_MINIMAL.match(line) if not (bcn_v5 or bcn_v4 or bcn_v3 or bcn_v2 or bcn or bcn_leg) else None
                    if bcn_v5 or bcn_v4 or bcn_v3 or bcn_v2 or bcn or bcn_leg or bcn_min:
                        match = bcn_v5 or bcn_v4 or bcn_v3 or bcn_v2 or bcn or bcn_leg or bcn_min
                        version = "v5" if bcn_v5 else ("v4" if bcn_v4 else ("v3" if bcn_v3 else None))
                        beacon_data = self._parse_beacon(match, version=version)
                        if pending_rx:
                            self.root.after(0, self._add_rx_packet, pending_rx, beacon_data)
                            pending_rx = None
                        continue

                    # Flush any pending RX that wasn't followed by [BEACON]
                    if pending_rx:
                        self.root.after(0, self._add_rx_packet, pending_rx, None)
                        pending_rx = None

                    # Check for [RX] line — v2 (with snr/sf/freq) first
                    m = RX_PATTERN_V2.match(line)
                    if m:
                        pending_rx = {
                            "src": m.group(1), "dst": m.group(2),
                            "rssi": m.group(3), "prssi": m.group(4),
                            "snr": m.group(5), "sf": m.group(6), "freq": m.group(7),
                            "type": m.group(8), "seq": m.group(9),
                            "len": m.group(10),
                        }
                        continue

                    m = RX_PATTERN.match(line)
                    if m:
                        pending_rx = {
                            "src": m.group(1), "dst": m.group(2),
                            "rssi": m.group(3), "prssi": m.group(4),
                            "snr": "—", "sf": "—", "freq": "—",
                            "type": m.group(5), "seq": m.group(6),
                            "len": m.group(7),
                        }
                        continue

                    m = RX_PATTERN_LEGACY.match(line)
                    if m:
                        pending_rx = {
                            "src": m.group(1), "dst": m.group(2),
                            "rssi": m.group(3), "prssi": "—",
                            "snr": "—", "sf": "—", "freq": "—",
                            "type": m.group(4), "seq": "—",
                            "len": m.group(5),
                        }
                        continue

            except (serial.SerialException, OSError):
                self.root.after(0, self._disconnect)
                break
            except Exception:
                pass

    def _parse_beacon(self, m, version=None):
        groups = m.groups()
        if version == "v5" and len(groups) == 10:
            # v5: i_ma, bus, bat, chg, tx_pwr, sf, lat_udeg, lon_udeg, fix, entries
            return {"i_ma": groups[0], "bus": groups[1], "bat": groups[2],
                    "chg": groups[3], "tx_pwr": groups[4], "sf": groups[5],
                    "lat_udeg": int(groups[6]), "lon_udeg": int(groups[7]),
                    "fix": int(groups[8]), "entries": groups[9]}
        if version == "v4" and len(groups) == 7:
            # v4: i_ma, bus, bat, chg, tx_pwr, sf, entries
            return {"i_ma": groups[0], "bus": groups[1], "bat": groups[2],
                    "chg": groups[3], "tx_pwr": groups[4], "sf": groups[5],
                    "entries": groups[6]}
        if version == "v3" and len(groups) == 5:
            # V3: i_ma, bus, bat, chg, entries (current already in mA on the wire)
            return {"i_ma": groups[0], "bus": groups[1], "bat": groups[2],
                    "chg": groups[3], "tx_pwr": "—", "sf": "—",
                    "entries": groups[4]}
        if len(groups) == 5:
            # V2: shunt(mV), bus, bat, chg, entries  — derive i_ma assuming 50mΩ shunt
            return {"i_ma": str(int(groups[0]) * 20), "bus": groups[1], "bat": groups[2],
                    "chg": groups[3], "tx_pwr": "—", "sf": "—",
                    "entries": groups[4]}
        elif len(groups) == 4:
            return {"i_ma": "—", "bus": groups[1], "bat": groups[0],
                    "chg": groups[2], "tx_pwr": "—", "sf": "—",
                    "entries": groups[3]}
        elif len(groups) == 2:
            return {"i_ma": "—", "bus": "—", "bat": groups[0],
                    "chg": "—", "tx_pwr": "—", "sf": "—",
                    "entries": groups[1]}
        else:
            return {"i_ma": "—", "bus": "—", "bat": "—",
                    "chg": "—", "tx_pwr": "—", "sf": "—",
                    "entries": groups[0]}

    # ---------------------------------------------------- Packet display
    def _add_rx_packet(self, rx, beacon):
        ts = datetime.now().strftime("%H:%M:%S")
        ptype = PKT_TYPES.get(rx["type"], rx["type"])
        self.packet_count += 1

        info = ""
        if beacon:
            self.beacon_count += 1
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
            extra = ""
            if beacon.get("tx_pwr", "—") != "—":
                extra = f"  TxPwr={beacon['tx_pwr']}  SF={beacon['sf']}"
            # GPS suffix for v5 beacons
            gps_suffix = ""
            if "lat_udeg" in beacon:
                lat = beacon["lat_udeg"] / 1_000_000
                lon = beacon["lon_udeg"] / 1_000_000
                if beacon["fix"]:
                    gps_suffix = f"  📍 {lat:.6f},{lon:.6f}"
                else:
                    gps_suffix = "  📍 no fix"
                self._update_gps_tab(ts, rx["src"], beacon)
            info = f"Bus={bus_v}  I={i_ma}  P={p_mw}  Charge={chg_s}  Routes={beacon['entries']}{extra}{gps_suffix}"

            # Add to beacon tree
            beacon_tags = ("odd",) if self.beacon_count % 2 else ()
            self.bcn_tree.insert("", 0, tags=beacon_tags, values=(
                ts, rx["src"], rx["rssi"], rx["prssi"], rx.get("snr", "—"),
                beacon.get("tx_pwr", "—"), beacon.get("sf", "—"),
                bus_v, i_ma, p_mw, chg_s, beacon["entries"]
            ))
            # Trim beacon tree
            children = self.bcn_tree.get_children()
            if len(children) > 200:
                for c in children[200:]:
                    self.bcn_tree.delete(c)

        packet_tags = [ptype]
        if self.packet_count % 2:
            packet_tags.append("odd")
        self.pkt_tree.insert("", 0, tags=tuple(packet_tags), values=(
            ts, rx["src"], rx["dst"], ptype, rx["seq"],
            rx["rssi"], rx["prssi"], rx.get("snr", "—"), rx.get("sf", "—"),
            rx["len"], info
        ))
        self.packet_metric.config(text=f"{self.packet_count} packets")
        self.beacon_metric.config(text=f"{self.beacon_count} beacons")
        self.last_packet_metric.config(text=f"Last RX: {ts}  node {rx['src']}  {ptype}")
        # Trim packet tree
        children = self.pkt_tree.get_children()
        if len(children) > 500:
            for c in children[500:]:
                self.pkt_tree.delete(c)

    def _update_gps_tab(self, ts, src, beacon):
        lat = beacon["lat_udeg"] / 1_000_000
        lon = beacon["lon_udeg"] / 1_000_000
        fix = beacon["fix"]

        # Track last fix time per node
        src_key = str(src)
        if fix:
            self.last_fix_time[src_key] = ts

        last_fix_ts = self.last_fix_time.get(src_key, "never")

        if fix:
            fix_str = "✓ FIX"
            lat_str = f"{lat:.6f}°"
            lon_str = f"{lon:.6f}°"
            maps = f"https://maps.google.com/?q={lat:.6f},{lon:.6f}"
            tag = "fix"
            self._update_map_marker(ts, src_key, lat, lon)
        else:
            fix_str = "✗ NO FIX"
            # Show last known coords with stale marker when available
            if lat != 0.0 or lon != 0.0:
                lat_str = f"[stale] {lat:.6f}°"
                lon_str = f"[stale] {lon:.6f}°"
                maps = f"(stale) https://maps.google.com/?q={lat:.6f},{lon:.6f}"
            else:
                lat_str = "— no data —"
                lon_str = "— no data —"
                maps = "—"
            tag = "nofix"

        # Remove previous row for this src (keep only latest per node)
        for iid in self.gps_tree.get_children():
            if self.gps_tree.set(iid, "src") == src_key:
                self.gps_tree.delete(iid)
                break

        gps_tags = (tag, "odd") if len(self.gps_tree.get_children()) % 2 else (tag,)
        self.gps_tree.insert("", 0, tags=gps_tags, values=(
            ts, src, fix_str,
            lat_str, lon_str,
            last_fix_ts,
            maps,
        ))

    def _update_map_marker(self, ts, src, lat, lon):
        self.latest_map_url = f"https://maps.google.com/?q={lat:.6f},{lon:.6f}"
        self.gps_nodes[src] = {"time": ts, "lat": lat, "lon": lon, "url": self.latest_map_url}
        self.map_status.config(text=f"Latest fix: node {src} at {lat:.6f}, {lon:.6f} ({ts})")

        if not self.map_widget:
            return

        label = f"Node {src}\n{ts}"
        marker = self.map_markers.get(src)
        if marker:
            try:
                marker.set_position(lat, lon)
                marker.set_text(label)
            except Exception:
                marker.delete()
                marker = None
        if not marker:
            self.map_markers[src] = self.map_widget.set_marker(lat, lon, text=label)

        if len(self.map_markers) == 1:
            self.map_widget.set_position(lat, lon)
            self.map_widget.set_zoom(15)

    def _center_latest_map_node(self):
        if not self.gps_nodes:
            messagebox.showinfo("No GPS fix", "No node has reported a valid GPS fix yet.")
            return
        latest = max(self.gps_nodes.values(), key=lambda item: item["time"])
        if self.map_widget:
            self.map_widget.set_position(latest["lat"], latest["lon"])
            self.map_widget.set_zoom(15)
        self.latest_map_url = latest["url"]
        self.map_status.config(text=f"Centered latest fix: {latest['lat']:.6f}, {latest['lon']:.6f}")

    def _open_latest_map_url(self):
        if not self.latest_map_url:
            messagebox.showinfo("No GPS fix", "No node has reported a valid GPS fix yet.")
            return
        webbrowser.open(self.latest_map_url)

    # ---------------------------------------------------------- Commands
    def _validated_remote_values(self, need_pwr=False, need_sf=False):
        try:
            pwr = int(self.remote_pwr_var.get())
            sf = int(self.remote_sf_var.get())
        except (tk.TclError, ValueError):
            messagebox.showerror("Invalid config", "TX power and SF must be numeric.")
            return None

        if need_pwr and not 1 <= pwr <= 22:
            messagebox.showerror("Invalid TX power", "TX power must be in the range 1..22.")
            return None
        if need_sf and not 5 <= sf <= 12:
            messagebox.showerror("Invalid SF", "Spreading factor must be in the range 5..12.")
            return None
        return pwr, sf

    def _send_remote_power(self):
        values = self._validated_remote_values(need_pwr=True)
        if not values:
            return
        pwr, _sf = values
        self._send_remote_cfg(f"send cfg pwr {pwr}", changes_sf=False)

    def _send_remote_sf(self):
        values = self._validated_remote_values(need_sf=True)
        if not values:
            return
        _pwr, sf = values
        self._send_remote_cfg(f"send cfg sf {sf}", changes_sf=True, new_sf=sf)

    def _send_remote_both(self):
        values = self._validated_remote_values(need_pwr=True, need_sf=True)
        if not values:
            return
        pwr, sf = values
        self._send_remote_cfg(f"send cfg both {pwr} {sf}", changes_sf=True, new_sf=sf)

    def _send_remote_cfg(self, command, changes_sf=False, new_sf=None):
        if self._send_raw(command):
            self.last_remote_command = command
            self.remote_status.config(text=f"Sent: {command}  |  awaiting CMD_ACK")
            if changes_sf and self.sync_base_sf_var.get() and new_sf is not None:
                self.root.after(1500, lambda sf=new_sf: self._sync_base_sf_now(sf))

    def _sync_base_sf_now(self, sf=None):
        if sf is None:
            values = self._validated_remote_values(need_sf=True)
            if not values:
                return
            _pwr, sf = values
        if self._send_raw(f"set data_rate {sf}"):
            self.remote_status.config(text=f"Base radio SF sync sent: set data_rate {sf}")

    def _add_cfg_ack(self, groups):
        src, op, status, tx_pwr, sf = groups
        ts = datetime.now().strftime("%H:%M:%S")
        self.remote_ack_count += 1
        ok = status == "0"
        status_text = "OK" if ok else f"ERR {status}"
        op_text = CFG_OPS.get(op, f"op {op}")
        tags = ["ok" if ok else "fail"]
        if self.remote_ack_count % 2:
            tags.append("odd")
        self.ack_tree.insert("", 0, tags=tuple(tags), values=(
            ts, src, op_text, status_text, tx_pwr, sf, self.last_remote_command
        ))
        self.remote_status.config(
            text=f"CMD_ACK from node {src}: {status_text}, TX power {tx_pwr}, SF {sf}"
        )
        children = self.ack_tree.get_children()
        if len(children) > 100:
            for item in children[100:]:
                self.ack_tree.delete(item)

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
                return True
            except serial.SerialException as e:
                self._log(f"Send error: {e}\n")
                messagebox.showerror("Send failed", f"Could not send command:\n{e}")
        else:
            self._log(f"Not connected: {cmd}\n")
            messagebox.showinfo("Not connected", "Connect to a serial port before sending commands.")
        return False

    # --------------------------------------------------------------- Log
    def _clear_packets(self):
        for tree in (self.pkt_tree, self.bcn_tree, self.gps_tree):
            for item in tree.get_children():
                tree.delete(item)
        self.packet_count = 0
        self.beacon_count = 0
        self.last_fix_time.clear()
        self.gps_nodes.clear()
        self.latest_map_url = None
        for marker in self.map_markers.values():
            marker.delete()
        self.map_markers.clear()
        self.map_status.config(text="Waiting for GPS fix")
        self.packet_metric.config(text="0 packets")
        self.beacon_metric.config(text="0 beacons")
        self.last_packet_metric.config(text="Last RX: --")
        self.pkt_detail.configure(state=tk.NORMAL)
        self.pkt_detail.delete("1.0", tk.END)
        self.pkt_detail.configure(state=tk.DISABLED)

    def _copy_selected_packet_info(self):
        sel = self.pkt_tree.selection()
        if not sel:
            return
        vals = self.pkt_tree.item(sel[0], "values")
        if not vals:
            return
        text = self._format_packet_values(vals)
        self.root.clipboard_clear()
        self.root.clipboard_append(text)

    def _format_packet_values(self, vals):
        cols = ("TIME", "SRC", "DST", "TYPE", "SEQ", "RSSI", "PRSSI",
                "SNR", "SF", "LEN", "INFO")
        parts = []
        for name, val in zip(cols, vals):
            if val == "" or val is None:
                continue
            parts.append(f"{name}: {val}")
        if not parts:
            return ""
        return "  |  ".join(parts[:-1]) + "\n" + parts[-1]

    def _on_pkt_select(self, _event=None):
        sel = self.pkt_tree.selection()
        if not sel:
            return
        vals = self.pkt_tree.item(sel[0], "values")
        if not vals:
            return
        text = self._format_packet_values(vals)
        self.pkt_detail.configure(state=tk.NORMAL)
        self.pkt_detail.delete("1.0", tk.END)
        self.pkt_detail.insert("1.0", text)
        self.pkt_detail.configure(state=tk.DISABLED)

    def _on_pkt_double(self, _event=None):
        sel = self.pkt_tree.selection()
        if not sel:
            return
        vals = self.pkt_tree.item(sel[0], "values")
        text = self._format_packet_values(vals) if vals else ""
        if not text:
            return
        win = tk.Toplevel(self.root)
        win.title("Packet details")
        win.geometry("700x300")
        txt = scrolledtext.ScrolledText(win, wrap=tk.WORD, font=("Consolas", 10))
        txt.pack(fill=tk.BOTH, expand=True)
        txt.insert("1.0", text)

    def _clear_log(self):
        self.log_text.delete("1.0", tk.END)

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
