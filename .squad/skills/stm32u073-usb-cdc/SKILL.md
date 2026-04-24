---
name: "stm32u073-usb-cdc"
description: "STM32U073 USB DRD FS CDC ACM driver — bare-metal PMA/BDT, polled, coexists with ST DFU bootloader."
domain: "stm32-peripherals"
confidence: "medium"
source: "earned"
---

## Context
Single USB peripheral used as a CDC-ACM virtual COM port. Driver `firmware/src/driver/usb_cdc.c/h`, exercised by `app/usb_cdc/main.c`, and used by `system/terminal.c` as the command interface. No HAL — direct PMA/BDT/CHEP register access.

## Patterns

**Pins / clocks:**
- PA11 = USB_DM, PA12 = USB_DP, **AF10**
- USB clock must be HSI48 + CRS (auto-trimmed from USB SOF). Enable via `RCC_CRRCR |= 1` (HSI48 on), wait for HSI48RDY, then `RCC_CCIPR = 3<<26` (USB clock = HSI48).
- `CRS_CFGR` SYNCSRC = 2 (USB), `CRS_CR |= (1<<5)|(1<<6)` (autotrim + CRS enable).
- `PWR_CR2 |= (1<<10)` — USV bit, required on U0 to power the USB transceiver.
- RCC APB1 bits enabled: `(1<<28)` PWREN, `(1<<16)` CRSEN, `(1<<13)` USBEN.

**Endpoint layout (CDC ACM, 4 EPs):**
| EP | Type | Dir | Buf | Use |
|---|---|---|---|---|
| 0 | Control   | IN/OUT | 64/64 | enumeration, line coding |
| 1 | Bulk      | IN  (0x81) | 64 | CDC data → host (`cdc_print_str`) |
| 2 | Bulk      | OUT (0x02) | 64 | CDC data ← host (`rx_callback`) |
| 3 | Interrupt | IN  (0x83) | 8  | CDC notification (unused) |

Descriptors are hard-coded byte arrays in usb_cdc.c top. VID/PID = `0x0483 / 0x5740` (ST CDC).

**Init sequence (`usb_cdc_init()` usb_cdc.c:220+):**
1. GPIO PA11/12 AF10, high speed
2. Enable PWR + CRS + USB on APB1
3. Bring up HSI48 + CRS sync-to-SOF
4. `PWR_CR2.USV = 1`
5. `USB_CNTR = USBRST` → delay → `USB_CNTR = 0` → delay (standard release sequence)
6. Zero the 1 KB PMA (`PMA_BASE`)
7. `USB_ISTR = 0`, enable DP pull-up (`USB_BCDR |= DPPU`)
8. Unmask interrupts in CNTR (CTRM/RESETM/SUSPM/WKUPM/ERRM/ESOFM/SOFM) but **never configure NVIC** — polled only
9. `USB_DADDR = 0x80` (enable function, addr 0)

**Polling:** `usb_cdc_poll()` reads `USB_ISTR`, dispatches RESET / CTR events, services per-EP CTR_RX/CTR_TX bits. Main loop must call it every iteration.

**Baud rate:** host sets `line_coding` (default 115200-8N1) — the MCU ignores it. **USB CDC throughput is independent of baud**; the 115200 figure in `dfu_flash.sh` and `stty` calls is for the Linux TTY driver only.

**TX flow (`cdc_print_str`):** chunks string into 64-byte EP1 packets, busy-waits `ep1_tx_busy` (cleared by EP1 CTR_TX), up to 100000 nop iterations per chunk. Returns false if USB not configured or TX stalls.

**Buffer sizing:** RX callback receives at most 64 bytes/packet. Terminal's `MAX_ARGV = 128` — larger lines get truncated. Host-side tools use 64-byte chunks (see `linux-serial-hygiene` skill).

**DFU coexistence:** System memory bootloader lives at `0x1FFF0000`. `terminal.c jump_to_bootloader()` (invoked by `dfu` command):
1. Print "Entering DFU…", delay (flush USB)
2. `USB_BCDR &= ~DPPU` (drop pull-up → host sees disconnect)
3. `USB_CNTR = FRES|PDWN` (power down USB)
4. Disable SysTick, disable all NVIC IRQs, clear ICPR
5. `SYSCFG.MEM_MODE = 01` (map system memory to 0x00000000)
6. Read SP+reset vector from `0x1FFF0000`, `msr msp` + bxlr → enumerates as VID 0483 PID df11
Host then uses `dfu-util -a 0 -s 0x08000000:leave -D` (see `stm32-dfu-flash` skill).

## Examples
- usb_cdc.c:220 `usb_cdc_init()` — full clock + peripheral bring-up
- usb_cdc.c:240+ `usb_cdc_poll()` + `handle_setup()` — enumeration state machine
- usb_cdc.c `cdc_print_str()` — 64-byte chunked TX with busy-wait
- terminal.c `jump_to_bootloader()` — clean reset-to-DFU jump

## Anti-Patterns
- **Never** forget `PWR_CR2.USV = 1` on U0 — USB will enumerate but all transfers fail silently.
- Don't try to use HSE/PLL without CRS — HSI48 ±3% is outside USB spec; CRS autotrim fixes it from SOF.
- Don't call `cdc_print_str()` before `usb_cdc_is_configured()` returns true — it returns false but wastes main-loop time.
- Don't skip the USB shutdown (DPPU clear + FRES+PDWN) before jumping to DFU — the host keeps the old enumeration and the ST bootloader never appears.
- Don't ship with NVIC USB IRQ enabled — breaks the polled model and races `usb_cdc_poll` from the main loop.
- Don't assume a baud rate for USB CDC — `stty 115200` is Linux-side only; MCU throughput is USB-FS bulk (theoretical ~1 MB/s, here ~10 KB/s because of busy-wait TX).
