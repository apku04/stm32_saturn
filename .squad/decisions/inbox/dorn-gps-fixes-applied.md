# Dorn — GPS Driver Fix + Beacon Integration

**Date:** 2026-07-25  
**Status:** Implemented, build clean  
**Authority:** Dorn (STM32 Expert)  
**Files changed:** `firmware/src/driver/gps.c`, `firmware/include/stm32u0.h`, `firmware/app/lora/main.c`

## Summary of Changes

### Fix 1 — SENSE_LDO_EN Power Sequencing (CRITICAL)
- PA15 configured as push-pull output, driven HIGH in `gps_init()` before USART2 setup
- 100ms delay for LDO + GPS module power-up stabilisation
- Without this, GPS module has no power and produces no NMEA data

### Fix 2 — AF4 → AF7 (CRITICAL)
- PA2/PA3 alternate function changed from AF4 to AF7 (RM0503 Table 20)
- AF4 on PA2/PA3 routes to LPUART2/USART3, not USART2 — UART was electrically dead
- Now uses hw_pins.h macros (GPS_RX_PIN, GPS_TX_PIN) instead of magic numbers

### Fix 3 — Non-blocking GPS Poll
- `gps_poll()` now drains from ISR ring buffer, returns immediately if empty
- No blocking waits in GPS path — USB CDC remains fully functional

### Fix 4 — USART2 RXNE Interrupt + Ring Buffer
- 64-byte ring buffer captures USART2 bytes in ISR context
- ISR clears ORE (overrun) flag to prevent USART stall
- NVIC enabled for USART2 (IRQ 28)
- Vector table extended to 45 entries to cover USART2 slot
- Prevents NMEA corruption during SX1262 blocking radio operations

### Fix 5 — GPS in Beacon Payload
- Beacon v5: 18 bytes total (was 9)
- pkt.data[9..12] = lat_udeg (int32_t, LE, micro-degrees)
- pkt.data[13..16] = lon_udeg (int32_t, LE, micro-degrees)
- pkt.data[17] = fix_valid (0 or 1)
- Sends zeros with fix_valid=0 until GPS acquires fix

### Fix 6 — USART2 Defs Moved to stm32u0.h
- Removed local USART2 register definitions from gps.c
- Added canonical definitions in stm32u0.h with all needed bit masks

## Build Status
- ✅ Clean build (zero errors, zero compiler warnings)
- Text: 29420 bytes, Data: 108 bytes, BSS: 2240 bytes
- Growth: ~7KB from extended vector table + ISR + GPS payload

## Open Questions
1. **Beacon receiver parsing:** `app_incoming()` in main.c does not yet decode the GPS fields from received beacons (v5 format). Needs a v5 decoder branch.
2. **lora_monitor.py:** Python monitor tool may need update to parse GPS fields from v5 beacon.
3. **GPS module validation:** Needs hardware test with actual GPS module connected to H3 header.
4. **gps_set_af() helper:** Still uses hardcoded pin numbers (2*4, 3*4) — could use hw_pins.h macros for consistency. Low priority.
