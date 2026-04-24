---
name: "terminal-commands"
description: "USB-CDC terminal command dispatcher — parse, route, and add new 'get/set' commands for STM32 LoRa firmware."
domain: "firmware-debug"
confidence: "high"
source: "earned"
---

## Context
`firmware/src/system/terminal.c/h` is the operator interface over USB CDC (115200 nominal — USB doesn't care; see `stm32u073-usb-cdc` skill). The USB RX callback in `lora/main.c` (`usb_rx_handler`) hands 64-byte chunks to `terminal(msg, size)`. Pair with the `linux-serial-hygiene` skill on the host side.

## Patterns

**Entry point:** `terminal(uint8_t *msg, uint8_t size)` (terminal.c:44) — copies into a local `MAX_ARGV=128` buffer, NUL-terminates, runs `parseArgs`, dispatches via `menu()`.

**Argument parsing (`parseArgs` terminal.c:60):**
- Single-space separator, treats `\r` / `\n` / `\0` as terminators.
- Writes NULs in-place (destructive tokenization) — mutates the caller's copy, not the original USB buffer.
- `argv[]` is an array of `uint8_t*` (not `char*`) — cast to `(const char *)` when calling `strcmp`/`strtoul`.
- No escape handling, no quoting. `send hello world` sends two separate argv tokens and the `send` command re-joins them with spaces.

**Top-level dispatch (`menu` terminal.c:90):**
```
help | get <param> | set <param> <value> | send <msg…> | ping | version | reset | dfu
```
Each verb is a string compare against `argv[0]`, then a handler call. Unknown verb prints `"Error: unknown cmd: <tok>\n"` — if you see a flood of `unknown cmd: T` on Linux, it's **cooked-mode echo**. Fix with `stty raw -echo` (see `linux-serial-hygiene`).

**Add a new `get xxx` command (canonical example — `get solar`, terminal.c):**
1. Add an `else if (strcmp((const char*)argv[1], "xxx") == 0)` branch in `get_commands()`.
2. Compose the line with `snprintf(buf, sizeof(buf), …)` into the 128-byte local `buf` (don't go near it — overflow corrupts the stack).
3. Single `print(buf)` call. Don't call `print` per-field — each call is a USB chunk + busy-wait.
4. Add the param name to the `help` command's params list.

**`get solar` format** (terminal.c, the production template):
```
Solar:  bus=<u16> mV  shunt=<i32> µV  I=<i32> mA  P=<i32> mW  charge=<Off|Charging|Done|Fault>
```
- `bus_mv = ina219_read_bus_mv()`
- `sh_uv  = ina219_read_shunt_uv()`       ← signed, µV resolution required (see `ina219-driver-patterns`)
- `i_ma   = sh_uv / 50`                   ← 50 mΩ R58 shunt (NOT 100 mΩ)
- `p_mw   = (bus_mv * i_ma) / 1000`
- `charge = charge_status_str(charge_get_status())`

Parallel commands: `get charge` (status only), `get battery` (ADC raw + mV, legacy), `get vrefint` (ADC self-test), `get ina` (register dump), `get adcscan` / `get i2cscan` (bring-up sweeps), `get i2cread <addr> <reg>` (arbitrary I2C probe).

**Set commands (`set_commands` terminal.c:120):** validates range, calls driver `radio_set_*`, **writes through to `flash_data` (RAM only)**. `set flash` is a separate verb that commits `flash_data` to flash via `writeFlash()` — see `flash-config-storage` skill. The two-step design prevents wear from bad values.

**Special commands:**
- `reset` → `SCB_AIRCR = 0x05FA0004` (SYSRESETREQ)
- `dfu` → `jump_to_bootloader()`, see `stm32u073-usb-cdc` / `stm32-dfu-flash` skills
- `send <msg>` → builds a `Packet` (owner APP, dir OUTGOING) and enqueues on `pTxBuf`
- `ping` → enqueues a PING packet on `pTxBuf` for the network layer

**Output discipline:**
- `print()` → `cdc_print_str()` — returns false if USB not configured, otherwise busy-waits each 64-byte chunk
- Lines should end with `\n` (no `\r\n` needed; host tools handle both)
- Tools that parse output (lora_monitor.py, pytest suite) grep for fixed prefixes — `[RX]`, `[BEACON]`, `Solar:`, `Charge:`. **Don't change these prefixes** without updating the test suite.

## Examples
- terminal.c:44 `terminal()` — entry, buffer copy, parse, dispatch
- terminal.c `get_commands` `"solar"` branch — the canonical multi-field `get` layout
- terminal.c:215 `jump_to_bootloader` — DFU reset sequence
- firmware/app/lora/test/ — pytest harness that consumes these exact output prefixes

## Anti-Patterns
- **Never** print prompt characters (`>`, `$`) — automation tools do line-matching.
- Don't change existing output prefixes (`[RX]`, `[BEACON]`, `Solar:`) — breaks `lora_monitor.py` and the test suite.
- Don't accept more than 127 bytes of input — `MAX_ARGV` truncates silently; terminal emulators with big paste buffers can flood.
- Don't split a single field across `print()` calls — causes visible stutter and can interleave with async beacon prints.
- Don't call `writeFlash` on every `set` — use the explicit `set flash` two-step. Flash wear is real.
- Don't forget Linux-side `stty -F /dev/ttyACM… 115200 raw -echo` before automating — cooked-mode echo feeds output back as input (see `linux-serial-hygiene`).
