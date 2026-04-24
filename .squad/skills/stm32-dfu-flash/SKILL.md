---
name: "stm32-dfu-flash"
description: "Flash STM32U073 firmware over USB DFU (SWD is disconnected on the Saturn board)"
domain: "firmware-flash"
confidence: "high"
source: "earned"
---

## Context
The Saturn LoRa Tracker (STM32U073CBT6) has **no working SWD** — the debug header is disconnected. The only supported flash path is **USB DFU** via the ST system bootloader (VID 0483, PID df11). Use this skill whenever the task involves programming firmware onto this board.

## Patterns

### Entering DFU mode
The board has no BOOT0 button and no double-tap reset. Enter DFU from the running app:

1. Running firmware exposes a USB CDC terminal (`/dev/ttyACMn`, 115200 8N1 raw).
2. Send the `dfu` command — the app jumps to the system bootloader at 0x1FFF0000.
3. `lsusb` should show `0483:df11` within ~2 s.

If the app is hung / not responding to `dfu`, the only recovery is a hardware-assisted BOOT0 pull-high before reset (rare — not scripted here).

### Flash command (direct)
```bash
sudo dfu-util -a 0 -s 0x08000000:leave -D firmware/app/lora/build/lora.bin
```
- `-a 0` = alt-setting 0 (internal flash)
- `-s 0x08000000` = flash base
- `:leave` = exit bootloader and run app after download

### Helper scripts
- `firmware/tools/dfu_flash.sh <bin> --enter` — sends `dfu` over CDC, waits for `0483:df11`, then flashes. **Preferred** for scripted flows.
- `firmware/tools/dfu_flash.sh <bin>` — assumes MCU is already in DFU mode.
- `firmware/tools/flash.sh` — SWD/OpenOCD path. **Do not use** on this board; SWD is disconnected.

### The benign `get_status` error
After `:leave`, dfu-util prints:
```
Submitting leave request...
Error during download get_status
```
This is **expected and safe**. `:leave` tells the bootloader to jump to the app immediately; the app is running before dfu-util can poll final status, so the status read fails. If the firmware is otherwise working (CDC re-enumerates, LED blinks, etc.) the flash succeeded.

### Linux serial hygiene before/after flash
```bash
stty -F /dev/ttyACM0 115200 raw -echo
```
Cooked-mode echo causes a feedback loop that floods the terminal with `unknown cmd:` errors.

## Examples
- Full bring-up: `cd firmware/app/lora && make && ../../tools/dfu_flash.sh build/lora.bin --enter`
- Manual path when CDC already dead: put in DFU via reset-with-BOOT0, then `sudo dfu-util -a 0 -s 0x08000000:leave -D build/lora.bin`

## Anti-Patterns
- ❌ Using `flash.sh` / OpenOCD / `st-link` — SWD is physically disconnected; these will hang or error.
- ❌ Treating the `Error during download get_status` line as a flash failure — don't re-flash in a loop; verify on-target behavior instead.
- ❌ Reading/writing `/dev/ttyACMn` without `stty ... raw -echo` first.
- ❌ Forgetting `sudo` on systems without a dfu udev rule — dfu-util will claim-interface fail.
