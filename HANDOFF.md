# Handoff Guide — Saturn LoRa Tracker

> Read this first. You are an LLM session continuing this project on a fresh
> Raspberry Pi. Your predecessor is on another RPi; you inherit the repo,
> the persistent memory, and the Squad team state. Get up to speed before
> touching anything.

---

## 1. What this project is

- **Saturn LoRa Tracker** — bare-metal C firmware for **STM32U073CBT6**
  (Cortex-M0+, 64 KB flash, 40 KB RAM, LQFP-48) driving an **SX1262**
  LoRa radio (E22-900M22S module, 868 MHz, SPI1).
- No STM HAL. Custom register defs in [firmware/include/stm32u0.h](firmware/include/stm32u0.h).
- Linker script: [firmware/linker/stm32u073cb.ld](firmware/linker/stm32u073cb.ld).
- PCB is EasyEDA (`stm32_lora.eprj`, SQLite3). Schematic PDF + webp at repo root.
- GitHub repo: `apku04/stm32_saturn`.
- Two physical boards on the bench. Both report USB serial `0001`; distinguish
  by USB bus path. See memory file for MAC↔UID mapping.

---

## 2. Where the persistent knowledge lives

You inherit three layers of memory. **Read all of them before doing real work.**

### 2.1 Copilot memory (`/memories/`)

Cross-session notes maintained by the Copilot memory tool. On the new RPi this
lives under `~/.vscode-server/.../memories/` (path is managed by the agent —
just use the `memory` tool).

**Critical file to migrate:**

- `/memories/repo/stm32-lora-build.md` — flash gotchas, SX1262 STDBY rule,
  UID_BASE address, MAC derivation algorithm, packet struct layout, two-board
  flashing procedure. **This is the single most important file.** Copy it
  verbatim to the new machine's memory.

To migrate: on the old RPi, `view` the file with the memory tool, then on
the new RPi `create` it at the same path with the same content.

### 2.2 Squad team state (`.squad/`)

Lives in the repo, so it travels with `git clone` / `rsync`. Contents:

- [.squad/team.md](.squad/team.md) — roster (Warhammer 40k cast: Guilliman,
  Ferrus, Corax, Dorn, Khan, Lion, Russ, Perturabo, Scribe, Ralph).
