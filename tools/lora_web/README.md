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

## Enclosure tab (gateway Pi only)

On the outdoor gateway Pi an SHT3x sits inside the case on I2C-1
(pin 1 3V3, pin 3 SDA, pin 5 SCL, pin 9 GND; address 0x44). `local_env.py`
samples it every 60 s together with the Pi CPU temperature and stores the
readings in the `env_readings` table of `beacons.db`. The **Enclosure** tab
shows case air / CPU / dew point, peaks, condensation margin, throttle state
and alerts (thresholds in `const ENC` in `index.html`), plus CSV export at
`/api/env.csv?hours=N` (`hours=0` = full history).

```bash
sudo apt install python3-smbus2
python3 server.py --env-interval 60      # default; --env-i2c-bus -1 disables it
```

The Pi runs I2C at 10 kHz (`dtparam=i2c_arm_baudrate=10000` in
`/boot/firmware/config.txt`). The SHT3x resets on every measurement if its
VCC/GND contact is poor (status register bit 4 set again after a measure).
