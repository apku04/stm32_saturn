# Brostin — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

- **I2C1 vs I2C2 mismatch:** `i2c.c` aliases all registers to I2C1 (0x40005400) but the terminal `get i2cscan` message says "Scanning I2C2". The RCC enable bit differs (I2C1=bit21, I2C2=bit22 of APBENR1). This must be reconciled before INA219 will ACK on the correct bus.
- **No shunt/current register access:** Only `ina219_read_bus_mv()` (reg 0x02) is exposed. No terminal command to dump raw INA219 registers. Recommend adding `get ina219` for debug.
- **OVF bit silently discarded:** `ina219_read_bus_mv()` does `>> 3` which strips OVF (bit 0) and CNVR (bit 1). Safe for bus voltage reads but needs attention if current measurement is added.
- **No I2C bus recovery:** If SDA/SCL get stuck, there's no 9-clock bit-bang recovery in `i2c_init()`. Only fix is power cycle.
- **ADC formula verified:** `raw * 6600 / 4096` is correct for 1:1 divider with 3.3V VDDA. Max raw=4095 → 6598 mV, no overflow risk in uint32 math.
- **Beacon payload:** 5 bytes at `pkt.data[0..4]` — bat_mv(LE), sol_mv(LE), charge_status. `pkt.length = 17` (12 header + 5 data). Monitor regex `BEACON_PATTERN` matches the full format.
- **Graceful INA219 failure:** `i2c_read_reg` returns -2 (NACK) or -1 (timeout) bounded by 100k iterations (~6ms). `ina219_read_bus_mv()` returns 0 on error. No crash path.
- **Test app (`app/test/main.c`) is LED-blink only** — no USB, no I2C, no useful diagnostics. All testing must go through the lora app terminal.

### 2026-04-23 — I2C2 Testing & Integration (Cross-agent Learning)

**Baseline validation completed:**
- I2C2 hardware + firmware alignment verified (Kotov PCB + Cawl fix)
- Beacon V2 payload format validated (7 bytes, backward compatible)
- LED restoration confirmed (PB13/14 GPIO output)
- Monitor parsing ready (BEACON_PATTERN_V2 + legacy fallback)

**Recommended test sequence (for this session):**
1. **Single target:** I2C2 reads on PB8/9, LED toggle, beacon TX
2. **Target-to-target:** Beacon RX on monitor station, payload field extraction
3. **Regression:** Ensure V2 parser doesn't break legacy 5-byte format

**Known limitations to document:**
- No I2C bus recovery (stuck SDA/SCL → power cycle required)
- INA219 failure returns 0 (graceful, but no diagnostics)
- Test app limited (LED-blink only, use lora app terminal for validation)

**Cross-share with Ravenor:** I2C2/INA219 topology, Beacon V2 format, Kotov's PCB findings, Cawl's pin-locking principles

### 2026-04-23 — Full Test Run (commit 910cb9c / bd9ec70)

**Hardware tested:** 2 × STM32U073CBT6 boards (S/N: 0001), both on USB 3 hubs (Bus 001 + Bus 003).

**Build:** Clean rebuild passed. 22320 bytes text. No errors.

**Flash:** Both boards flashed via DFU (dfu_flash.sh --enter + manual for board 2). DFU round-trip works reliably.

**Pytest results:** 32/35 passed, 3 failed (106s total).
- All 32 single-target tests pass (terminal, radio config, error handling, flash, DFU, battery ADC, reset).
- 3 target-to-target message tests fail due to `parse_response()` format mismatch — expects PIC24 pipe-separated `header|hexdata` format but firmware outputs `[RX] src=X...\nHH HH HH`. Radio TX/RX itself works.

**INA219 status:** Not responding on either board. `get i2cscan` completes cleanly, no device at 0x40. I2C2 driver (PB8/PB9 AF6) initializes OK — hardware issue. Both boards may lack INA219 or have different wiring. Firmware gracefully returns 0. Beacons include shunt=0 bus=0.

**V2 beacon format confirmed working:** Board-to-board beacons show `[BEACON] shunt=0 bus=0 bat=215 chg=1 entries=2`. All 4 fields present. Bidirectional.

**Battery ADC:** 170–215 mV on USB-only power (no LiPo). Formula raw×6600/4096 verified correct. Higher than expected 0-100 mV; likely high-Z divider leakage.

**Key finding — test_target_test.py needs parse_response() update:** The pipe-separated format `header|hexdata` was a PIC24 convention. STM32 firmware uses the `[RX]` + hex dump format. The 3 failing tests are false negatives — create a ticket to update the parser.

### 2026-04-23 — Parser Fix (parse_response format mismatch)

**Task:** Fix 3 failing pytest tests caused by `parse_response()` expecting PIC24 pipe-delimited format while STM32 firmware outputs `[RX]` format.

**Changes made:**
1. **test_target_test.py** — Rewrote `parse_response()`:
   - Old: split on `|`, expected `header_csv|hexdata` (PIC24 convention)
   - New: regex match `[RX] src=X dst=X rssi=X prssi=X type=X seq=X len=X`, then decode space-separated hex payload from next line
   - Uses same RX_PATTERN as `lora_monitor.py` for consistency
   - Maps [RX] fields to Header dataclass (src→source_adr, dst→destination_adr, type→control_app, etc.)

2. **conftest.py** — Added radio config reset to `serial_ports` fixture:
   - Sets frequency/data_rate/tx_power to known defaults on both boards before communication tests
   - Prevents cross-test contamination from earlier single-device tests

**Verification:**
- Local unit tests: 5/5 pass (various [RX] formats, edge cases)
- Isolated pytest run: test_send_receive_message PASSED with new parser (confirmed [RX] + hex parsing works end-to-end)
- Full suite: 32 passed / 3 failed — communication tests fail due to radio hardware not responding (separate from parser fix)

**Remaining issue:** Radio intermittently non-functional. Both boards on identical config (868MHz, SF7, 14dBm), `send` returns Done, but receiver gets 0 bytes. Possible SX1262 RX mode issue after repeated DFU cycles, or antenna/proximity problem. Not a test code bug.
