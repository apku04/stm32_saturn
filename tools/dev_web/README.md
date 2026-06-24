# Saturn Dev Dashboard

Local development control panel for **pi5-jenkins** — the dev host.
Distinct from `tools/lora_web/` (production map dashboard, runs on the gateway pi-zero).

## Run

```bash
./tools/dev_web/run.sh                  # default port 8089
./tools/dev_web/run.sh --port 9000      # alternative port
```

Open `http://<pi5-jenkins>:8089/` in a browser.

To keep it running across logout:

```bash
nohup ./tools/dev_web/run.sh > /tmp/dev_web.log 2>&1 &
```

## What it shows

* **Connected Devices** — every USB CDC port (`/dev/ttyACM*`, `/dev/ttyUSB*`) and any
  STM32 in DFU mode (`0483:df11`). STM32 CDC devices (`0483:5740`) are highlighted.
  Click a device to select it.
* **Device Info** — VID/PID, USB bus path, serial, manufacturer, etc.
  Hit **Probe** to inject `version`, `get mac_address`, `get uid`, `get bor`, `get flash`
  on the selected port — responses appear in the console.
* **Firmware** — auto-discovered `.bin` files under `firmware/app/*/` and
  `firmware/app/*/build/`. Click to select.
* **Flash** —
  * **USB DFU**: runs `firmware/tools/dfu_flash.sh <bin> --enter` (sends `dfu`
    over CDC of selected device, waits for `0483:df11`, flashes via `dfu-util`).
  * **SWD**: runs `sudo bash firmware/tools/flash.sh <bin>` (openocd via the
    Pi GPIO bit-bang adapter — wiring docs in `firmware/tools/openocd.cfg`).
  * Output streams live into the **Output** pane.
  * For SWD, the host needs passwordless `sudo` for `openocd` (or run the
    server as root). See "Permissions" below.
* **Console** — live serial console with command history (↑/↓), CRLF on send,
  ring buffer of the last ~5000 lines replayed to new clients. Auto-recovers
  after flash by re-opening the port.
* **OTA Update** — placeholder for future LoRa OTA work. Buttons are stubbed.

## Permissions

* **CDC** (`/dev/ttyACM*`): the user must be in the `dialout` group
  (`sudo usermod -aG dialout pi`, re-login).
* **DFU**: handled by the existing `firmware/tools/dfu_flash.sh` (uses
  `sudo dfu-util` internally — needs sudoers entry or interactive password).
* **SWD**: the openocd flash script uses `sudo` to access GPIO. For
  hands-off web flashing, allow it without a password:

  ```
  # /etc/sudoers.d/dev_web
  pi ALL=(ALL) NOPASSWD: /usr/bin/openocd, /usr/bin/dfu-util
  ```

  (Adjust paths via `which openocd dfu-util`.)

## Architecture (very short)

* `server.py` — stdlib HTTP + pyserial. Endpoints:
  * `GET  /api/devices`
  * `GET  /api/firmwares`
  * `GET  /api/console/stream?device=<dev>`   (SSE, live console + replay)
  * `POST /api/console/{open,close,send}`
  * `POST /api/probe`
  * `POST /api/flash`                          (returns `{job: <id>}`)
  * `GET  /api/job/stream?id=<id>`             (SSE, flash log)
  * `POST /api/ota`                            (501, reserved)
* `index.html` — single-page UI (no build step, no external CDN beyond
  what your browser already caches; everything inline).
* No DB, no persistence — pure live tool.

## Topology reminder

| Host          | Role                | Web                                     |
| ------------- | ------------------- | --------------------------------------- |
| pi-zero (sentry, 192.168.1.38) | **Gateway** + STM32 MAC 3 | `tools/lora_web/` on `:8088` (production) |
| pi5-jenkins (this) | Development        | `tools/dev_web/` on `:8089` (this tool)   |
