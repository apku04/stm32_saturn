# Test Results — Flash Approved Firmware (commit 0483139)

**Date:** 2025-07-23  
**Tester:** Brostin  
**Firmware:** commit 0483139 `fix(bat_mv): use INA219 bus voltage as battery proxy`  
**Build:** 22264 bytes text, 108 data, 1932 bss  

## Hardware

| | Board 1 | Board 2 |
|---|---|---|
| USB Bus | Bus 001 (port 1-2) | Bus 003 (port 3-2) |
| MAC Address | unknown | 33 |
| USB S/N | 0001 | 0001 |
| LiPo | reportedly yes | no |
| Status | **UNAVAILABLE** (USB dead) | operational |

## Results Table

| Test | Board 1 | Board 2 |
|------|---------|---------|
| Build | PASS (22264B) | PASS (22264B) |
| DFU flash | N/A — USB dead | **PASS** |
| Boot/USB CDC | N/A | **PASS** (ttyACM0 after flash) |
| I2C scan 0x40 | N/A | **NOT FOUND** — INA219 absent |
| INA219 bus_mv | N/A | 0 mV (no device) |
| bat_mv in beacon | N/A | 0 mV (expected, no INA219) |
| Battery ADC | N/A | 0 mV raw=0 (ADC stubbed) |
| Beacon TX | N/A | not observable (single board) |
| Beacon RX (target-to-target) | N/A | SKIPPED (need 2 boards) |
| Pytest single-device | N/A | **32/32 PASS** |
| Pytest communication | N/A | 3/3 SKIPPED (need 2 boards) |

## Detailed Findings

### Board 1 — USB Failure
- Board 1 was present on Bus 001 Device 035 as CDC (0483:5740) but was **completely unresponsive** to serial commands (no version response, no passive output)
- USB port reset attempt caused permanent disconnect: `device not accepting address, error -71`, `unable to enumerate USB device`
- Board needs physical USB cable replug to recover
- Was never flashed with new firmware

### Board 2 — Full Test
- Flashed commit 0483139 via DFU successfully (22372 bytes downloaded)
- Boot and USB CDC enumeration: OK
- Terminal commands all functional: version, get/set frequency, data_rate, tx_power, mac_address, flash, routing
- **I2C2 scan completed cleanly** — scans 0x08–0x77, prints "Done", no devices found
- **INA219 not at 0x40** — same as previous test run (commit 910cb9c), INA219 (U10) likely not populated on either board
- **ADC stub verified:** `get battery` → `Battery: 0 mV (raw: 0)` — broken ADC correctly stubbed
- **INA219 graceful failure verified:** `get solar` → `Solar: 0 mV` — returns 0 without crash or hang
- Charge status: "Charging" (USB power)

### Radio Config (Board 2)
- Frequency: 868000000 Hz
- Data Rate: 7 (SF7)
- TX Power: 14 dBm

### Pytest Results (Board 2 only)
```
32 passed, 3 skipped in 43.70s
```
- All 32 single-device tests PASS (terminal, radio config, error handling, flash, DFU, battery, reset)
- 3 communication tests SKIPPED (require 2 boards)

### Identical Serial Numbers
Both boards enumerate with USB S/N `0001`. Different MAC addresses are set via `set mac_address` in firmware flash, but the USB serial numbers should ideally differ for reliable multi-board identification.

## Verdict

**Firmware commit 0483139 — PASS (single-board)**

- Firmware builds clean, flashes via DFU, boots, USB CDC functional
- INA219 code path exercised and returns 0 gracefully (no INA219 hardware)
- ADC stub works as designed
- All 32 single-device pytest tests pass
- Cannot verify INA219 actual readings or beacon RX without:
  1. Board 1 recovery (physical USB replug)
  2. INA219 (U10) physically populated on at least one board

## Action Items

1. **Board 1 recovery:** Physically replug USB cable on Bus 001 port
2. **INA219 population:** Confirm if U10 is soldered on either board — neither shows 0x40 on I2C scan
3. **Re-test with 2 boards:** Flash both, run full pytest including communication tests
4. **USB S/N differentiation:** Consider unique S/N per board for automated test routing
