---
name: "linux-serial-hygiene"
description: "Always put STM32 USB CDC TTYs into raw mode before cat/echo — avoids firmware seeing echoed garbage"
domain: "host-debug"
confidence: "high"
source: "earned"
---

## Context
When interacting with an STM32 USB CDC ACM port (`/dev/ttyACM*`) from a Linux shell using raw `cat`, `echo`, or redirection, the TTY defaults to **cooked mode**. Cooked mode echoes every incoming byte back out the same TTY, which the MCU then receives as input. The firmware sees this as spurious commands.

Real case on this project: the receiver's terminal spammed `unknown cmd: T` because beacon packets printed a `T`-prefixed line, the kernel echoed it back into the MCU's CDC RX, and the command parser tried to execute it.

`tools/lora_monitor.py` is fine — pyserial forces raw mode when it opens the port. This gotcha only bites raw shell use.

## Patterns

### Always-first incantation
```bash
stty -F /dev/ttyACM0 115200 raw -echo
```
Run this **before** any `cat`, `echo`, or `>` redirection on the port. It is idempotent.

### Read-only monitoring
```bash
stty -F /dev/ttyACM0 115200 raw -echo
cat /dev/ttyACM0
# or with timeout:
timeout 15 cat /dev/ttyACM0
```

### Send a single command (no echo back into MCU)
```bash
stty -F /dev/ttyACM0 115200 raw -echo
printf 'dfu\r\n' > /dev/ttyACM0
```

### Bidirectional terminal
Prefer a real terminal program — they handle raw mode, line endings, and local echo correctly:
```bash
picocom -b 115200 /dev/ttyACM0      # Ctrl-A Ctrl-X to exit
minicom -b 115200 -D /dev/ttyACM0   # Ctrl-A X to exit
```

### Distinguishing two boards (both ACM)
```bash
ls -l /dev/serial/by-id/            # stable, board-identifying symlinks — use these
dmesg | tail -20                    # shows which ACMn just enumerated
udevadm info -q property -n /dev/ttyACM0 | grep -E 'ID_SERIAL|ID_VENDOR'
```
Prefer `/dev/serial/by-id/usb-STMicroelectronics_*` in scripts so ACM0/ACM1 swaps on re-plug don't break them.

## Anti-Patterns
- `cat /dev/ttyACM0` without `stty ... raw -echo` first → cooked-mode echo loop, firmware sees garbage commands.
- Two concurrent readers on the same TTY (`cat` in one terminal, `minicom` in another) → bytes split unpredictably between them; each sees partial lines. Kill one first (`fuser /dev/ttyACM0` to find holders).
- Hard-coding `/dev/ttyACM0` when two boards are connected — enumeration order is not stable across reboots or re-plugs.
- Using `echo foo > /dev/ttyACM0` in cooked mode — adds kernel-translated newlines and may echo.
