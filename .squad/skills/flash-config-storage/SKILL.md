---
name: "flash-config-storage"
description: "STM32U073 flash config storage — last page, 2 KB page size, 64-bit doubleword programming, erase-before-write."
domain: "firmware-storage"
confidence: "high"
source: "earned"
---

## Context
Persistent device settings (frequency, data rate, TX power, MAC address) are stored in the **last 2 KB page of the 128 KB flash** (page 63, `0x0801F800`). Driver: `firmware/src/system/flash_config.c/h`. Read on boot by `radio_init()`, written on demand via `set flash` terminal command.

> **Flash size.** STM32U073**CB**T6 = 128 KB flash / 40 KB SRAM, confirmed by `firmware/linker/stm32u073cb.ld` (`FLASH … LENGTH = 128K`). Last page base = `0x08000000 + 128K - 2K = 0x0801F800`. If a different flash size variant is ever fitted, `CONFIG_PAGE_ADDR` and `CONFIG_PAGE_NUM` must move.

## Patterns

**Layout constants (flash_config.c:15):**
- `CONFIG_PAGE_ADDR = 0x0801F800` — base of last 2 KB page
- `CONFIG_PAGE_NUM  = 63`         — used in `FLASH_CR.PNB` field
- Page size: **2 KB** on STM32U0 series (not 1 KB like older STM32L0).
- Programming granularity: **64-bit doubleword** — mandatory on U0. A partial write stalls `FLASH_SR.BSY` forever.

**Unlock sequence (`flash_unlock()` L20):**
```
if (FLASH_CR & FLASH_CR_LOCK) {
    FLASH_KEYR = FLASH_KEY1;   // 0x45670123
    FLASH_KEYR = FLASH_KEY2;   // 0xCDEF89AB
}
```
Wrong order or wrong keys sets `SR.PGSERR` and locks the controller until reset.

**Erase page (`writeFlash()` L32):**
```
FLASH_CR = FLASH_CR_PER | (CONFIG_PAGE_NUM << 3);  // PNB at bit 3
FLASH_CR |= FLASH_CR_STRT;
while (FLASH_SR & FLASH_SR_BSY);
FLASH_CR &= ~FLASH_CR_PER;
```
Erased flash reads `0xFFFFFFFFFFFFFFFF`. `readFlash()` treats `0xFFFFFFFF` as "unwritten" and returns `0xFF`.

**Program doublewords (`writeFlash()` L44):**
- `FLASH_CR |= FLASH_CR_PG`
- For each byte of `deviceData_t`, write at offset `i*8`:
  - low word = the byte (zero-extended in lower 8 bits of 32)
  - high word = `0xFFFFFFFF` (unused)
  - poll `BSY`, clear `SR.EOP`
- Clear `PG` at the end, relock.

This is **extremely wasteful** (1 byte per 8 bytes of flash), but it's simple and the config is tiny (~7 fields). A `deviceData_t` costs ~56 bytes of flash.

**Read (`readFlash()` L60):**
```
uint32_t val = *(volatile uint32_t *)(CONFIG_PAGE_ADDR + addr*8);
*read_data = (val == 0xFFFFFFFF) ? 0xFF : (val & 0xFF);
```
`addrEnum` values index into the 8-byte slots. Current keys: `DEVICE_ID`, `RADIO_FREQ1..4`, `RADIO_DR`, `RADIO_TX_PWR`.

**Wear:** Every `writeFlash()` erases the whole page. STM32U0 spec is ~10k erase cycles per page. A user hammering `set flash` 10 000 times will brick the config slot. Acceptable for field config; NOT acceptable for logging.

**Atomicity:** There is **no** atomicity — a power loss mid-erase leaves the page all-FF (all defaults); mid-program leaves a partially written page (some fields default, others new). `radio_init()` validates frequency is in {444/868/870 MHz} and falls back to 868 MHz if read is junk.

## Examples
- flash_config.c:32 `writeFlash()` — erase + doubleword program
- flash_config.c:60 `readFlash()` — single-byte read with 0xFF detection
- sx1262.c `radio_init()` L300+ — reads freq/DR/power + validates with fallback defaults
- terminal.c `set_commands(... "flash" ...)` — user-triggered write

## Anti-Patterns
- **Never** program less than a full doubleword — BSY will hang. The driver works around this by always writing 8 bytes (lo = payload, hi = 0xFFFFFFFF) which counts as a full doubleword.
- **Never** write twice to the same doubleword without erasing — flash programming can only clear bits (1→0). The driver always erases the whole page first.
- Don't call `writeFlash()` from a hot path — full page erase is ~20–40 ms and blocks interrupts via `while (BSY)`.
- Don't forget `flash_lock()` on exit paths — a left-open unlock combined with a wild write in other code can corrupt any flash byte.
- Don't assume page size = 1 KB (STM32L0 convention) — STM32U0 is **2 KB**. PNB values and offsets differ.
- Don't store more than a few bytes this way — each byte costs 8 bytes of flash. For larger data, use a proper log-structured store or external SPI flash (PB5/PB10/PB11/PB12 footprint exists).