- [.squad/decisions.md](.squad/decisions.md) — canonical decision ledger.
- [.squad/agents/*/history.md](.squad/agents/) — each agent's learnings.
  **Read the history file of any agent before spawning them.**
- [.squad/skills/](.squad/skills/) — earned reusable patterns
  (sx1262-driver, stm32-dfu-flash, ina219-driver-patterns,
  packet-buffer, terminal-commands, etc.). **Check here before reinventing.**
- [.squad/routing.md](.squad/routing.md), [.squad/ceremonies.md](.squad/ceremonies.md),
  [.squad/casting/](.squad/casting/) — coordinator config.
- [.squad/orchestration-log/](.squad/orchestration-log/) and
  [.squad/log/](.squad/log/) — append-only history of every prior session.

`.gitattributes` already declares `merge=union` for the append-only Squad
files so concurrent work on two RPis won't conflict on merge.

### 2.3 Repo source of truth

- [firmware/](firmware/) — all firmware (apps under `app/`, drivers under
  `src/driver/`, protocol under `src/protocol/`, system under `src/system/`).
- [tools/lora_monitor.py](tools/lora_monitor.py) — host-side serial monitor.
- [docs/hardware/schematic.txt](docs/hardware/schematic.txt) + PDF/webp at root.

---

## 3. Bringing up the new RPi

### 3.1 Sync the repo

```bash
# Option A: clone fresh from GitHub
git clone git@github.com:apku04/stm32_saturn.git ~/work/stm32

# Option B: rsync from old RPi (preserves uncommitted work)
rsync -avh --exclude='firmware/app/*/build' \
    pi@<old-rpi-ip>:/home/pi/work/stm32/ ~/work/stm32/
```

The repo path **must** be `~/work/stm32` to match the existing memory and
scripts (or update both).

### 3.2 Install host tools

```bash
sudo apt update
sudo apt install -y \
    gcc-arm-none-eabi binutils-arm-none-eabi \
    dfu-util openocd \
    python3-serial python3-pip \
    git make
pip3 install --user pyserial pytest
```

### 3.3 USB permissions

Add yourself to `dialout` so `/dev/ttyACM*` and DFU access work without sudo
on the CDC side (DFU itself still uses `sudo dfu-util`):

```bash
sudo usermod -aG dialout $USER
# log out / back in
```

Optional udev rule for DFU without sudo:

```bash
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", MODE="0666"' \
    | sudo tee /etc/udev/rules.d/50-stm32-dfu.rules
sudo udevadm control --reload-rules
```

### 3.4 (Optional) SWD via RPi GPIO

Only needed for first-time bootloader unbrick or low-level debug. The Pi 5
GPIO bitbang OpenOCD config is at [firmware/tools/openocd.cfg](firmware/tools/openocd.cfg).
Daily flashing uses USB DFU, not SWD.

---

## 4. How to build and flash (USB DFU — daily workflow)

The MCU runs a USB CDC app. To reflash:

1. Build:
   ```bash
   cd ~/work/stm32/firmware/app/lora
   make clean && make        # ALWAYS clean — Makefile doesn't track .h deps
   ```
2. Send the `dfu` command over the CDC port — the app jumps to the system
   bootloader (USB VID:PID becomes `0483:df11`).
3. Flash:
   ```bash
   sudo dfu-util -a 0 -s 0x08000000:leave \
       -D ~/work/stm32/firmware/app/lora/lora.bin
   ```
   `Error during download get_status` after `:leave` is **expected/normal**.

The wrapper script does both steps:

```bash
cd ~/work/stm32/firmware
./tools/dfu_flash.sh app/lora/lora.bin --enter
```

**Two-board gotcha:** if both boards are plugged in, send `dfu` to the
target board's CDC first, wait for a single `0483:df11` to appear, then
flash with `-d 0483:df11`. See `/memories/repo/stm32-lora-build.md`.

**SWD fallback:** `sudo ./tools/flash.sh app/lora/lora.bin` (only if USB
CDC is dead).

---

## 5. Critical gotchas (TL;DR — full detail in repo memory)

1. **`make clean && make` after editing any `.h`** — Makefile has no header
   dep tracking. Stale binaries silently get reflashed.
2. **`UID_BASE = 0x1FFF6E50`** on STM32U073 (NOT the value in many docs).
3. **MAC derivation = FNV-1a 32-bit over all 12 UID bytes, fold to 8 bits.**
   Flat XOR-fold collides on sibling dies.
4. **SX1262 `set_modulation_params` requires STDBY first**, then restart RX.
   Calling it during continuous RX hangs the chip.
5. **pyserial DTR/RTS must be True** to enable CDC TX from MCU.
6. **MAC↔board mapping:** ACM0 (Bus1) = MAC 44, ACM1 (Bus3) = MAC 161.

---

## 6. First-session checklist for the new LLM

Do these in order before accepting any user task:

1. ☐ Read this file fully.
2. ☐ Run `memory view /memories/repo/stm32-lora-build.md`. If missing,
      ask the user to paste it from the old RPi and `memory create` it.
3. ☐ Read [.squad/team.md](.squad/team.md), [.squad/decisions.md](.squad/decisions.md),
      [.squad/routing.md](.squad/routing.md).
4. ☐ Skim [.squad/skills/](.squad/skills/) directory listing — know what
      patterns are already captured.
5. ☐ For any agent you spawn: read their
      `.squad/agents/{name}/history.md` first (the Squad coordinator
      already does this via the spawn template).
6. ☐ Verify toolchain: `arm-none-eabi-gcc --version`, `dfu-util --version`,
      `ls /dev/ttyACM*`.
7. ☐ Verify build: `cd firmware/app/lora && make clean && make`.
8. ☐ Greet the user with a short status: "Squad v0.9.1 loaded, repo at
      ~/work/stm32, N agents on the roster, ready."

---

## 7. Keeping the two RPis in sync

- Squad state is git-tracked and uses `merge=union` — push/pull normally
  and append-only files merge cleanly.
- Copilot `/memories/` is **not** in the repo. When you learn something
  new and durable, write it to `/memories/repo/stm32-lora-build.md` AND
  mention it in the next commit message so the other RPi's session knows
  to pull the memory delta (you'll need to re-create the memory entry on
  the other side — there's no automatic sync).
- Avoid simultaneous flashing of the same physical board from both Pis.
