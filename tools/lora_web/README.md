# Saturn LoRa Web Dashboard

Browser dashboard for beacons received by the LoRa receiver attached to this
PC. Logs everything to a SQLite database and serves zoomable per-MAC graphs of
temperature, humidity, RSSI, SNR, battery, INA219 current/power and TX
parameters.

## Quick start

```bash
pip install pyserial
python3 server.py
# → [http] serving http://localhost:8080/
```

Open http://localhost:8080/ in a browser.

The serial port is auto-detected (looks for STM32 VID `0483` / PIC24 VID
`04D8`, then any `ttyACM*`/`ttyUSB*`). Override with `--port`:

```bash
python3 server.py --port /dev/ttyACM0
python3 server.py --port /dev/ttyACM1 --http-port 8081
python3 server.py --no-serial          # just browse the existing DB
```

The database lives next to `server.py` as `beacons.db` (override with `--db`).
It is append-only WAL SQLite — safe to copy/back-up while the server runs.

## What it shows

For each source MAC address (one chip per node):

- **Summary**: latest temp, humidity, RSSI, SNR, bus voltage, battery,
  current, charge state, TX power, SF.
- **Graphs** (drag to zoom, double-click to reset, mode-bar pan/zoom):
  temperature, humidity, RSSI/pRSSI/SNR, bus + battery voltage, INA219
  current + power, TX power + SF.
- **Time range**: last hour … last year (selectable).
- **Auto-refresh**: off / 10 s / 30 s / 60 s.

## Compatibility

Parses BEACON formats v3, v4, v5, v6 (per
[firmware/app/lora/main.c](../../firmware/app/lora/main.c)). Older formats are
ignored — they don't carry temp/humidity/INA219 anyway.

## Network access

By default binds to `0.0.0.0` so other devices on the LAN can reach it. Bind
to localhost only with `--bind 127.0.0.1`.
